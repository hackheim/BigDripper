#pragma once
#include <Arduino.h>
#include "drip/params.h"

// Tunable settings, persisted to NVS via the Preferences library so they
// survive a reboot. Call params_begin() once from setup(), before anything
// reads them.
//
// They're read and written as a whole: params_check() a changed copy, then
// params_set() it, so a rejected form never leaves half of it saved and the
// print engine never sees a mix of old and new values.

struct Settings {
  // mm of carriage travel per encoder detent. Calibrates encoder position to
  // a physical distance; not used by the print engine yet.
  float scale_mm_per_detent;

  // Longest text the queue accepts, in printed glyphs (font_glyph_count()).
  uint32_t max_text_len;

  // Pause between one printed text finishing and the next one starting.
  uint32_t print_pause_ms;

  // Encoder detents per printed column. No field on the settings page yet.
  uint32_t clicks_per_column;

  // Ceiling on how long a column's coils stay open, and the Trace pulse.
  uint32_t column_burst_ms;

  // Percent of the measured column period the burst targets, so dots don't
  // smear together at speed. 5-95.
  uint32_t burst_duty_pct;

  // Floor on burst length, below which the valves don't reliably open.
  // 1 ms up to column_burst_ms.
  uint32_t min_burst_ms;

  // Trace only: how long every coil stays closed between pulses.
  uint32_t trace_gap_ms;

  // Lines only: a full column every this many columns. 1 = every column.
  uint32_t test_line_spacing;

  // Blank columns between printed characters, 0..FONT_MAX_SPACING. Applies
  // to messages queued from then on, and the default text.
  uint32_t letter_spacing;

  // Stretching: at speed, columns are widened so each takes at least this
  // long to cross. 0 = never stretch.
  uint32_t min_column_ms;

  // The most a column is widened, as a percentage of its normal width.
  // 100-1000.
  uint32_t max_stretch_pct;
};

void params_begin();

Settings params_get();

// nullptr if `s` is fine to save, otherwise why not, fit to show a user.
const char *params_check(const Settings &s);

// Saves `s`, which must have passed params_check(). Doesn't reach the print
// engine by itself: see printer_set_settings().
void params_set(const Settings &s);

// What the print engine needs out of `s`.
drip::Params params_to_engine(const Settings &s);
