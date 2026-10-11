#include "coils.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"

// COIL1..COIL16 -> ESP32-S3 GPIO, in order. The state vector is MSB-first:
// bit 15 = COIL1 (COIL_PINS[0]) ... bit 0 = COIL16 (COIL_PINS[15]).
static const uint8_t COIL_PINS[NUM_COILS] = {9, 11, 10, 12, 14, 13, 1, 6, 7, 8, 38, 39, 40, 41, 42, 2};

// Per-coil register info for set_coil_state(), filled in by coils_begin().
// GPIO 0-31 live in bank 0 (GPIO.out_w1ts/out_w1tc); GPIO 32+ live in bank 1
// (GPIO.out1_w1ts.val/out1_w1tc.val). Precomputing which bank each coil's pin
// falls in, and its bit mask within that bank, means set_coil_state() can
// turn a 16-bit pattern into 4 register writes instead of 16 digitalWrite()
// calls, so every coil switches in the same instant rather than one at a time.
static DRAM_ATTR uint32_t coil_mask[NUM_COILS];
static DRAM_ATTR bool coil_is_bank1[NUM_COILS];

// Also called from the close timer's interrupt, hence IRAM.
static void IRAM_ATTR set_coil_state(uint16_t bits) {
  uint16_t bit = 0x8000;  // bit 15 -> COIL_PINS[0] (COIL1)
  uint32_t bank0_set = 0, bank0_clear = 0;
  uint32_t bank1_set = 0, bank1_clear = 0;

  for (int i = 0; i < NUM_COILS; i++) {
    bool on = (bits & bit) != 0;
    if (coil_is_bank1[i]) {
      if (on) bank1_set |= coil_mask[i]; else bank1_clear |= coil_mask[i];
    } else {
      if (on) bank0_set |= coil_mask[i]; else bank0_clear |= coil_mask[i];
    }
    bit >>= 1;
  }

#ifdef CONSOLE_PREVIEW_DRY_RUN
  // Dry run (Ticket 7.2): masks computed as usual so the timing is the same,
  // but no valve ever opens, in any mode.
  (void)bank0_set; (void)bank0_clear; (void)bank1_set; (void)bank1_clear;
#else
  GPIO.out_w1ts = bank0_set;
  GPIO.out_w1tc = bank0_clear;
  GPIO.out1_w1ts.val = bank1_set;
  GPIO.out1_w1tc.val = bank1_clear;
#endif
}

// --- Burst close timer -----------------------------------------------------
//
// A free-running 1 MHz GPTimer whose alarm closes the coils at the end of a
// burst, from an interrupt on the print engine's core rather than the
// esp_timer task (which runs on core 0 with WiFi).
//
// coils_apply() sets the coils and moves the alarm together with interrupts
// off on this core, so the interrupt can't land in between. What it can do
// is fire for the *old* alarm just before coils_apply() runs and only get
// handled just after, by which time it would close the new burst early. So
// the handler checks the timer's count against the alarm currently wanted
// (close_at_count): a stale alarm is still short of it and does nothing.

static gptimer_handle_t close_timer = nullptr;
static portMUX_TYPE coil_mux = portMUX_INITIALIZER_UNLOCKED;
static const uint64_t NO_CLOSE = UINT64_MAX;
static volatile uint64_t close_at_count = NO_CLOSE;

static bool IRAM_ATTR on_close_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t *e, void *) {
  portENTER_CRITICAL_ISR(&coil_mux);
  if (e->count_value >= close_at_count) {
    set_coil_state(0x0000);
    close_at_count = NO_CLOSE;
  }
  portEXIT_CRITICAL_ISR(&coil_mux);
  return false;
}

void coils_begin() {
  for (int i = 0; i < NUM_COILS; i++) {
    uint8_t pin = COIL_PINS[i];
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    coil_is_bank1[i] = pin >= 32;
    coil_mask[i] = 1UL << (pin % 32);
  }

  gptimer_config_t cfg = {};
  cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  cfg.direction = GPTIMER_COUNT_UP;
  cfg.resolution_hz = 1000000;
  ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &close_timer));
  gptimer_event_callbacks_t cbs = {};
  cbs.on_alarm = on_close_alarm;
  ESP_ERROR_CHECK(gptimer_register_event_callbacks(close_timer, &cbs, nullptr));
  ESP_ERROR_CHECK(gptimer_enable(close_timer));
  ESP_ERROR_CHECK(gptimer_start(close_timer));
  log_i("coils: burst timer interrupt on core %d", xPortGetCoreID());
}

void coils_apply(const drip::CoilCommand &cmd) {
  bool timed = cmd.bits != 0 && cmd.close_at != drip::NEVER;
  portENTER_CRITICAL(&coil_mux);
  set_coil_state(cmd.bits);
  if (timed) {
    drip::Micros in_us = cmd.close_at - esp_timer_get_time();
    if (in_us < 10) in_us = 10;  // never an alarm already in the past
    uint64_t count = 0;
    gptimer_get_raw_count(close_timer, &count);
    close_at_count = count + (uint64_t)in_us;
    gptimer_alarm_config_t alarm = {};
    alarm.alarm_count = close_at_count;
    gptimer_set_alarm_action(close_timer, &alarm);
  } else {
    close_at_count = NO_CLOSE;
    gptimer_set_alarm_action(close_timer, nullptr);
  }
  portEXIT_CRITICAL(&coil_mux);
}
