#include "params.h"
#include <Preferences.h>

static Preferences prefs;
static SemaphoreHandle_t params_mutex;
static Settings current;

void params_begin() {
  params_mutex = xSemaphoreCreateMutex();

  // false = read/write. NVS keys are capped at 15 chars.
  prefs.begin("bigdripper", false);
  current.scale_mm_per_detent = prefs.getFloat("scale", SETTINGS_DEFAULTS.scale_mm_per_detent);
  current.max_text_len = prefs.getUInt("maxlen", SETTINGS_DEFAULTS.max_text_len);
  current.print_pause_ms = prefs.getUInt("pausems", SETTINGS_DEFAULTS.print_pause_ms);
  current.clicks_per_column = prefs.getUInt("clkpercol", SETTINGS_DEFAULTS.clicks_per_column);
  current.column_burst_ms = prefs.getUInt("burstms", SETTINGS_DEFAULTS.column_burst_ms);
  current.burst_duty_pct = prefs.getUInt("burstduty", SETTINGS_DEFAULTS.burst_duty_pct);
  current.min_burst_ms = prefs.getUInt("minburstms", SETTINGS_DEFAULTS.min_burst_ms);
  current.trace_gap_ms = prefs.getUInt("tracegapms", SETTINGS_DEFAULTS.trace_gap_ms);
  current.test_line_spacing = prefs.getUInt("testspacing", SETTINGS_DEFAULTS.test_line_spacing);
  current.letter_spacing = prefs.getUInt("letterspace", SETTINGS_DEFAULTS.letter_spacing);
  current.stretch_above_kmh = prefs.getFloat("stretchkmh", SETTINGS_DEFAULTS.stretch_above_kmh);
  current.max_stretch_pct = prefs.getUInt("maxstretch", SETTINGS_DEFAULTS.max_stretch_pct);

  // Whatever an older firmware saved has to be usable by this one.
  if (params_check(current) != nullptr) {
    log_w("saved settings invalid (%s), using defaults", params_check(current));
    current = SETTINGS_DEFAULTS;
  }
}

Settings params_get() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  Settings s = current;
  xSemaphoreGive(params_mutex);
  return s;
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
  prefs.putFloat("stretchkmh", s.stretch_above_kmh);
  prefs.putUInt("maxstretch", s.max_stretch_pct);
  xSemaphoreGive(params_mutex);
}
