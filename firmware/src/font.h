#pragma once
#include <Arduino.h>
#include <vector>

// Bitmap fonts for the 16-nozzle print head: every glyph is 16 rows tall,
// one row per nozzle. The glyph shapes live in font_design/ (gen_font.py,
// spleen_8x16.py), which generates font_data.cpp; see
// font_design/font_preview.txt for what they look like.
//
// What a text can contain:
//   - In Drip and Drip bold: A-Z (lowercase is printed as uppercase),
//     Æ Ø Å (æ ø å too), 0-9, space and . , ! ? - : '
//   - In Spleen: A-Z, 0-9 and space only.
//   - Emoji, the same in every font, as Slack/Discord-style shortcodes
//     (either case): :smile: :sad: :big_smile: :neutral: :confused: :wink:
//     :surprised: :tongue: :heart: :drop: :star:. ASCII faces like :-) or <3
//     print as the plain characters they are.
// Anything else, `*` included, prints as the font's space. A text prints in
// one font; there's no markup to switch mid-text.

// Named as on the web page: Spleen (the original Spleen 8x16), Drip and
// Drip bold (the generated fonts).
enum class Font : uint8_t { Spleen, Drip, DripBold };

// "Spleen", "Drip", "Drip bold" -- shown on the page and in /status.
const char *font_label(Font font);

// Parses the id the page's form sends: "spleen", "drip" or "drip_bold". Returns
// false (and leaves *out alone) for anything else.
bool font_from_id(const String &id, Font *out);

const int FONT_HEIGHT = 16;

// Blank columns printed after every glyph.
const int FONT_SPACING = 1;

// All-on columns added before and after an inverted text. Without them a
// glyph whose outer column has ink would leave that edge dry next to dry
// ground, and the text would have no visible start or end.
const int FONT_INVERT_EDGE = 2;

// Renders `text` into the columns to print, left to right, spacing
// included. Each value is fed straight into set_coil_state() (bit 15 =
// COIL1 ... bit 0 = COIL16).
//
// With `invert`, every column is bit-inverted (spacing columns included),
// so the letters are left dry in a sprayed band, and FONT_INVERT_EDGE
// all-on columns frame it on each side.
std::vector<uint16_t> font_render(const String &text, Font font, bool invert = false);

// How many glyphs `text` prints: one per letter, emoji or unknown
// character (which prints as a space). This is what the max text length
// limits, not bytes.
size_t font_glyph_count(const String &text, Font font);

// Debug helper: logs `text` as FONT_HEIGHT rows of '#'/'.' via log_i, the
// right way up, so the font can be checked over serial without hardware.
void font_print_ascii(const String &text, Font font);
