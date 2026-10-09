#pragma once
#include <Arduino.h>

// Encoder-driven test patterns for checking print quality at speed. Unlike
// trace mode, which pulses on a timer, these go through the normal
// per-column print path: spacing is set in columns (so it's a fixed distance
// on the ground, whatever the speed) and every burst is subject to the
// speed-adaptive burst length and overrun handling. Same threading story as
// trace.h: set by the web server, read by the print engine, so the on/off
// state is a plain atomic.

void test_pattern_start();
void test_pattern_stop();
bool test_pattern_is_active();

// Coil bits for `column` of the active pattern. Currently the only pattern:
// a full vertical line (all coils) every params_get_test_line_spacing()
// columns, blank in between.
uint16_t test_pattern_bits(int32_t column);
