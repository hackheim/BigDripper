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

static const FontGlyph *find(const FontGlyph *table, size_t count,
                             const char *token, size_t len) {
  for (size_t i = 0; i < count; i++) {
    if (strlen(table[i].token) == len && memcmp(table[i].token, token, len) == 0) {
      return &table[i];
    }
  }
  return nullptr;
}

// Longest emoji token starting at `s`, ignoring case, or nullptr. Longest
// so ":-)" wins over a hypothetical ":-" and the nose-less ":)" both work.
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

// Glyph columns before the row-to-coil mapping, i.e. bit 15 = top row.
static std::vector<uint16_t> render_rows(const String &text) {
  std::vector<uint16_t> out;
  const char *s = text.c_str();
  size_t n = text.length();
  bool bold = false;

  size_t i = 0;
  while (i < n) {
    const FontGlyph *table = bold ? FONT_BOLD : FONT_REGULAR;
    size_t count = bold ? FONT_BOLD_COUNT : FONT_REGULAR_COUNT;

    size_t len = 0;
    const FontGlyph *g = match_emoji(s + i, &len);
    if (!g) {
      if (s[i] == '*') {
        bold = !bold;
        i++;
        continue;
      }

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

    out.insert(out.end(), g->columns, g->columns + g->width);
    out.insert(out.end(), FONT_SPACING, 0);
    i += len;
  }
  return out;
}

std::vector<uint16_t> font_render(const String &text) {
  std::vector<uint16_t> cols = render_rows(text);
  for (uint16_t &c : cols) c = to_coils(c);
  return cols;
}

void font_print_ascii(const String &text) {
  std::vector<uint16_t> cols = render_rows(text);
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
