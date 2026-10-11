// PCNT bench test: can the hardware pulse counter be the encoder input for
// the print engine (lib/drip_core), with its Wake windows mapped onto PCNT
// watch points?
//
// What it checks, with the real encoder and the WiFi AP up:
//   1. Counts: PCNT (hardware quadrature, glitch filter) against the old
//      software decoder (GPIO interrupts on core 0, next to WiFi) on the
//      same two pins. They should agree; if they drift apart, one of them
//      is losing counts.
//   2. Moving windows: like the engine, keeps a window [lo, hi] around the
//      position and is woken by watch points at lo-1 and hi+1. Moves them
//      on every crossing *without clearing the counter*, which is the open
//      question: do new watch point values take effect while counting?
//      A 20 ms poll catches any crossing whose event never came ("stale").
//   3. Where the PCNT interrupt runs (should be core 1) and how long from
//      interrupt to the task seeing it, and from there to the watch points
//      being moved.
//
// Output and commands on Serial0 (the CP2102 port):
//   w <n>   window width in counts (default 40)
//   m <n>   0: move watch points only, 1: also clear the counter (default)
//   r       reset the statistics
//
//   pio run -e pcnt-test -t upload -t monitor

#include <Arduino.h>
#include <WiFi.h>
#include "driver/pulse_cnt.h"
#include "esp_timer.h"
#include "soc/pcnt_struct.h"

static const int PIN_A = 4;
static const int PIN_B = 5;

// Raw counter range. The hardware resets to 0 when it reaches either limit;
// the callback carries the difference into `offset` so positions are 64-bit.
static const int LIMIT = 30000;

static pcnt_unit_handle_t unit = nullptr;
static TaskHandle_t engine_task_handle = nullptr;
static portMUX_TYPE pos_mux = portMUX_INITIALIZER_UNLOCKED;

// Written by the PCNT callback, read by the engine task under pos_mux.
static volatile int64_t offset = 0;
static volatile int64_t isr_time_us = 0;
static volatile int isr_value = 0;
static volatile int isr_core = -1;
static volatile uint32_t isr_events = 0;

// --- Old software decoder, for comparison --------------------------------

static const DRAM_ATTR int8_t QUAD_TABLE[16] = {0, -1, +1, 0, +1, 0, 0, -1,
                                                -1, 0, 0, +1, 0, +1, -1, 0};
static volatile int32_t sw_count = 0;
static volatile uint8_t sw_prev = 0;
static volatile uint32_t sw_invalid = 0;  // transitions the table rejects
static portMUX_TYPE sw_mux = portMUX_INITIALIZER_UNLOCKED;

static void ARDUINO_ISR_ATTR sw_isr() {
  uint8_t state = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
  portENTER_CRITICAL_ISR(&sw_mux);
  uint8_t idx = (sw_prev << 2) | state;
  int8_t step = QUAD_TABLE[idx];
  sw_count += step;
  if (step == 0 && (sw_prev != state)) sw_invalid++;
  sw_prev = state;
  portEXIT_CRITICAL_ISR(&sw_mux);
}

static void sw_task(void *) {
  // attachInterrupt from core 0, like the current firmware.
  sw_prev = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
  attachInterrupt(digitalPinToInterrupt(PIN_A), sw_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_B), sw_isr, CHANGE);
  vTaskDelete(nullptr);
}

// --- PCNT ------------------------------------------------------------------

static bool IRAM_ATTR on_reach(pcnt_unit_handle_t u, const pcnt_watch_event_data_t *e, void *) {
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&pos_mux);
  if (e->watch_point_value == LIMIT) offset += LIMIT;
  if (e->watch_point_value == -LIMIT) offset -= LIMIT;
  isr_time_us = now;
  isr_value = e->watch_point_value;
  isr_events++;
  portEXIT_CRITICAL_ISR(&pos_mux);
  isr_core = xPortGetCoreID();

  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(engine_task_handle, &woken);
  return woken == pdTRUE;
}

// Raw counter register of unit 0, the only unit this sketch creates.
static inline int raw_count() {
  return (int16_t)PCNT.cnt_unit[0].pulse_cnt_un;
}

// Absolute position. If the counter has just wrapped at a limit but the
// callback hasn't run yet, the limit event is still pending in the status
// register: count it here so the position doesn't jump by LIMIT.
static int64_t position() {
  portENTER_CRITICAL(&pos_mux);
  int raw = raw_count();
  int64_t pos = offset + raw;
  uint32_t pending = PCNT.int_raw.val & 1;
  uint32_t status = PCNT.status_unit[0].val;
  portEXIT_CRITICAL(&pos_mux);
  if (pending) {
    // status bits: 2 = thres1, 3 = thres0, 4 = low limit, 5 = high limit, 6 = zero
    if (status & (1 << 5)) pos += LIMIT;
    if (status & (1 << 4)) pos -= LIMIT;
  }
  return pos;
}

static void pcnt_begin() {
  pcnt_unit_config_t ucfg = {};
  ucfg.low_limit = -LIMIT;
  ucfg.high_limit = LIMIT;
  ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &unit));

  pcnt_glitch_filter_config_t fcfg = {};
  fcfg.max_glitch_ns = 1000;
  ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(unit, &fcfg));

  // Full x4 quadrature: each channel counts the edges of one signal, with
  // the other as the direction level. Same as IDF's rotary encoder example.
  pcnt_chan_config_t a = {};
  a.edge_gpio_num = PIN_A;
  a.level_gpio_num = PIN_B;
  pcnt_channel_handle_t ch_a;
  ESP_ERROR_CHECK(pcnt_new_channel(unit, &a, &ch_a));
  pcnt_chan_config_t b = {};
  b.edge_gpio_num = PIN_B;
  b.level_gpio_num = PIN_A;
  pcnt_channel_handle_t ch_b;
  ESP_ERROR_CHECK(pcnt_new_channel(unit, &b, &ch_b));
  ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                               PCNT_CHANNEL_EDGE_ACTION_INCREASE));
  ESP_ERROR_CHECK(pcnt_channel_set_level_action(ch_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
  ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                               PCNT_CHANNEL_EDGE_ACTION_DECREASE));
  ESP_ERROR_CHECK(pcnt_channel_set_level_action(ch_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

  ESP_ERROR_CHECK(pcnt_unit_add_watch_point(unit, LIMIT));
  ESP_ERROR_CHECK(pcnt_unit_add_watch_point(unit, -LIMIT));

  pcnt_event_callbacks_t cbs = {};
  cbs.on_reach = on_reach;
  ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(unit, &cbs, nullptr));
  ESP_ERROR_CHECK(pcnt_unit_enable(unit));
  ESP_ERROR_CHECK(pcnt_unit_clear_count(unit));
  ESP_ERROR_CHECK(pcnt_unit_start(unit));
}

// --- The engine stand-in -----------------------------------------------------

static volatile int32_t window_width = 40;

// 1: clear the counter after moving the watch points, folding what it held
// into `offset`, in case new values only take effect on a reset.
static volatile int clear_mode = 1;

struct Stats {
  uint32_t crossings = 0;   // woken by a watch point and outside the window: as intended
  uint32_t spurious = 0;    // woken but still inside the window (limit wraps, bounces)
  uint32_t stale = 0;       // poll found us outside the window with no event: missed
  uint32_t raced = 0;       // moved out of the new window while it was being set
  uint32_t value_mismatch = 0;  // event value wasn't one of the two thresholds we set
  uint32_t add_errors = 0;
  uint32_t shifted = 0;     // clear mode: moved between choosing and clearing
  int64_t lat_sum = 0;
  int64_t lat_max = 0;
  int64_t lat_min = INT64_MAX;
  int64_t reprog_max = 0;
  int64_t reprog_sum = 0;
  uint32_t reprogs = 0;
};
static Stats stats;
static volatile bool reset_stats = false;

// The two thresholds currently set, in raw counter space, or INT32_MIN.
static int thr_lo = INT32_MIN, thr_hi = INT32_MIN;

// Sets the watch points for the window [lo, hi] (absolute), i.e. at lo-1
// and hi+1 in raw counter terms. A threshold beyond the counter's limits
// isn't set: the limit event wakes us first anyway.
static void set_thresholds(int new_lo, int new_hi) {
  if (thr_lo != INT32_MIN) pcnt_unit_remove_watch_point(unit, thr_lo);
  if (thr_hi != INT32_MIN) pcnt_unit_remove_watch_point(unit, thr_hi);
  thr_lo = thr_hi = INT32_MIN;
  if (new_lo != INT32_MIN) {
    if (pcnt_unit_add_watch_point(unit, new_lo) == ESP_OK) thr_lo = new_lo; else stats.add_errors++;
  }
  if (new_hi != INT32_MIN) {
    if (pcnt_unit_add_watch_point(unit, new_hi) == ESP_OK) thr_hi = new_hi; else stats.add_errors++;
  }
}

static int clip(int64_t raw) {
  return (raw > -LIMIT && raw < LIMIT) ? (int)raw : INT32_MIN;
}

static void set_window(int64_t lo, int64_t hi) {
  int64_t t0 = esp_timer_get_time();
  if (!clear_mode) {
    portENTER_CRITICAL(&pos_mux);
    int64_t off = offset;
    portEXIT_CRITICAL(&pos_mux);
    set_thresholds(clip(lo - 1 - off), clip(hi + 1 - off));
  } else {
    // Thresholds relative to where the counter will be zeroed, then the
    // clear, with what the counter held folded into `offset`. If the wheel
    // moved in between, the thresholds are off by that much: redo them.
    for (int tries = 0; tries < 4; tries++) {
      int64_t base = position();
      set_thresholds(clip(lo - 1 - base), clip(hi + 1 - base));
      portENTER_CRITICAL(&pos_mux);
      int raw = raw_count();
      pcnt_unit_clear_count(unit);
      offset += raw;
      int64_t now_base = offset;
      portEXIT_CRITICAL(&pos_mux);
      if (now_base == base) break;
      stats.shifted++;
    }
  }
  int64_t dt = esp_timer_get_time() - t0;
  stats.reprog_sum += dt;
  stats.reprogs++;
  if (dt > stats.reprog_max) stats.reprog_max = dt;
}

static int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b < 0) ? q - 1 : q;
}

static void engine_task(void *) {
  engine_task_handle = xTaskGetCurrentTaskHandle();
  // From this task, so the PCNT interrupt is allocated on this core.
  pcnt_begin();

  int64_t lo = 0, hi = -1;  // empty: forces a window on the first pass
  uint32_t seen_events = 0;
  for (;;) {
    bool notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20)) > 0;
    int64_t now = esp_timer_get_time();
    if (reset_stats) {
      stats = Stats();
      reset_stats = false;
    }
    int64_t pos = position();
    bool outside = pos < lo || pos > hi;

    if (notified) {
      portENTER_CRITICAL(&pos_mux);
      int64_t t_isr = isr_time_us;
      int value = isr_value;
      uint32_t events = isr_events;
      portEXIT_CRITICAL(&pos_mux);
      seen_events = events;
      if (outside) {
        stats.crossings++;
        int64_t lat = now - t_isr;
        stats.lat_sum += lat;
        if (lat > stats.lat_max) stats.lat_max = lat;
        if (lat < stats.lat_min) stats.lat_min = lat;
        if (value != thr_lo && value != thr_hi && value != LIMIT && value != -LIMIT) {
          stats.value_mismatch++;
        }
      } else {
        stats.spurious++;
      }
    } else if (outside && hi >= lo) {
      stats.stale++;
      Serial0.printf("!! stale: pos=%lld window=[%lld,%lld] thr raw=[%d,%d] raw=%d off=%lld events=%lu\n",
                     (long long)pos, (long long)lo, (long long)hi, thr_lo, thr_hi, raw_count(),
                     (long long)offset, (unsigned long)seen_events);
    }

    if (outside) {
      // Re-centre on the window `pos` is in, and check we didn't leave it
      // again while the watch points were being moved.
      int32_t w = window_width;
      for (int tries = 0; tries < 4; tries++) {
        lo = floor_div(pos, w) * w;
        hi = lo + w - 1;
        set_window(lo, hi);
        pos = position();
        if (pos >= lo && pos <= hi) break;
        stats.raced++;
      }
    }
  }
}

// --- Setup, report, commands -------------------------------------------------

void setup() {
  Serial0.begin(115200);
  delay(200);
  Serial0.println("\n== PCNT bench test ==");
  pinMode(PIN_A, INPUT_PULLUP);
  pinMode(PIN_B, INPUT_PULLUP);

  // WiFi up on core 0 like the real firmware, so the software decoder has
  // the radio to compete with.
  WiFi.mode(WIFI_AP);
  WiFi.softAP("BigDripper-pcnt", "dripdrip1");
  Serial0.printf("AP up at %s\n", WiFi.softAPIP().toString().c_str());

  xTaskCreatePinnedToCore(engine_task, "engine", 4096, nullptr, 10, nullptr, 1);
  xTaskCreatePinnedToCore(sw_task, "sw", 4096, nullptr, 5, nullptr, 0);
  Serial0.println("commands: 'w <n>' window width, 'r' reset stats");
}

static String line;

void loop() {
  while (Serial0.available()) {
    char c = Serial0.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.startsWith("w ")) {
        long w = line.substring(2).toInt();
        if (w >= 1) window_width = w;
        Serial0.printf("window width %ld\n", (long)window_width);
      } else if (line.startsWith("m ")) {
        clear_mode = line.substring(2).toInt() ? 1 : 0;
        Serial0.printf("clear mode %d\n", clear_mode);
      } else if (line == "r") {
        reset_stats = true;
        Serial0.println("stats reset");
      }
      line = "";
    } else {
      line += c;
    }
  }

  static int64_t last_pos = INT64_MIN;
  static int32_t last_sw = INT32_MIN;
  static uint32_t last_report = 0;
  int64_t pos = position();
  int32_t sw = sw_count;
  if (millis() - last_report >= 500 && (pos != last_pos || sw != last_sw)) {
    last_report = millis();
    last_pos = pos;
    last_sw = sw;
    Stats s = stats;
    Serial0.printf(
        "pcnt=%lld sw=%ld sum=%lld diff=%lld sw_invalid=%lu | w=%ld cross=%lu spur=%lu stale=%lu "
        "raced=%lu shifted=%lu badval=%lu adderr=%lu m=%d | lat us min/avg/max=%lld/%lld/%lld reprog avg/max=%lld/%lld "
        "| isr core %d\n",
        (long long)pos, (long)sw, (long long)(pos + sw), (long long)(pos - sw),
        (unsigned long)sw_invalid, (long)window_width, (unsigned long)s.crossings,
        (unsigned long)s.spurious, (unsigned long)s.stale, (unsigned long)s.raced,
        (unsigned long)s.shifted,
        (unsigned long)s.value_mismatch, (unsigned long)s.add_errors, clear_mode,
        (long long)(s.crossings ? s.lat_min : 0),
        (long long)(s.crossings ? s.lat_sum / s.crossings : 0), (long long)s.lat_max,
        (long long)(s.reprogs ? s.reprog_sum / s.reprogs : 0), (long long)s.reprog_max, isr_core);
  }
  delay(20);
}
