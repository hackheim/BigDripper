#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "esp_timer.h"
#include "soc/gpio_struct.h"
#include "wifi_text_input.h"
#include "params.h"
#include "font.h"
#include "text_queue.h"
#include "mode.h"
#include "test_pattern.h"
#include "console_preview.h"

// COIL1..COIL16 -> ESP32-S3 GPIO, in order. The state vector is MSB-first:
// bit 15 = COIL1 (coils[0]) ... bit 0 = COIL16 (coils[15]).
const int NUM_COILS = 16;
const int NUM_NEOPIXEL_STRIPS = 4;
const uint8_t coils[NUM_COILS] = {9, 11, 10, 12, 14, 13, 1, 6, 7, 8, 38, 39, 40, 41, 42, 2};
const uint8_t neopixels[NUM_NEOPIXEL_STRIPS] = {15, 16, 17, 18};

// Per-coil register info for set_coil_state(), filled in by coils_begin().
// GPIO 0-31 live in bank 0 (GPIO.out_w1ts/out_w1tc); GPIO 32+ live in bank 1
// (GPIO.out1_w1ts.val/out1_w1tc.val). Precomputing which bank each coil's pin
// falls in, and its bit mask within that bank, means set_coil_state() can
// turn a 16-bit pattern into 4 register writes instead of 16 digitalWrite()
// calls, so every coil switches in the same instant rather than one at a time.
static uint32_t coil_mask[NUM_COILS];
static bool coil_is_bank1[NUM_COILS];

void coils_begin() {
  for (int i = 0; i < NUM_COILS; i++) {
    uint8_t pin = coils[i];
    if (pin < 32) {
      coil_is_bank1[i] = false;
      coil_mask[i] = 1UL << pin;
    } else {
      coil_is_bank1[i] = true;
      coil_mask[i] = 1UL << (pin - 32);
    }
  }
}

// Strip 1 (first neopixel GPIO) pulses red; strip 2 (second GPIO) runs a
// yellow Cylon eye. The other two pins are reserved and aren't driven yet.
const int GLOW_STRIP_PIXELS = 48;
const int CYLON_STRIP_PIXELS = 28;
Adafruit_NeoPixel glow_strip(GLOW_STRIP_PIXELS, neopixels[0], NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel cylon_strip(CYLON_STRIP_PIXELS, neopixels[1], NEO_GRB + NEO_KHZ800);

// Encoder
const int NUM_ENCODER_CHANNELS = 2;
const uint8_t encoder[NUM_ENCODER_CHANNELS] = {4,5};

// --- Interrupt-driven quadrature decoder -----------------------------------
//
// State is (A << 1) | B. Indexing this table with (previous << 2) | current
// gives the step: +1 clockwise, -1 counter-clockwise, and 0 for the six
// impossible transitions (both channels changing at once), which is what
// contact bounce and missed steps look like. Rejecting those instead of
// guessing is what keeps the count from drifting on a cheap encoder.
//
// DRAM_ATTR keeps the table out of flash. An IRAM ISR must not touch flash,
// because the cache is disabled during flash writes and the fetch would fault.
static const DRAM_ATTR int8_t QUAD_TABLE[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};

// Same reason: plain globals live in DRAM, so cache the pins here rather than
// reading the const encoder[] array (.rodata, i.e. flash) from the ISR.
static uint8_t enc_pin_a = 0;
static uint8_t enc_pin_b = 0;

static volatile int32_t encoder_raw = 0;   // quarter-steps, not detents
static volatile uint8_t encoder_prev = 0;
static portMUX_TYPE encoder_mux = portMUX_INITIALIZER_UNLOCKED;

// Set by the encoder task before it attaches the interrupt, so the ISR can wake
// it. A plain global lives in .bss (DRAM), which an IRAM ISR may read.
static volatile TaskHandle_t encoder_task_handle = NULL;

// Set by valve_task (via print_engine_run()) so the same ISR can also wake the
// print engine immediately on encoder movement, instead of it polling on a
// fixed tick. Independent of encoder_task_handle's publish order — the ISR
// null-checks each handle on its own.
static volatile TaskHandle_t valve_task_handle = NULL;

// Most detented encoders run through a full 4-state cycle per click, so shift
// by 2 to report detents. Set to 0 to count every edge instead.
static const int ENCODER_DETENT_SHIFT = 2;

static void ARDUINO_ISR_ATTR encoder_isr() {
  uint8_t state = (digitalRead(enc_pin_a) << 1) | digitalRead(enc_pin_b);

  portENTER_CRITICAL_ISR(&encoder_mux);
  encoder_raw += QUAD_TABLE[(encoder_prev << 2) | state];
  encoder_prev = state;
  portEXIT_CRITICAL_ISR(&encoder_mux);

  // Wake the encoder task and the valve task instead of having them poll. The
  // ISR stays short: it only counts, the tasks do anything expensive.
  BaseType_t higher_woken = pdFALSE;
  if (encoder_task_handle != NULL) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(encoder_task_handle, &woken);
    higher_woken |= woken;
  }
  if (valve_task_handle != NULL) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(valve_task_handle, &woken);
    higher_woken |= woken;
  }
  portYIELD_FROM_ISR(higher_woken);
}

// Safe to call from loop(): int32_t is not atomic against a concurrent ISR on
// the other core, so take the same lock the ISR uses.
int32_t encoder_position() {
  portENTER_CRITICAL(&encoder_mux);
  int32_t raw = encoder_raw;
  portEXIT_CRITICAL(&encoder_mux);

  // Arithmetic shift floors toward negative infinity. Dividing by 4 would
  // truncate toward zero and make the detent straddling zero twice as wide.
  return raw >> ENCODER_DETENT_SHIFT;
}

void encoder_zero() {
  portENTER_CRITICAL(&encoder_mux);
  encoder_raw = 0;
  portEXIT_CRITICAL(&encoder_mux);
}

void encoder_begin() {
  enc_pin_a = encoder[0];
  enc_pin_b = encoder[1];

  // Seed the previous state, otherwise the first edge is decoded against a
  // state the encoder was never in and can produce one bogus step.
  encoder_prev = (digitalRead(enc_pin_a) << 1) | digitalRead(enc_pin_b);

  // Both channels, both edges — a quadrature cycle has four transitions and
  // dropping any of them halves the resolution and breaks the state table.
  attachInterrupt(digitalPinToInterrupt(enc_pin_a), encoder_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(enc_pin_b), encoder_isr, CHANGE);
}
// ---------------------------------------------------------------------------


// --- Core assignment -------------------------------------------------------
//
// Arduino's loopTask is pinned to ARDUINO_RUNNING_CORE, which this board sets to
// 1 (see the board manifest's extra_flags). So core 1 is where setup()/loop()
// already run, and core 0 is otherwise idle in this sketch.
#define ENCODER_CORE 0
#define VALVE_CORE   1

// Defined below. This is a .cpp, so unlike an .ino there is no automatic
// prototype generation.
void test_GPIO_outputs();
void print_engine_run();
void coils_begin();

static void encoder_task(void *arg) {
  // Publish the handle before attaching, so the ISR never fires with it unset.
  encoder_task_handle = xTaskGetCurrentTaskHandle();

  // attachInterrupt MUST be called from this task, not from setup(). The first
  // attachInterrupt anywhere in the sketch calls gpio_install_isr_service(),
  // which allocates the shared GPIO interrupt on whatever core is executing at
  // that moment (see __attachInterruptFunctionalArg in esp32-hal-gpio.c).
  // Calling it from setup() would bind the ISR to core 1 no matter where this
  // task runs. Caveat: that service is shared, so every GPIO interrupt added
  // later also lands on this core.
  encoder_begin();

  log_i("encoder task + ISR on core %d", xPortGetCoreID());

  // Logging every single detent works fine for a slow test turn, but a fast
  // spin can notify this task faster than a UART write at 115200 baud can
  // drain (log_i's underlying uart_tx busy-waits for FIFO space), which was
  // starving IDLE0 on this core long enough to trip the task watchdog. Cap
  // how often we actually log; position tracking itself stays uncapped.
  const uint32_t LOG_INTERVAL_MS = 50;
  uint32_t last_log_ms = 0;

  int32_t last = encoder_position();
  for (;;) {
    // Block until the ISR reports an edge. No polling, so IDLE0 keeps running
    // and the task watchdog stays fed.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    int32_t pos = encoder_position();
    if (pos != last) {
      uint32_t now = millis();
      if (now - last_log_ms >= LOG_INTERVAL_MS) {
        log_i("encoder %+ld (%s) [core %d]",
              (long)pos, pos > last ? "CW" : "CCW", xPortGetCoreID());
        last_log_ms = now;
      }
      last = pos;
    }
  }
}

static void valve_task(void *arg) {
  log_i("valve task on core %d", xPortGetCoreID());

  // Blocking sequencer; delay() inside a task is vTaskDelay, so it yields the
  // core rather than spinning.
  print_engine_run();  // never returns

  vTaskDelete(NULL);
}

// Slow red breathe: a sine wave keeps the fade smooth at both ends, unlike a
// linear ramp which looks like it "hangs" near full brightness/off.
// --- Neopixel power budget -------------------------------------------------
//
// All strips share a 1 A supply, so brightness is capped by design rather
// than measured at runtime. WS2812B figures, on the conservative side: ~20 mA
// per colour channel at full (255) and ~1 mA per pixel just for being
// powered, even when dark.
static constexpr float NEOPIXEL_BUDGET_MA = 1000.0f;
static constexpr float NEOPIXEL_MARGIN_MA = 100.0f;   // headroom for the estimate being off
static constexpr float MA_PER_CHANNEL_FULL = 20.0f;
static constexpr float MA_IDLE_PER_PIXEL = 1.0f;

static constexpr float NEOPIXEL_IDLE_MA =
    (GLOW_STRIP_PIXELS + CYLON_STRIP_PIXELS) * MA_IDLE_PER_PIXEL;

// Cylon eye colour: a warm yellow (pure 255,255,0 looks greenish on WS2812s).
static constexpr uint8_t CYLON_R = 255;
static constexpr uint8_t CYLON_G = 170;
static constexpr uint8_t CYLON_B = 0;

// The trail fades by CYLON_DECAY every frame and the head adds at most one
// full pixel's worth of light per frame, so the total lit amount is bounded
// by the geometric series 1 / (1 - CYLON_DECAY) pixels at full colour.
static constexpr float CYLON_DECAY = 0.8f;
static constexpr float CYLON_MAX_MA =
    (1.0f / (1.0f - CYLON_DECAY)) *
    ((CYLON_R + CYLON_G + CYLON_B) / 255.0f) * MA_PER_CHANNEL_FULL;

// Whatever is left goes to the red glow. All 48 pixels light up at once at
// the top of the pulse, so this is the strip that actually needs capping.
static constexpr float GLOW_AVAILABLE_MA =
    NEOPIXEL_BUDGET_MA - NEOPIXEL_MARGIN_MA - NEOPIXEL_IDLE_MA - CYLON_MAX_MA;
static constexpr float GLOW_RED_MAX_F =
    GLOW_AVAILABLE_MA / GLOW_STRIP_PIXELS / MA_PER_CHANNEL_FULL * 255.0f;
static constexpr uint8_t GLOW_RED_MAX = GLOW_RED_MAX_F > 255.0f ? 255 : (uint8_t)GLOW_RED_MAX_F;

static_assert(GLOW_AVAILABLE_MA > 0, "neopixel budget exhausted before the glow strip");
static_assert(NEOPIXEL_IDLE_MA + CYLON_MAX_MA +
              GLOW_STRIP_PIXELS * (GLOW_RED_MAX / 255.0f) * MA_PER_CHANNEL_FULL
              <= NEOPIXEL_BUDGET_MA - NEOPIXEL_MARGIN_MA,
              "neopixel worst case exceeds the power budget");

static void neopixel_task(void *arg) {
  log_i("neopixel task on core %d: glow red max %u/255, worst case ~%d mA of %d mA budget",
        xPortGetCoreID(), GLOW_RED_MAX,
        (int)(NEOPIXEL_IDLE_MA + CYLON_MAX_MA +
              GLOW_STRIP_PIXELS * (GLOW_RED_MAX / 255.0f) * MA_PER_CHANNEL_FULL),
        (int)NEOPIXEL_BUDGET_MA);

  glow_strip.begin();
  glow_strip.show();  // all off
  cylon_strip.begin();
  cylon_strip.show();

  const uint32_t PULSE_PERIOD_MS = 4000;
  const uint32_t CYLON_SWEEP_MS = 1200;    // one end to the other
  const uint32_t UPDATE_INTERVAL_MS = 20;  // ~50 Hz, smooth without flooding the bus

  // Per-pixel brightness of the Cylon eye and its trail, 0..1.
  float cylon_level[CYLON_STRIP_PIXELS] = {0};

  for (;;) {
    uint32_t now = millis();

    // Red glow.
    uint32_t phase_ms = now % PULSE_PERIOD_MS;
    float phase = (2.0f * PI * phase_ms) / PULSE_PERIOD_MS;
    float brightness = (sinf(phase - PI / 2.0f) + 1.0f) / 2.0f;  // 0..1, starts at 0

    uint8_t red = (uint8_t)(brightness * GLOW_RED_MAX);
    uint32_t color = glow_strip.Color(red, 0, 0);
    for (int i = 0; i < GLOW_STRIP_PIXELS; i++) {
      glow_strip.setPixelColor(i, color);
    }

    // Cylon: the eye bounces end to end (triangle wave), leaving a trail
    // that fades by CYLON_DECAY each frame. Fading the existing trail rather
    // than drawing it relative to the head means it follows the eye
    // naturally through the turnaround at each end.
    uint32_t t = now % (2 * CYLON_SWEEP_MS);
    float sweep = (t < CYLON_SWEEP_MS) ? (float)t / CYLON_SWEEP_MS
                                       : 2.0f - (float)t / CYLON_SWEEP_MS;  // 0..1..0
    float head = sweep * (CYLON_STRIP_PIXELS - 1);

    for (int i = 0; i < CYLON_STRIP_PIXELS; i++) {
      cylon_level[i] *= CYLON_DECAY;
    }
    // Split the head across the two pixels it sits between, so it glides
    // instead of stepping. The two weights sum to 1, which is what the
    // CYLON_MAX_MA bound relies on.
    int head_i = (int)head;
    float frac = head - head_i;
    cylon_level[head_i] = max(cylon_level[head_i], 1.0f - frac);
    if (head_i + 1 < CYLON_STRIP_PIXELS) {
      cylon_level[head_i + 1] = max(cylon_level[head_i + 1], frac);
    }

    for (int i = 0; i < CYLON_STRIP_PIXELS; i++) {
      float l = cylon_level[i];
      cylon_strip.setPixelColor(i, cylon_strip.Color((uint8_t)(l * CYLON_R),
                                                     (uint8_t)(l * CYLON_G),
                                                     (uint8_t)(l * CYLON_B)));
    }

    glow_strip.show();
    cylon_strip.show();

    vTaskDelay(pdMS_TO_TICKS(UPDATE_INTERVAL_MS));
  }
}
// ---------------------------------------------------------------------------


void setup() {
  Serial.begin(115200);

  // Native USB CDC only exists once the host has enumerated it, which takes a
  // second or two after boot. Without this wait, everything printed here is
  // written into the void. Bounded so the sketch still runs untethered.
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  log_i("BigDripper valve test, %d coils", NUM_COILS);

  log_i("GPIO outputs:");
  for (int i = 0; i < NUM_COILS; i++) {
      pinMode(coils[i], OUTPUT);
      digitalWrite(coils[i], LOW);
      log_i("COIL%d -> GPIO%u", i + 1, coils[i]);
  }
  coils_begin();

  log_i("GPIO inputs:");
  for (int i = 0; i < NUM_ENCODER_CHANNELS; i++) {
      pinMode(encoder[i], INPUT_PULLUP);
      log_i("ENCODER %s -> GPIO%u", i == 0 ? "A": "B", encoder[i]);
  }

  log_i("setup() on core %d", xPortGetCoreID());

  // params_begin() must complete before the tasks below start, since
  // valve_task's print engine reads params from the moment it runs, and
  // valve_task's priority is high enough to preempt setup() (still running
  // as loopTask) as soon as it's created.
  params_begin();

  // Before valve_task starts, since the print engine pushes to the preview
  // queue. No-op in normal builds.
  console_preview_begin();

  // 4096-byte stacks: both tasks call log_i, and the vsnprintf underneath it is
  // the stack-hungry part. Priorities are above loopTask's 1 so neither is
  // starved by it; being on separate cores makes that mostly academic.
  xTaskCreatePinnedToCore(encoder_task,   "encoder",   4096, NULL, 3, NULL, ENCODER_CORE);
  xTaskCreatePinnedToCore(valve_task,     "valve",     4096, NULL, 2, NULL, VALVE_CORE);
  xTaskCreatePinnedToCore(neopixel_task,  "neopixel",  4096, NULL, 1, NULL, ENCODER_CORE);

  wifi_text_input_begin();
}

void set_coil_state(uint16_t bitVector) {
  uint16_t bit = 0x8000;  // bit 15 -> coils[0] (COIL1)
  uint32_t bank0_set = 0, bank0_clear = 0;
  uint32_t bank1_set = 0, bank1_clear = 0;

  for (int i = 0; i < NUM_COILS; i++) {
    bool on = (bitVector & bit) != 0;
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
  // One write per bank per direction, instead of one digitalWrite() per
  // coil, so every coil switches within the same register access rather
  // than one at a time.
  GPIO.out_w1ts = bank0_set;
  GPIO.out_w1tc = bank0_clear;
  GPIO.out1_w1ts.val = bank1_set;
  GPIO.out1_w1tc.val = bank1_clear;
#endif
}

// --- Coil close timer -------------------------------------------------------
//
// A column's burst is ended by a one-shot esp_timer instead of the valve
// task polling millis(), so the close happens when the timer fires rather
// than up to 5 ms later on the next scan tick.
//
// The timer callback runs in the dedicated esp_timer task, a different
// thread from valve_task, so its close and the task's next open can race.
// coil_mux serializes the two set_coil_state() calls that can actually
// collide, and coil_generation guards against a stale timer: every open (or
// forced state change that must not be undone by an old timer) bumps the
// generation, and coil_armed_generation records which generation the
// currently-armed timer is allowed to close. If a new open/force happens
// before a late callback acquires the lock, the generation it captured no
// longer matches and it skips the close instead of stomping the newer state.
static esp_timer_handle_t coil_off_timer = NULL;
static portMUX_TYPE coil_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t coil_generation = 0;
static volatile uint32_t coil_armed_generation = 0;

static void coil_off_timer_cb(void *arg) {
  portENTER_CRITICAL(&coil_mux);
  if (coil_armed_generation == coil_generation) {
    set_coil_state(0x0000);
  }
  portEXIT_CRITICAL(&coil_mux);
}

void coil_timer_begin() {
  esp_timer_create_args_t args = {};
  args.callback = coil_off_timer_cb;
  args.name = "coil_off";
  esp_timer_create(&args, &coil_off_timer);
}

// Invalidates any in-flight close timer without changing the coils, so a
// stale close can't land after whatever is about to force the coils to a
// new state (priming/trace taking over mid-burst). Does not touch the
// coils itself — the caller is expected to set them right after.
static void coil_timer_invalidate() {
  esp_timer_stop(coil_off_timer);  // ESP_ERR_INVALID_STATE if already idle; fine to ignore
  portENTER_CRITICAL(&coil_mux);
  coil_generation++;
  portEXIT_CRITICAL(&coil_mux);
}

// True if the previous column's close hasn't fired yet, i.e. the column
// about to open would be an overrun. Call before coil_timer_open(), which
// closes-then-reopens regardless -- this is purely for counting/logging.
static bool coil_timer_is_pending() {
  return esp_timer_is_active(coil_off_timer);
}

// Opens a column's valves and arms the close timer for burst_us. No
// logging between the coil write and the timer start, per the "no log_*
// calls between opening and arming" rule -- a UART write here would add
// jitter to exactly the latency this exists to kill.
static void coil_timer_open(uint16_t bits, uint32_t burst_us) {
  esp_timer_stop(coil_off_timer);
  uint32_t my_gen;
  portENTER_CRITICAL(&coil_mux);
  coil_generation++;
  my_gen = coil_generation;
  set_coil_state(bits);
  portEXIT_CRITICAL(&coil_mux);
  coil_armed_generation = my_gen;
  esp_timer_start_once(coil_off_timer, burst_us);
}
// ---------------------------------------------------------------------------

// Naive test function for flipping all coils on / off
void test_GPIO_outputs() {
  while (true) {
    static unsigned tick = 0;

    // Serial.printf: always compiled in, plain text, you control the format.
    set_coil_state(0xFFFF);
    log_d("[%u] coils ON  (%lu ms)", tick, millis());
    delay(1000);

    // log_*: adds level, timestamp, file:line and function; compiled out entirely
    // when CORE_DEBUG_LEVEL is below the macro's level.
    set_coil_state(0x0000);
    log_d("[%u] coils OFF", tick++);
    delay(1000);
  }
}

// naive test function for reading encoder output
void test_encoder() {
  int a = digitalRead(encoder[0]);
  int b = digitalRead(encoder[1]);
  log_i("A:%d - B:%d", a, b);

}

// Scans across the text currently at the front of the queue, driven by the
// encoder: params_get_clicks_per_column() detents move the scan one glyph
// column. Re-zeroes the encoder every time a new text becomes current, so
// column 0 always lines up with wherever the wheel happens to be when that
// text starts printing.
void print_engine_run() {
  // Publish before anything below can be relied on to wake promptly. The
  // encoder ISR null-checks this independently of encoder_task_handle, so
  // publish order between the two tasks' startup doesn't matter.
  valve_task_handle = xTaskGetCurrentTaskHandle();
  coil_timer_begin();

  QueuedText current = {String(""), TEXT_QUEUE_DEFAULT_FONT, false};
  // `current` rendered to coil states, one per column, spacing included.
  // Rendered once when the text becomes current rather than per column.
  std::vector<uint16_t> current_columns;
  int32_t last_column = -1;

  // True while printing TEXT_QUEUE_DEFAULT_TEXT because nothing is queued.
  // Tracked as a flag rather than by comparing strings, so a user who
  // queues the same text still gets it printed once and advanced past.
  bool looping_default = false;

  // See encoder_task()'s LOG_INTERVAL_MS comment: a fast spin can change
  // columns faster than a UART write drains, so cap how often we log a
  // column change even though we still render every one.
  const uint32_t LOG_INTERVAL_MS = 50;
  uint32_t last_log_ms = 0;

  // Mode as of the previous loop iteration, for the transition side-effects
  // below. Starts equal to mode_get()'s boot value so booting into Text
  // doesn't count as "entering Text".
  PrintMode prev_mode = PrintMode::Text;

  // Set when leaving trace mode: the next column the scan reports is taken
  // as the new baseline instead of being fired, so stopping trace leaves
  // every valve closed rather than jumping straight into a burst for
  // wherever the wheel ended up.
  bool resync_column = false;

  // Column-period measurement for the speed-adaptive burst (Ticket 2): EMA of
  // the time between column changes, so a single jittery encoder interval
  // doesn't swing the computed burst. period_valid is false until a real
  // sample exists; until then the burst computation falls back to the max
  // burst. have_last_change_us additionally gates the very first delta after
  // a reset, so that delta (spanning whatever happened before the reset) is
  // discarded as the gap itself rather than mistaken for a real sample.
  const float PERIOD_EMA_ALPHA = 0.3f;
  const uint32_t PERIOD_RESET_GAP_US = 500000;  // bike stopped
  bool have_last_change_us = false;
  uint32_t last_column_change_us = 0;
  float smoothed_period_us = 0;
  bool period_valid = false;
  auto reset_period_estimate = [&]() {
    period_valid = false;
    have_last_change_us = false;
  };

  // Diagnostic only (Ticket 2.4): counts columns where the previous burst's
  // timer was still running when a new one arrived (sudden acceleration).
  // Doesn't persist across reboots.
  uint32_t overrun_count = 0;

  while (true) {
    // Read once and used for the whole iteration, so a change from the web
    // task mid-iteration can't run half of one mode and half of another.
    PrintMode mode = mode_get();
    bool entering_text = false;

    if (mode != prev_mode) {
      switch (prev_mode) {
        case PrintMode::Prime:
          // Close everything rather than falling through to whatever bits
          // the next mode happens to start with.
          set_coil_state(0x0000);
          reset_period_estimate();
          break;
        case PrintMode::Trace:
          set_coil_state(0x0000);
          resync_column = true;
          reset_period_estimate();
          log_i("trace: stopped, all coils closed");
          break;
        case PrintMode::Lines:
          encoder_zero();
          resync_column = true;
          reset_period_estimate();
          log_i("test pattern: stopped");
          break;
        default:
          break;
      }

      switch (mode) {
        case PrintMode::Off:
          // Invalidate first so a close timer left armed by the last column
          // can't fire into whatever mode comes next.
          coil_timer_invalidate();
          set_coil_state(0x0000);
          log_i("mode: off, all coils closed");
          break;
        case PrintMode::Prime:
        case PrintMode::Trace:
          // A timer armed by the column that was open when priming/trace
          // engaged must not fire mid-mode and close the valves out from
          // under it.
          coil_timer_invalidate();
          break;
        case PrintMode::Lines:
          // Restart the scan from column 0 without firing that column, so
          // switching modes while standing still doesn't spray a line --
          // the first burst waits for the wheel to move.
          encoder_zero();
          resync_column = true;
          reset_period_estimate();
          log_i("test pattern: started");
          break;
        case PrintMode::Text:
          // Same for text: restart the current text from its beginning, and
          // don't fire until the wheel moves.
          entering_text = true;
          encoder_zero();
          resync_column = true;
          reset_period_estimate();
          last_column = -1;
          break;
      }
      prev_mode = mode;
      console_preview_mode(mode);
    }

    if (mode == PrintMode::Off) {
      // Nothing prints: valves stay closed (done on entry above) and the
      // text queue isn't read or advanced, so Text resumes the same message.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
      continue;
    }

    if (mode == PrintMode::Prime) {
      // Manual override: hold every valve open regardless of scan state.
      set_coil_state(0xFFFF);
      delay(5);
      continue;
    }

    if (mode == PrintMode::Trace) {
      // Debug pulse: all coils on for exactly the configured burst, then
      // all off for the configured trace gap. Blocking is fine here since nothing else
      // runs while tracing. Log after the burst, not during, so a slow
      // UART write can't stretch the pulse being measured.
      uint32_t burst_ms = params_get_column_burst_ms();
      uint32_t start_us = micros();
      set_coil_state(0xFFFF);
      // Poll rather than delay(burst_ms) so any mode change closes the
      // valves immediately even in the middle of a long burst.
      uint32_t burst_start = millis();
      while (millis() - burst_start < burst_ms && mode_get() == PrintMode::Trace) {
        delay(1);
      }
      set_coil_state(0x0000);
      uint32_t elapsed_us = micros() - start_us;
      log_i("trace: all coils ON for %lu ms (measured %lu us)",
            (unsigned long)burst_ms, (unsigned long)elapsed_us);

      // Off period, polled in small steps so a mode change takes effect
      // promptly instead of after the full gap.
      uint32_t gap_ms = params_get_trace_gap_ms();
      uint32_t off_start = millis();
      while (millis() - off_start < gap_ms && mode_get() == PrintMode::Trace) {
        delay(5);
      }
      continue;
    }

    // The test pattern replaces the text as the source of each column's
    // bits, but otherwise runs through the normal per-column path below.
    bool testing = mode == PrintMode::Lines;

    if (!testing) {
      QueuedText next = text_queue_current();
      bool use_default = next.text.length() == 0 && text_queue_is_idle();
      if (use_default) {
        next = {String(TEXT_QUEUE_DEFAULT_TEXT), TEXT_QUEUE_DEFAULT_FONT, false};
      }
      // Font and invert compared too, so the same text queued again in
      // another font or inverted right after itself still re-renders.
      if (next.text != current.text || next.font != current.font ||
          next.invert != current.invert || use_default != looping_default) {
        current = next;
        current_columns = font_render(current.text, current.font, current.invert);
        looping_default = use_default;
        last_column = -1;
        // A new text starting is a deliberate print, so fire its first
        // column -- unless we only got here by switching into Text, which
        // waits for the wheel like any other mode switch.
        if (!entering_text) {
          resync_column = false;
        }
        reset_period_estimate();
        encoder_zero();
        log_i("print engine: now printing \"%s\" in %s%s", current.text.c_str(),
              font_label(current.font), current.invert ? ", inverted" : "");
        if (current.text.length() > 0) {
          console_preview_message(current, looping_default);
        }
      }

      if (current_columns.empty()) {
        set_coil_state(0x0000);
        if (current.text.length() > 0 && !looping_default) {
          // Queued text with nothing printable in it. text_queue_push()
          // rejects those, so this is only a backstop. The
          // scan below would never reach its end, so skip it rather than
          // stalling the queue.
          log_i("print engine: nothing to print in \"%s\", skipping", current.text.c_str());
          text_queue_advance();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
        continue;
      }
    }

    int32_t clicks_per_column = (int32_t)params_get_clicks_per_column();
    if (clicks_per_column <= 0) clicks_per_column = 1;

    // Scanning backwards past the start of the text clamps at column 0
    // rather than going negative.
    int32_t pos = encoder_position();
    int32_t column = (pos > 0) ? (pos / clicks_per_column) : 0;

    // The test pattern has no end; only text wraps or advances the queue.
    int32_t total_columns = 0;
    if (!testing) {
      total_columns = (int32_t)current_columns.size();
      if (looping_default) {
        // Wrap instead of advancing, so the default text repeats seamlessly
        // for as long as the wheel keeps turning.
        column %= total_columns;
      } else if (column >= total_columns) {
        log_i("print engine: done, advancing queue");
        set_coil_state(0x0000);
        text_queue_advance();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
        continue;
      }
    }

    if (resync_column) {
      last_column = column;
      resync_column = false;
    }

    if (column != last_column) {
      // Measure the time since the previous column change and fold it into
      // the EMA. The first delta after a reset (have_last_change_us false,
      // or a >500ms gap below) is discarded rather than sampled -- see
      // reset_period_estimate()'s comment.
      uint32_t now_us = micros();
      uint32_t period_us = 0;
      if (have_last_change_us) {
        period_us = now_us - last_column_change_us;
        if (period_us > PERIOD_RESET_GAP_US) {
          period_valid = false;
        } else if (!period_valid) {
          smoothed_period_us = (float)period_us;
          period_valid = true;
        } else {
          smoothed_period_us = PERIOD_EMA_ALPHA * period_us + (1.0f - PERIOD_EMA_ALPHA) * smoothed_period_us;
        }
      }
      last_column_change_us = now_us;
      have_last_change_us = true;

      uint16_t bits = testing ? test_pattern_bits(column) : current_columns[column];

      // Scale the burst to the measured column period so dots stay short and
      // separate at speed, capped at the walking-speed default and floored
      // at the shortest burst the valves reliably open for. min_burst_us
      // capped at max_burst_us guards against a transient
      // min_burst_ms > column_burst_ms (params changed mid-print); /params
      // validation is the real guard against persisting that combination.
      uint32_t max_burst_us = params_get_column_burst_ms() * 1000;
      uint32_t min_burst_us = min(params_get_min_burst_ms() * 1000, max_burst_us);
      uint32_t burst_us = max_burst_us;
      if (period_valid) {
        float target_us = (params_get_burst_duty_pct() / 100.0f) * smoothed_period_us;
        burst_us = (uint32_t)min((float)max_burst_us, max((float)min_burst_us, target_us));
      }
      if (coil_timer_is_pending()) {
        // Previous burst hadn't closed yet -- coil_timer_open() below stops
        // that timer and overwrites the coil state with this column's bits
        // before re-arming, so the merge is avoided, but count it: a
        // non-zero rate here means bursts are outrunning the column period
        // at the current speed/duty/min-burst settings.
        overrun_count++;
      }
      coil_timer_open(bits, burst_us);

      // Only after the timer is armed (see coil_timer_open()). Columns the
      // encoder jumped over never fire, so the preview says so instead of
      // silently squashing the picture. last_column is -1 right after a
      // reset, where nothing was skipped.
      if (last_column >= 0) {
        int32_t jump = abs(column - last_column);
        if (looping_default) {
          // The default text wraps, so last -> 0 is one column, not a jump.
          jump = min(jump, total_columns - jump);
        }
        if (jump > 1) {
          console_preview_skipped(jump - 1);
        }
      }
      console_preview_column(bits);
      last_column = column;

      uint32_t now = millis();
      if (now - last_log_ms >= LOG_INTERVAL_MS) {
        log_i("print engine: pos=%ld col=%ld/%ld bits=0x%04x period_us=%lu burst_us=%lu overruns=%lu",
              (long)pos, (long)column, (long)total_columns, bits,
              (unsigned long)period_us, (unsigned long)burst_us, (unsigned long)overrun_count);
        last_log_ms = now;
      }
    }
    // Repeated scan ticks on the same column do nothing now -- the close
    // timer armed above owns ending the burst, not this loop.

    // Blocks until the encoder ISR notifies us (immediate pickup of a new
    // column) or 5 ms elapses (fallback tick for priming/trace/queue state
    // changes, which aren't signaled by the encoder).
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
  }
}

void demoLEDs() {
  for (int i=0; i< 16; i++) {
    digitalWrite(coils[i], HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(coils[i], LOW);
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  for (int j=0; j< 3; j++) {
    for (int i=0; i< 16; i++) {
        digitalWrite(coils[i], HIGH);
      }
      vTaskDelay(pdMS_TO_TICKS(100));

      for (int i=0; i< 16; i++) {
        digitalWrite(coils[i], LOW);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void loop() {

  // demoLEDs();

  
  // Both jobs now live in their own pinned tasks, so loopTask has nothing to do.
  // It still has to yield: returning immediately would spin core 1 at full tilt
  // and starve the idle task. Deleting loopTask outright is the alternative, but
  // keeping it costs nothing and leaves somewhere to put ad-hoc test code.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
