#include "font.h"
#include "font_data.h"
#include <string.h>
#include <strings.h>

// Glyph columns are stored with bit 15 = top row. With COIL1 printing the
// top row the text came out mirrored on the ground, so the top row goes to
// COIL16 (bit 0) instead. If it ever comes out mirrored again (e.g. the head
// is remounted), set this to false.
static const bool TOP_ROW_ON_COIL16 = true;

static uint16_t to_coils(uint16_t column) {
  if (!TOP_ROW_ON_COIL16) return column;
  uint16_t out = 0;
  for (int row = 0; row < FONT_HEIGHT; row++) {
    if (column & (0x8000 >> row)) out |= 1 << row;
  }
  return out;
}

const char *font_label(Font font) {
  switch (font) {
    case Font::Spleen:   return "Spleen";
    case Font::DripBold: return "Drip bold";
    default:             return "Drip";
  }
}

bool font_from_id(const String &id, Font *out) {
  if (id == "spleen") { *out = Font::Spleen; return true; }
  if (id == "drip") { *out = Font::Drip; return true; }
  if (id == "drip_bold") { *out = Font::DripBold; return true; }
  return false;
}

static const FontGlyph *find(const FontGlyph *table, size_t count,
                             const char *token, size_t len) {
  for (size_t i = 0; i < count; i++) {
    if (strlen(table[i].token) == len && memcmp(table[i].token, token, len) == 0) {
      return &table[i];
    }
  }
  return nullptr;
}

// Longest emoji shortcode starting at `s`, ignoring case, or nullptr.
static const FontGlyph *match_emoji(const char *s, size_t *len_out) {
  const FontGlyph *best = nullptr;
  size_t best_len = 0;
  for (size_t i = 0; i < FONT_EMOJI_COUNT; i++) {
    size_t len = strlen(FONT_EMOJI[i].token);
    if (len > best_len && strncasecmp(s, FONT_EMOJI[i].token, len) == 0) {
      best = &FONT_EMOJI[i];
      best_len = len;
    }
  }
  *len_out = best_len;
  return best;
}

// Byte length of the UTF-8 sequence starting with `lead`. A stray
// continuation byte counts as 1 so a malformed string still advances.
static size_t utf8_len(uint8_t lead) {
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

// Walks `text` one printed glyph at a time, left to right, calling
// fn(const FontGlyph *) for each. Rendering and font_glyph_count() both go
// through here, so the length limit counts exactly what gets printed: one per
// letter, emoji or unknown character (printed as a space).
template <typename Fn>
static void for_each_glyph(const String &text, Font font, Fn fn) {
  const char *s = text.c_str();
  size_t n = text.length();
  const FontGlyph *table;
  size_t count;
  switch (font) {
    case Font::Spleen:   table = FONT_SPLEEN;    count = FONT_SPLEEN_COUNT;    break;
    case Font::DripBold: table = FONT_DRIP_BOLD; count = FONT_DRIP_BOLD_COUNT; break;
    default:             table = FONT_DRIP;      count = FONT_DRIP_COUNT;      break;
  }

  size_t i = 0;
  while (i < n) {
    size_t len = 0;
    const FontGlyph *g = match_emoji(s + i, &len);
    if (!g) {
      char token[4];
      len = utf8_len((uint8_t)s[i]);
      if (i + len > n) len = n - i;
      memcpy(token, s + i, len);
      if (len == 1) {
        token[0] = toupper((unsigned char)token[0]);
      } else if (len == 2 && (uint8_t)token[0] == 0xC3 &&
                 (uint8_t)token[1] >= 0xA0 && (uint8_t)token[1] <= 0xBE &&
                 (uint8_t)token[1] != 0xB7) {
        // Latin-1 lowercase -> uppercase is 0x20 down (æ ø å -> Æ Ø Å).
        // Skips ÷ (not a letter) and ÿ (its uppercase isn't in Latin-1).
        token[1] -= 0x20;
      }
      g = find(table, count, token, len);
      if (!g) g = find(table, count, " ", 1);
    }

    fn(g);
    i += len;
  }
}

// Glyph columns before the row-to-coil mapping, i.e. bit 15 = top row.
static std::vector<uint16_t> render_rows(const String &text, Font font) {
  std::vector<uint16_t> out;
  for_each_glyph(text, font, [&](const FontGlyph *g) {
    out.insert(out.end(), g->columns, g->columns + g->width);
    out.insert(out.end(), FONT_SPACING, 0);
  });
  return out;
}

size_t font_glyph_count(const String &text, Font font) {
  size_t count = 0;
  for_each_glyph(text, font, [&](const FontGlyph *) { count++; });
  return count;
}

std::vector<uint16_t> font_render(const String &text, Font font, bool invert) {
  std::vector<uint16_t> cols = render_rows(text, font);
  for (uint16_t &c : cols) c = to_coils(c);
  if (invert) {
    // to_coils() only permutes bits, so inverting after it is the same as
    // inverting the glyph.
    for (uint16_t &c : cols) c = ~c;
    cols.insert(cols.begin(), FONT_INVERT_EDGE, 0xFFFF);
    cols.insert(cols.end(), FONT_INVERT_EDGE, 0xFFFF);
  }
  return cols;
}

void font_print_ascii(const String &text, Font font) {
  std::vector<uint16_t> cols = render_rows(text, font);
  String line;
  line.reserve(cols.size());
  for (int row = 0; row < FONT_HEIGHT; row++) {
    line = "";
    for (uint16_t c : cols) {
      line += (c & (0x8000 >> row)) ? '#' : '.';
    }
    log_i("%s", line.c_str());
  }
}
