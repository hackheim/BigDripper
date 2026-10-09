#pragma once
#include <Arduino.h>
#include <vector>

// Bitmap fonts for the 16-nozzle print head: every glyph is 16 rows tall,
// one row per nozzle. The glyph shapes live in font_design/gen_font.py,
// which generates font_data.cpp; see font_design/font_preview.txt for what
// they look like.
//
// What a text can contain:
//   - A-Z (lowercase is printed as uppercase), Æ Ø Å (æ ø å too), 0-9,
//     space and . , ! ? - : '
//   - *bold*: an asterisk switches between regular and bold weight, so
//     "HI *THERE*" prints THERE in bold. The asterisks aren't printed.
//   - Emoji: :-) :-( :-D :-| :-/ ;-) :-O :-P (the nose is optional, letters
//     either case), <3 for a heart, :drop: and :star:.
// Anything else prints as a space.

const int FONT_HEIGHT = 16;

// Blank columns printed after every glyph.
const int FONT_SPACING = 1;

// Renders `text` into the columns to print, left to right, spacing
// included. Each value is fed straight into set_coil_state() (bit 15 =
// COIL1 ... bit 0 = COIL16).
std::vector<uint16_t> font_render(const String &text);

// Debug helper: logs `text` as FONT_HEIGHT rows of '#'/'.' via log_i, the
// right way up, so the font can be checked over serial without hardware.
void font_print_ascii(const String &text);
