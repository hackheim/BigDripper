// The pure part of params.h: defaults, checking and the conversion to the
// engine's units. No NVS or FreeRTOS here, so the web simulator (sim/)
// builds it too and checks settings exactly as the board does.
#include <math.h>
#include "font.h"
#include "params.h"

const Settings SETTINGS_DEFAULTS = {
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
    0.0f,  // stretch_above_kmh
    300,   // max_stretch_pct
};

const char *params_check(const Settings &s) {
  if (!(s.scale_mm_per_detent > 0) || !isfinite(s.scale_mm_per_detent)) {
    return "Distance per encoder click must be above 0.";
  }
  if (s.max_text_len < 1 || s.max_text_len > 255) {
    return "Longest message must be between 1 and 255 characters.";
  }
  if (s.clicks_per_column < 1) return "Column width must be at least 1 click.";
  if (s.letter_spacing > FONT_MAX_SPACING) return "Gap between letters must be between 0 and 8 columns.";
  if (!(s.stretch_above_kmh >= 0) || !isfinite(s.stretch_above_kmh)) {
    return "Stretch speed can't be negative (0 = never stretch).";
  }
  return drip::params_validate(params_to_engine(s));
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
  // Stretching starts where a normal column takes min_column_us to cross:
  // column width (mm) over the stretch speed (mm/us = km/h / 3600).
  if (s.stretch_above_kmh > 0 && s.scale_mm_per_detent > 0) {
    double column_mm = (double)s.clicks_per_column * s.scale_mm_per_detent;
    p.min_column_us = (drip::Micros)llround(column_mm * 3600.0 / s.stretch_above_kmh);
  } else {
    p.min_column_us = 0;
  }
  p.max_stretch_pct = s.max_stretch_pct;
  return p;
}
