#include "encoder.h"
#include "driver/pulse_cnt.h"
#include "esp_timer.h"

static const int PIN_A = 4;
static const int PIN_B = 5;

// The hardware counter is 16-bit and resets to 0 at either limit. Rather
// than tracking wraps, it's cleared every time the window moves (see
// encoder_set_window()), and the window is never more than WINDOW_SPAN
// either side of where it was set, so it never gets near the limits.
static const int LIMIT = 32000;
static const int64_t WINDOW_SPAN = 16384;

static const int NO_WATCH = INT32_MIN;

static pcnt_unit_handle_t unit = nullptr;
static TaskHandle_t notify_task = nullptr;

// position = offset + the counter. `offset` changes only when the task
// clears the counter, and the last watch point event is written by the
// interrupt; both under `mux`.
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t offset = 0;
static drip::Position event_position = INT64_MIN;
static drip::Micros event_at = 0;

// Task only: the watch points set now (counter values), the window they
// implement (positions), and the window that was asked for, which is wider
// when WINDOW_SPAN clipped it.
static int watch_lo = NO_WATCH;
static int watch_hi = NO_WATCH;
static drip::Position set_lo = 1;
static drip::Position set_hi = 0;  // empty until the first encoder_set_window()
static drip::Position asked_lo = 0;
static drip::Position asked_hi = 0;

static bool IRAM_ATTR on_reach(pcnt_unit_handle_t, const pcnt_watch_event_data_t *e, void *) {
  drip::Micros now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&mux);
  event_position = offset + e->watch_point_value;
  event_at = now;
  portEXIT_CRITICAL_ISR(&mux);

  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(notify_task, &woken);
  return woken == pdTRUE;
}

static drip::Position position() {
  int raw = 0;
  portENTER_CRITICAL(&mux);
  pcnt_unit_get_count(unit, &raw);
  drip::Position pos = offset + raw;
  portEXIT_CRITICAL(&mux);
  return pos;
}

void encoder_begin(TaskHandle_t task) {
  notify_task = task;

  pcnt_unit_config_t ucfg = {};
  ucfg.low_limit = -LIMIT;
  ucfg.high_limit = LIMIT;
  ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &unit));

  // Well below the ~100 us between edges at a fast hand spin.
  pcnt_glitch_filter_config_t fcfg = {};
  fcfg.max_glitch_ns = 1000;
  ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(unit, &fcfg));

  // Full x4 quadrature: each channel counts one signal's edges with the
  // other as the direction. Same direction as the old software decoder.
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

  pcnt_event_callbacks_t cbs = {};
  cbs.on_reach = on_reach;
  ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(unit, &cbs, nullptr));
  ESP_ERROR_CHECK(pcnt_unit_enable(unit));
  ESP_ERROR_CHECK(pcnt_unit_clear_count(unit));
  ESP_ERROR_CHECK(pcnt_unit_start(unit));
  log_i("encoder: PCNT on GPIO%d/%d, interrupt on core %d", PIN_A, PIN_B, xPortGetCoreID());
}

drip::Sample encoder_sample(drip::Micros now) {
  drip::Position pos = position();
  portENTER_CRITICAL(&mux);
  drip::Sample s = {pos, pos == event_position ? event_at : now};
  portEXIT_CRITICAL(&mux);
  return s;
}

static void set_watch_points(int lo, int hi) {
  if (watch_lo != NO_WATCH) pcnt_unit_remove_watch_point(unit, watch_lo);
  if (watch_hi != NO_WATCH) pcnt_unit_remove_watch_point(unit, watch_hi);
  watch_lo = watch_hi = NO_WATCH;
  if (pcnt_unit_add_watch_point(unit, lo) == ESP_OK) watch_lo = lo;
  if (pcnt_unit_add_watch_point(unit, hi) == ESP_OK) watch_hi = hi;
}

bool encoder_set_window(drip::Position lo, drip::Position hi) {
  drip::Position pos = position();
  // Still inside the window that's set, and asked for the same one again:
  // nothing to do. That's most steps.
  if (lo == asked_lo && hi == asked_hi && pos >= set_lo && pos <= set_hi) return true;

  // On this chip a watch point set while counting only takes effect once
  // the counter is reset (measured on the board, see
  // spikes/pcnt_test/). So: set them relative to where the counter is,
  // then clear it, carrying what it held into `offset`. If the wheel moved
  // in between, they're off by that much and are set again.
  for (int tries = 0; tries < 4; tries++) {
    drip::Position base = position();
    if (base < lo || base > hi) return false;
    drip::Position wlo = (lo < base - WINDOW_SPAN) ? base - WINDOW_SPAN : lo;
    drip::Position whi = (hi > base + WINDOW_SPAN) ? base + WINDOW_SPAN : hi;
    set_watch_points((int)(wlo - 1 - base), (int)(whi + 1 - base));

    int raw = 0;
    portENTER_CRITICAL(&mux);
    pcnt_unit_get_count(unit, &raw);
    pcnt_unit_clear_count(unit);
    offset += raw;
    bool moved = offset != base;
    portEXIT_CRITICAL(&mux);
    if (!moved) {
      set_lo = wlo;
      set_hi = whi;
      asked_lo = lo;
      asked_hi = hi;
      return true;
    }
  }
  // Moving too fast to pin down; let the caller step and try again.
  set_lo = 1;
  set_hi = 0;
  return false;
}
