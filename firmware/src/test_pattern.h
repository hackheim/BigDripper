#pragma once
#include <Arduino.h>

// Encoder-driven test patterns for checking print quality at speed, printed
// while the mode (mode.h) is PrintMode::Lines. Unlike trace mode, which
// pulses on a timer, these go through the normal per-column print path:
// spacing is set in columns (so it's a fixed distance on the ground, whatever
// the speed) and every burst is subject to the speed-adaptive burst length
// and overrun handling.

// Coil bits for `column` of the active pattern. Currently the only pattern:
// a full vertical line (all coils) every params_get_test_line_spacing()
// columns, blank in between.
uint16_t test_pattern_bits(int32_t column);
