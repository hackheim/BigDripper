#include "params.h"
#include <Preferences.h>

// Used only until the /params page saves a value for the first time.
static const float DEFAULT_SCALE_MM_PER_DETENT = 1.0f;
static const size_t DEFAULT_MAX_TEXT_LEN = 64;
static const uint32_t DEFAULT_PRINT_PAUSE_MS = 2000;
static const uint32_t DEFAULT_CLICKS_PER_COLUMN = 100;
static const uint32_t DEFAULT_COLUMN_BURST_MS = 20;
static const uint32_t DEFAULT_TRACE_GAP_MS = 1000;
static const uint32_t DEFAULT_BURST_DUTY_PCT = 50;
static const uint32_t DEFAULT_MIN_BURST_MS = 4;
static const uint32_t DEFAULT_TEST_LINE_SPACING = 10;

static Preferences prefs;
static SemaphoreHandle_t params_mutex;

static float scale_mm_per_detent;
static size_t max_text_len;
static uint32_t print_pause_ms;
static uint32_t clicks_per_column;
static uint32_t column_burst_ms;
static uint32_t trace_gap_ms;
static uint32_t burst_duty_pct;
static uint32_t min_burst_ms;
static uint32_t test_line_spacing;

void params_begin() {
  params_mutex = xSemaphoreCreateMutex();

  // false = read/write. NVS keys are capped at 15 chars.
  prefs.begin("bigdripper", false);
  scale_mm_per_detent = prefs.getFloat("scale", DEFAULT_SCALE_MM_PER_DETENT);
  max_text_len = prefs.getUInt("maxlen", DEFAULT_MAX_TEXT_LEN);
  print_pause_ms = prefs.getUInt("pausems", DEFAULT_PRINT_PAUSE_MS);
  clicks_per_column = prefs.getUInt("clkpercol", DEFAULT_CLICKS_PER_COLUMN);
  column_burst_ms = prefs.getUInt("burstms", DEFAULT_COLUMN_BURST_MS);
  trace_gap_ms = prefs.getUInt("tracegapms", DEFAULT_TRACE_GAP_MS);
  burst_duty_pct = prefs.getUInt("burstduty", DEFAULT_BURST_DUTY_PCT);
  min_burst_ms = prefs.getUInt("minburstms", DEFAULT_MIN_BURST_MS);
  test_line_spacing = prefs.getUInt("testspacing", DEFAULT_TEST_LINE_SPACING);
}

float params_get_scale_mm_per_detent() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  float v = scale_mm_per_detent;
  xSemaphoreGive(params_mutex);
  return v;
}

void params_set_scale_mm_per_detent(float v) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  scale_mm_per_detent = v;
  prefs.putFloat("scale", v);
  xSemaphoreGive(params_mutex);
}

size_t params_get_max_text_len() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  size_t v = max_text_len;
  xSemaphoreGive(params_mutex);
  return v;
}

void params_set_max_text_len(size_t v) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  max_text_len = v;
  prefs.putUInt("maxlen", (uint32_t)v);
  xSemaphoreGive(params_mutex);
}

uint32_t params_get_print_pause_ms() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  uint32_t v = print_pause_ms;
  xSemaphoreGive(params_mutex);
  return v;
}

void params_set_print_pause_ms(uint32_t v) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  print_pause_ms = v;
  prefs.putUInt("pausems", v);
  xSemaphoreGive(params_mutex);
}

uint32_t params_get_clicks_per_column() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  uint32_t v = clicks_per_column;
  xSemaphoreGive(params_mutex);
  return v;
}

void params_set_clicks_per_column(uint32_t v) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  clicks_per_column = v;
  prefs.putUInt("clkpercol", v);
  xSemaphoreGive(params_mutex);
}

uint32_t params_get_column_burst_ms() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  uint32_t v = column_burst_ms;
  xSemaphoreGive(params_mutex);
  return v;
}

void params_set_column_burst_ms(uint32_t v) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  column_burst_ms = v;
  prefs.putUInt("burstms", v);
  xSemaphoreGive(params_mutex);
}

uint32_t params_get_trace_gap_ms() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  uint32_t v = trace_gap_ms;
  xSemaphoreGive(params_mutex);
  return v;
}

void params_set_trace_gap_ms(uint32_t v) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  trace_gap_ms = v;
  prefs.putUInt("tracegapms", v);
  xSemaphoreGive(params_mutex);
}

uint32_t params_get_burst_duty_pct() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  uint32_t v = burst_duty_pct;
  xSemaphoreGive(params_mutex);
  return v;
}

void params_set_burst_duty_pct(uint32_t v) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  burst_duty_pct = v;
  prefs.putUInt("burstduty", v);
  xSemaphoreGive(params_mutex);
}

uint32_t params_get_min_burst_ms() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  uint32_t v = min_burst_ms;
  xSemaphoreGive(params_mutex);
  return v;
}

void params_set_min_burst_ms(uint32_t v) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  min_burst_ms = v;
  prefs.putUInt("minburstms", v);
  xSemaphoreGive(params_mutex);
}

uint32_t params_get_test_line_spacing() {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  uint32_t v = test_line_spacing;
  xSemaphoreGive(params_mutex);
  return v;
}

void params_set_test_line_spacing(uint32_t v) {
  xSemaphoreTake(params_mutex, portMAX_DELAY);
  test_line_spacing = v;
  prefs.putUInt("testspacing", v);
  xSemaphoreGive(params_mutex);
}
