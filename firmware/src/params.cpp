#include "params.h"
#include <Preferences.h>

// Used only until the /params page saves a value for the first time.
static const float DEFAULT_SCALE_MM_PER_DETENT = 1.0f;
static const size_t DEFAULT_MAX_TEXT_LEN = 64;
static const uint32_t DEFAULT_PRINT_PAUSE_MS = 2000;
static const uint32_t DEFAULT_CLICKS_PER_COLUMN = 100;
static const uint32_t DEFAULT_COLUMN_BURST_MS = 20;

static Preferences prefs;
static SemaphoreHandle_t params_mutex;

static float scale_mm_per_detent;
static size_t max_text_len;
static uint32_t print_pause_ms;
static uint32_t clicks_per_column;
static uint32_t column_burst_ms;

void params_begin() {
  params_mutex = xSemaphoreCreateMutex();

  // false = read/write. NVS keys are capped at 15 chars.
  prefs.begin("bigdripper", false);
  scale_mm_per_detent = prefs.getFloat("scale", DEFAULT_SCALE_MM_PER_DETENT);
  max_text_len = prefs.getUInt("maxlen", DEFAULT_MAX_TEXT_LEN);
  print_pause_ms = prefs.getUInt("pausems", DEFAULT_PRINT_PAUSE_MS);
  clicks_per_column = prefs.getUInt("clkpercol", DEFAULT_CLICKS_PER_COLUMN);
  column_burst_ms = prefs.getUInt("burstms", DEFAULT_COLUMN_BURST_MS);
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
