// Host-side checks for font.cpp: fonts, emoji shortcodes and
// font_glyph_count().
// Run with font_design/host_test/run.sh from firmware/.
#include "font.h"
#include <cstdlib>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char *what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static const Font FONTS[] = {Font::Spleen, Font::Drip, Font::DripBold};

static std::vector<uint16_t> r(const char *s, Font f = Font::Drip) { return font_render(String(s), f); }

// Renders each byte of `s` as its own text and joins them, i.e. what `s`
// prints as if none of it were an emoji.
static std::vector<uint16_t> plain(const char *s) {
  std::vector<uint16_t> out;
  for (const char *p = s; *p; p++) {
    char one[2] = {*p, 0};
    std::vector<uint16_t> cols = r(one);
    out.insert(out.end(), cols.begin(), cols.end());
  }
  return out;
}

int main() {
  const char *codes[] = {":smile:", ":sad:", ":big_smile:", ":neutral:",
                         ":confused:", ":wink:", ":surprised:", ":tongue:",
                         ":heart:", ":drop:", ":star:"};
  for (const char *c : codes) {
    std::vector<uint16_t> cols = r(c);
    bool ok = true;
    for (Font f : FONTS) {
      ok = ok && r(c, f) == cols && font_glyph_count(c, f) == 1;
    }
    check(ok && cols.size() == 16 + FONT_SPACING, c);
  }

  check(r(":Smile:") == r(":smile:") && r(":SMILE:") == r(":smile:"), "shortcodes ignore case");

  const char *ascii[] = {":-)", ":)", "<3", "HTTP://", "KL:DO", ";-)", ":-P"};
  for (const char *a : ascii) {
    check(r(a) == plain(a) && font_glyph_count(a, Font::Drip) == strlen(a), a);
  }

  check(font_glyph_count("HI :smile: :heart:", Font::Drip) == 6, "HI :smile: :heart: counts 6");
  check(font_glyph_count("\xC3\x86\xC3\x98\xC3\x85", Font::Drip) == 3, "\xC3\x86\xC3\x98\xC3\x85 counts 3");
  check(font_glyph_count("", Font::Drip) == 0, "empty counts 0");
  check(font_glyph_count("~#", Font::Drip) == 2, "unknown characters count 1 each");
  check(font_glyph_count(":smile", Font::Drip) == 6, "unterminated shortcode is plain text");

  // No *bold* markup: '*' is an unknown character, i.e. a space, in every font.
  bool stars = true;
  for (Font f : FONTS) {
    stars = stars && r("HI *X*", f) == r("HI  X ", f) && font_glyph_count("**", f) == 2;
  }
  check(stars, "* prints as a space in every font");

  check(r("HELLO", Font::Spleen).size() == 5 * (8 + FONT_SPACING), "Spleen glyphs are 8 columns");
  check(r("hello", Font::Spleen) == r("HELLO", Font::Spleen), "Spleen lowercase prints as uppercase");
  check(r("\xC3\x86\xC3\x98\xC3\x85!", Font::Spleen) == r("    ", Font::Spleen), "\xC3\x86\xC3\x98\xC3\x85! prints as 4 spaces in Spleen");
  check(r("\xC3\x86\xC3\x98\xC3\x85!", Font::Drip) != r("    ", Font::Drip), "\xC3\x86\xC3\x98\xC3\x85! prints in Drip");
  check(r("HELLO", Font::DripBold) != r("HELLO", Font::Drip), "Drip bold differs from Drip");

  Font f = Font::Drip;
  check(font_from_id("spleen", &f) && f == Font::Spleen && font_from_id("drip_bold", &f) && f == Font::DripBold &&
            font_from_id("drip", &f) && f == Font::Drip && !font_from_id("bogus", &f) && f == Font::Drip,
        "font_from_id");
  check(String(font_label(Font::DripBold)) == "Drip bold", "font_label");

  printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
