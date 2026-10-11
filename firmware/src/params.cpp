#include "params.h"
#include <Preferences.h>
#include "encoder.h"
#include "font.h"

// Used only until the /params page saves a value for the first time.
static const Settings DEFAULTS = {
    1.0f,  // scale_mm_per_detent
    64,    // max_text_len
    2000,  // print_pause_ms
    100,   // clicks_per_column
    20,    // column_burst_ms
    50,    // burst_duty_pct
    4,     // min_burst_ms
    1000,  // trace_gap_ms
    10,    // test_line_spacing
    FONT_SPACING,  // letter_spacing
};

static Preferences prefs;
static SemaphoreHandle_t params_mutex;
static Settings current;

void params_begin() {
  params_mutex = xSemaphoreCreateMutex();

  // false = read/write. NVS keys are capped at 15 chars.
  prefs.begin("bigdripper", false);
  current.scale_mm_per_detent = prefs.getFloat("scale", DEFAULTS.scale_mm_per_detent);
  current.max_text_len = prefs.getUInt("maxlen", DEFAULTS.max_text_len);
  current.print_pause_ms = prefs.getUInt("pausems", DEFAULTS.print_pause_ms);
  current.clicks_per_column = prefs.getUInt("clkpercol", DEFAULTS.clicks_per_column);
  current.column_burst_ms = prefs.getUInt("burstms", DEFAULTS.column_burst_ms);
  current.burst_duty_pct = prefs.getUInt("burstduty", DEFAULTS.burst_duty_pct);
  current.min_burst_ms = prefs.getUInt("minburstms", DEFAULTS.min_burst_ms);
  current.trace_gap_ms = prefs.getUInt("tracegapms", DEFAULTS.trace_gap_ms);
  current.test_line_spacing = prefs.getUInt("testspacing", DEFAULTS.test_line_spacing);
  current.letter_spacing = prefs.getUInt("letterspace", DEFAULTS.letter_spacing);

  // Whatever an older firmware saved has to be usable by this one.
  if (params_check(current) != nullptr) {
    log_w("saved settings invalid (%s), using defaults", params_check(current));
    current = DEFAULTS;
  }
}

Settings params_get() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  Settings s = current;
  xSemaphoreGive(params_mutex);
  return s;
}

const char *params_check(const Settings &s) {
  if (s.max_text_len < 1 || s.max_text_len > 255) {
    return "Max text length must be between 1 and 255.";
  }
  if (s.clicks_per_column < 1) return "Detents per column must be at least 1.";
  if (s.letter_spacing > FONT_MAX_SPACING) return "Letter spacing must be between 0 and 8 columns.";
  return drip::params_validate(params_to_engine(s));
}

void params_set(const Settings &s) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  current = s;
  prefs.putFloat("scale", s.scale_mm_per_detent);
  prefs.putUInt("maxlen", s.max_text_len);
  prefs.putUInt("pausems", s.print_pause_ms);
  prefs.putUInt("clkpercol", s.clicks_per_column);
  prefs.putUInt("burstms", s.column_burst_ms);
  prefs.putUInt("burstduty", s.burst_duty_pct);
  prefs.putUInt("minburstms", s.min_burst_ms);
  prefs.putUInt("tracegapms", s.trace_gap_ms);
  prefs.putUInt("testspacing", s.test_line_spacing);
  prefs.putUInt("letterspace", s.letter_spacing);
  xSemaphoreGive(params_mutex);
}

drip::Params params_to_engine(const Settings &s) {
  drip::Params p;
  p.counts_per_column = (int32_t)(s.clicks_per_column * ENCODER_COUNTS_PER_DETENT);
  p.max_burst_us = (drip::Micros)s.column_burst_ms * 1000;
  p.min_burst_us = (drip::Micros)s.min_burst_ms * 1000;
  p.burst_duty_pct = s.burst_duty_pct;
  p.trace_gap_us = (drip::Micros)s.trace_gap_ms * 1000;
  p.line_spacing = (int32_t)s.test_line_spacing;
  p.message_pause_us = (drip::Micros)s.print_pause_ms * 1000;
  return p;
}
