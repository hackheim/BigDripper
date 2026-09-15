#pragma once
#include <Arduino.h>

// Monospace bitmap font for the 16-nozzle print head, natively 16 rows tall
// (one row per nozzle) so there's no upscaling blur/blockiness. Caps, digits
// and space only. Each glyph is FONT_GLYPH_WIDTH columns wide; each column is
// a uint16_t meant to be fed straight into set_coil_state() (bit 15 = COIL1/
// top nozzle ... bit 0 = COIL16/bottom nozzle, same convention as
// example_valve_control.cpp). FONT_CHAR_WIDTH adds one blank column of
// inter-character spacing.
//
// Assumes COIL1 is mounted at the top of the head and COIL16 at the bottom;
// flip the row-to-bit mapping in font.cpp's convert_glyph() if that's
// backwards.

const int FONT_HEIGHT = 16;
const int FONT_GLYPH_WIDTH = 8;
const int FONT_CHAR_WIDTH = FONT_GLYPH_WIDTH + 1;

// Builds the glyph lookup table. Must be called once (e.g. from setup())
// before font_get_glyph() or font_print_ascii().
void font_init();

// Writes FONT_GLYPH_WIDTH columns for `c` into out[0..FONT_GLYPH_WIDTH-1].
// Case-insensitive; unsupported characters produce a blank glyph.
void font_get_glyph(char c, uint16_t *out);

// Debug helper: logs `c`'s glyph as FONT_HEIGHT rows of '#'/'.' via log_i,
// so the font can be sanity-checked over serial without real hardware.
void font_print_ascii(char c);
