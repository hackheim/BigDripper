#!/usr/bin/env python3
"""Generates the BigDripper print-head fonts (16 rows = 16 valves) and
writes an ASCII preview. Three fonts, named as on the web page:
  Spleen     Spleen 8x16, the original font, from bitmaps (spleen_8x16.py)
  Drip       generated regular weight
  Drip bold  generated bold weight

Glyphs are built from strokes instead of being hand-drawn per style, so the
regular and bold weights stay consistent:
  regular: vertical strokes 1 column wide, horizontal strokes 2 rows tall
  bold:    vertical strokes 2 columns wide, horizontal strokes 3 rows tall

Usage:
  python3 gen_font.py [font_preview.txt]      ASCII preview
  python3 gen_font.py --c ../src/font_data.cpp  tables for the firmware
"""
import sys

import spleen_8x16

HEIGHT = 16


class Glyph:
    def __init__(self, w, v, h):
        self.W, self.V, self.H = w, v, h
        self.T = 0                      # top bar row
        self.B = HEIGHT - h             # bottom bar row
        self.M = (HEIGHT - h) // 2      # middle bar row
        self.R = w - v                  # right stem column
        self.C = (w - v) // 2           # centred stem column
        self.c = v                      # notch size where bowls meet
        # Corner radii for cut(): big enough that round letters (O) read
        # differently from square-cornered ones (D, B).
        self.rx, self.ry = (3, 4) if v == 1 else (4, 5)
        self.px = [[False] * w for _ in range(HEIGHT)]

    def _set(self, x, y, on=True):
        if 0 <= x < self.W and 0 <= y < HEIGHT:
            self.px[y][x] = on

    def rect(self, x0, y0, x1, y1, on=True):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self._set(x, y, on)

    def v(self, x, y0=0, y1=HEIGHT - 1):
        """Vertical stroke, V columns wide starting at x."""
        self.rect(x, y0, x + self.V - 1, y1)

    def h(self, y, x0=0, x1=None):
        """Horizontal stroke, H rows tall starting at y."""
        self.rect(x0, y, self.W - 1 if x1 is None else x1, y + self.H - 1)

    def line(self, x0, y0, x1, y1):
        """Diagonal. Steep lines get the vertical-stroke width, shallow ones
        the horizontal-stroke height, so weight matches the nearest axis.
        Anything steeper than ~27 degrees counts as steep: a 4-tall bold
        brush on a medium slope turns into a blob."""
        dx, dy = x1 - x0, y1 - y0
        if 2 * abs(dy) >= abs(dx):
            for i in range(abs(dy) + 1):
                y = y0 + (i if dy >= 0 else -i)
                x = int(x0 + dx * i / abs(dy) + 0.5) if dy else x0
                self.rect(x, y, x + self.V - 1, y)
        else:
            for i in range(abs(dx) + 1):
                x = x0 + (i if dx >= 0 else -i)
                y = int(y0 + dy * i / abs(dx) + 0.5)
                y = min(y, HEIGHT - self.H)
                self.rect(x, y, x, y + self.H - 1)

    def mline(self, x0, y0, x1, y1):
        """line() plus its left-right mirror image, so symmetric letters
        (M, V, W, X, Y) come out symmetric despite rounding."""
        scratch = Glyph(self.W, self.V, self.H)
        scratch.line(x0, y0, x1, y1)
        for y in range(HEIGHT):
            for x in range(self.W):
                if scratch.px[y][x]:
                    self._set(x, y)
                    self._set(self.W - 1 - x, y)

    def cut(self, corner, x=None, y=None):
        """Rounds a corner into a quarter ellipse of radii rx, ry. corner is
        tl/tr/bl/br; x, y default to the glyph's own corner. Clears what
        lies outside the outer arc and fills the band between it and an
        inner arc V columns / H rows in, so the curve keeps the stroke
        weights. Pixels inside the inner arc are left alone, so a stroke
        crossing the corner's box survives."""
        sx = 1 if corner[1] == 'l' else -1
        sy = 1 if corner[0] == 't' else -1
        if x is None:
            x = 0 if sx == 1 else self.W - 1
        if y is None:
            y = 0 if sy == 1 else HEIGHT - 1
        rx, ry = self.rx, self.ry
        for i in range(int(rx + 0.5)):
            for j in range(int(ry + 0.5)):
                px, py = rx - (i + 0.5), ry - (j + 0.5)  # from arc centre
                if (px / rx) ** 2 + (py / ry) ** 2 > 1:
                    self._set(x + sx * i, y + sy * j, False)
                elif (px / (rx - self.V)) ** 2 + (py / (ry - self.H)) ** 2 >= 1:
                    self._set(x + sx * i, y + sy * j)

    def dot(self, x, y):
        """Punctuation dot: 2V wide, H tall."""
        self.rect(x, y, x + 2 * self.V - 1, y + self.H - 1)

    # --- Shapes for emoji. A shape is a predicate on continuous coordinates
    # (the glyph spans 0..W by 0..HEIGHT), sampled at each pixel's centre.

    def mask(self, inside):
        return [[inside(x + 0.5, y + 0.5) for x in range(self.W)]
                for y in range(HEIGHT)]

    def outline(self, m):
        """The shape's rim: V columns thick at its sides, H rows thick at its
        top and bottom, i.e. the same weights as the letters' strokes.
        Only looks straight across and straight up/down: checking the
        diagonals too would make curves V+H thick where they slope."""
        def solid(x, y):
            return 0 <= x < self.W and 0 <= y < HEIGHT and m[y][x]
        def interior(x, y):
            return (all(solid(x + i, y) for i in range(-self.V, self.V + 1)) and
                    all(solid(x, y + j) for j in range(-self.H, self.H + 1)))
        return [[m[y][x] and not interior(x, y)
                 for x in range(self.W)] for y in range(HEIGHT)]

    def paint(self, m, on=True, rows=range(HEIGHT), cols=None):
        for y in rows:
            for x in (range(self.W) if cols is None else cols):
                if m[y][x]:
                    self._set(x, y, on)

    def bitmap(self, rows):
        """Hand-drawn pixels, '#' = on, vertically centred."""
        top = (HEIGHT - len(rows)) // 2
        for y, row in enumerate(rows):
            for x, c in enumerate(row):
                self._set(x, top + y, c == '#')


def ellipse(cx, cy, rx, ry):
    return lambda x, y: ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1


def draw(ch, g):
    W, V, H, T, B, M, R, C, c = g.W, g.V, g.H, g.T, g.B, g.M, g.R, g.C, g.c
    last = HEIGHT - 1
    dx = C - (2 * V - V) // 2  # x of a centred punctuation dot

    if ch == ' ':
        pass
    elif ch == 'A':
        g.v(0); g.v(R); g.h(T); g.h(M); g.cut('tl'); g.cut('tr')
    elif ch == 'B':
        g.v(0); g.v(R); g.h(T); g.h(M); g.h(B)
        g.rect(W - c, M, W - 1, M + H - 1, False)
        g.cut('tr'); g.cut('br')
    elif ch == 'C':
        g.v(0); g.h(T); g.h(B); g.cut('tl'); g.cut('bl')
    elif ch == 'D':
        g.v(0); g.v(R); g.h(T); g.h(B); g.cut('tr'); g.cut('br')
    elif ch == 'E':
        g.v(0); g.h(T); g.h(M, 0, W - 2); g.h(B)
    elif ch == 'F':
        g.v(0); g.h(T); g.h(M, 0, W - 2)
    elif ch == 'G':
        g.v(0); g.h(T); g.h(B); g.v(R, M, last); g.h(M, C, W - 1)
        g.cut('tl'); g.cut('bl'); g.cut('br')
    elif ch == 'H':
        g.v(0); g.v(R); g.h(M)
    elif ch == 'I':
        g.v(C); g.h(T, C - 2, C + V + 1); g.h(B, C - 2, C + V + 1)
    elif ch == 'J':
        g.v(R); g.h(T, 2, W - 1); g.h(B); g.v(0, B - 3, last)
        g.cut('bl'); g.cut('br')
    elif ch == 'K':
        g.v(0); g.line(V, M, R, 0); g.line(V, M + H - 1, R, last)
    elif ch == 'L':
        g.v(0); g.h(B)
    elif ch == 'M':
        g.v(0); g.v(R); g.mline(0, 0, C, 8)
    elif ch == 'N':
        g.v(0); g.v(R); g.line(0, 0, R, last)
    elif ch == 'O':
        g.v(0); g.v(R); g.h(T); g.h(B)
        for k in ('tl', 'tr', 'bl', 'br'):
            g.cut(k)
    elif ch == 'P':
        g.v(0); g.h(T); g.h(M); g.v(R, 0, M + H - 1)
        g.cut('tr'); g.cut('br', W - 1, M + H - 1)
    elif ch == 'Q':
        # Square bottom-right corner: the tail runs out through it.
        g.v(0); g.v(R); g.h(T); g.h(B); g.line(C, 10, R, last)
        g.cut('tl'); g.cut('tr'); g.cut('bl')
    elif ch == 'R':
        draw('P', g); g.line(C, M + H, R, last)
    elif ch == 'S':
        g.h(T); g.h(M); g.h(B); g.v(0, 0, M + H - 1); g.v(R, M, last)
        g.cut('tl'); g.cut('br')
        g.cut('bl', 0, M + H - 1); g.cut('tr', W - 1, M)
    elif ch == 'T':
        g.h(T); g.v(C)
    elif ch == 'U':
        g.v(0); g.v(R); g.h(B); g.cut('bl'); g.cut('br')
    elif ch == 'V':
        g.v(0, 0, M); g.v(R, 0, M)
        g.mline(0, M, C, last)
    elif ch == 'W':
        g.v(0); g.v(R); g.mline(0, last, C, 7)
    elif ch == 'X':
        g.mline(0, 0, R, last)
    elif ch == 'Y':
        g.mline(0, 0, C, 8); g.v(C, 8, last)
    elif ch == 'Z':
        g.h(T); g.h(B); g.line(R, H, 0, B - 1)

    elif ch == '0':
        draw('O', g); g.rect(C, M, C + V - 1, M + H - 1)
    elif ch == '1':
        g.v(C); g.h(B, C - 2, C + V + 1); g.line(C - 3, 3, C, 0)
    elif ch == '2':
        g.h(T); g.v(0, 0, 4); g.v(R, 0, 6); g.line(R, 6, 0, B); g.h(B)
        g.cut('tl'); g.cut('tr')
    elif ch == '3':
        g.h(T); g.h(M, 2); g.h(B); g.v(R)
        g.rect(W - c, M, W - 1, M + H - 1, False)
        g.cut('tr'); g.cut('br')
    elif ch == '4':
        y4 = HEIGHT - H - 4
        x4 = W - V - 1
        g.v(0, 0, y4 + H - 1); g.h(y4); g.v(x4)
    elif ch == '5':
        g.h(T); g.v(0, 0, M + H - 1); g.h(M); g.v(R, M, last); g.h(B)
        g.cut('br'); g.cut('tr', W - 1, M)
    elif ch == '6':
        g.v(0); g.h(T); g.h(M); g.h(B); g.v(R, M, last)
        g.cut('tl'); g.cut('bl'); g.cut('br'); g.cut('tr', W - 1, M)
    elif ch == '7':
        g.h(T); g.line(R, H, 2, last)
    elif ch == '8':
        draw('O', g); g.h(M)
        g.rect(0, M, c - 1, M + H - 1, False)
        g.rect(W - c, M, W - 1, M + H - 1, False)
    elif ch == '9':
        draw('6', g)
        g.px = [row[::-1] for row in g.px[::-1]]   # 9 is 6 rotated 180°

    elif ch == 'Æ':
        g.v(0); g.v(C); g.h(T); g.h(M, 0, W - 2); g.h(B, C, W - 1)
        g.cut('tl')
    elif ch == 'Ø':
        draw('O', g); g.line(R, 0, 0, last)
        g.cut('tr'); g.cut('bl')   # trims the slash's ends to the curve
    elif ch == 'Å':
        # No room above a 16-row cap, so the A body is shortened to make
        # space for the ring.
        # The ring's top and bottom are 1 row even in bold; a 4-row bold
        # ring would leave the A body too short to read.
        hole_w = 1 if W % 2 else 2  # keeps the ring centred
        ring_w, ring_h = 2 * V + hole_w, 2 + V
        top = ring_h + 1            # first row of the A body
        rx = (W - ring_w) // 2
        g.rect(rx, 0, rx + ring_w - 1, ring_h - 1)
        g.rect(rx + V, 1, rx + V + hole_w - 1, ring_h - 2, False)
        # At least 2 rows of counter between the bars, or the bold A closes up.
        mid = max(top + (HEIGHT - top - H) // 2, top + H + 2)
        g.v(0, top, last); g.v(R, top, last); g.h(top); g.h(mid)
        g.cut('tl', 0, top); g.cut('tr', W - 1, top)

    elif ch == '.':
        g.dot(dx, B)
    elif ch == ',':
        g.dot(dx, B - H); g.line(dx + 2 * V - 1, B, dx, last)
    elif ch == '!':
        g.rect(dx, 0, dx + 2 * V - 1, B - H - 1); g.dot(dx, B)
    elif ch == '?':
        g.h(T); g.v(0, 0, 4); g.v(R, 0, M); g.h(M, C, R + V - 1)
        g.v(C, M, B - H - 1); g.dot(dx, B)
        g.cut('tl'); g.cut('tr')
    elif ch == '-':
        g.h(M, 1, W - 2)
    elif ch == ':':
        g.dot(dx, B - 8); g.dot(dx, B)
    elif ch == "'":
        g.v(C, 0, 4)
    else:
        draw_emoji(ch, g)
    return g


def draw_emoji(name, g):
    W, V, H = g.W, g.V, g.H
    cx = W / 2

    eye_top, eye_w, eye_h = 4, V + 1, 3
    mouth_top = eye_top + eye_h + 1    # first row a mouth may use
    mouth_bot = HEIGHT - H - 2         # last row, leaving a gap above the rim
    mouth_cols = range(4, W - 4)       # keeps mouths clear of the rim
    mouth_mid = (mouth_top + mouth_bot + 1) / 2

    def face():
        g.paint(g.outline(g.mask(ellipse(cx, 8, cx, 8))))

    def eyes(wink=False):
        # Left eye at x = 4, right one mirrored. A wink is a short dash.
        for x in (4, W - 4 - eye_w):
            if wink and x == 4:
                dash_h = max(1, H - 1)
                y = eye_top + eye_h - dash_h
                g.rect(x - 1, y, x + eye_w, y + dash_h - 1)
            else:
                g.rect(x, eye_top, x + eye_w - 1, eye_top + eye_h - 1)

    def rim_of(cy, ry):
        """Rim of an ellipse centred at (cx, cy), clipped to the mouth area:
        centred above the mouth it is a smile, below it a frown."""
        g.paint(g.outline(g.mask(ellipse(cx, cy, 4.5, ry))),
                rows=range(mouth_top, mouth_bot + 1), cols=mouth_cols)

    span = mouth_bot - mouth_top + 1
    if name in (':-)', ';-)'):
        face(); eyes(wink=name == ';-)')
        rim_of(mouth_bot + 1 - 5, 5)
    elif name == ':-(':
        face(); eyes()
        rim_of(mouth_top + 5, 5)
    elif name == ':-D':
        face(); eyes()
        g.paint(g.mask(ellipse(cx, mouth_top, 4.5, span)),
                rows=range(mouth_top, mouth_bot + 1), cols=mouth_cols)
    elif name == ':-|':
        face(); eyes()
        g.h(mouth_top + (span - H) // 2, 5, W - 6)
    elif name == ':-/':
        face(); eyes()
        g.line(5, mouth_bot - H + 1, W - 6, mouth_top)
    elif name == ':-O':
        face(); eyes()
        g.paint(g.outline(g.mask(ellipse(cx, mouth_mid, 2 + V, span / 2))))
    elif name == ':-P':
        face(); eyes()
        g.h(mouth_top, 4, W - 5)
        # Tongue sticking out below the right half of the lip.
        g.paint(g.mask(ellipse(cx + 1.5, mouth_top + H, 2.5, span - H + 1)),
                rows=range(mouth_top + H, mouth_bot + 1))
    elif name == '<3':
        g.bitmap([
            "..####....####..",
            ".######..######.",
            "################",
            "################",
            "################",
            ".##############.",
            "..############..",
            "...##########...",
            "....########....",
            ".....######.....",
            "......####......",
            ".......##.......",
        ])
    elif name == 'DROP':
        # Teardrop outline: a circle at the bottom with straight sides
        # running up to a point at the top centre.
        r, cy = 5.5, 10.0
        def drop(x, y):
            if ((x - cx) ** 2 + (y - cy) ** 2) <= r * r:
                return True
            return 0.5 <= y <= cy and abs(x - cx) <= r * (y - 0.5) / (cy - 0.5)
        g.paint(g.outline(g.mask(drop)))
    elif name == 'STAR':
        g.bitmap([
            ".......##.......",
            ".......##.......",
            "......####......",
            "......####......",
            ".....######.....",
            "################",
            ".##############.",
            "..############..",
            "...##########...",
            "....########....",
            "....########....",
            "...####..####...",
            "...###....###...",
            "..###......###..",
            "..##........##..",
            ".#............#.",
        ])
    else:
        raise ValueError(name)


CHARS = (list("ABCDEFGHIJKLMNOPQRSTUVWXYZ") + list("ÆØÅ")
         + list("0123456789") + list(".,!?-:'") + [' '])

# Emoji come in one style whatever the text weight. Square, so faces come
# out round rather than squashed to letter width; strokes as in regular.
EMOJI_WIDTH, EMOJI_V, EMOJI_H = 16, 1, 2
# (drawing name, shortcode). The shortcode is the only way to type each
# one, Slack/Discord style, so ASCII like ":)" or "<3" in a text never turns
# into a face by accident. Matching is case-insensitive.
EMOJI = [
    (':-)', ':smile:'),
    (':-(', ':sad:'),
    (':-D', ':big_smile:'),
    (':-|', ':neutral:'),
    (':-/', ':confused:'),
    (';-)', ':wink:'),
    (':-O', ':surprised:'),
    (':-P', ':tongue:'),
    ('<3', ':heart:'),
    ('DROP', ':drop:'),
    ('STAR', ':star:'),
]

# (C table name, label on the web page, description, width, V, H)
STYLES = [
    ("FONT_DRIP",      "DRIP",      "verticals 1 wide, horizontals 2 tall", 7, 1, 2),
    ("FONT_DRIP_BOLD", "DRIP BOLD", "verticals 2 wide, horizontals 3 tall", 8, 2, 3),
]

PER_LINE = 8
EMOJI_PER_LINE = 6
# A drop of water on the ground, and dry ground.
ON, OFF = '-', ' '
GAP = "   "


def render_section(out, title, glyphs, per_line):
    out.append(f"=== {title} ===")
    out.append("")
    for i in range(0, len(glyphs), per_line):
        chunk = glyphs[i:i + per_line]
        out.append(GAP.join(
            ("SPACE" if ch == ' ' else ch).center(len(px[0]))
            for ch, px in chunk))
        for y in range(HEIGHT):
            out.append(GAP.join(
                "".join(ON if p else OFF for p in px[y])
                for _, px in chunk))
        out.append("")
    out.append("")


def emoji_glyphs():
    return [(name, code, draw(name, Glyph(EMOJI_WIDTH, EMOJI_V, EMOJI_H)).px)
            for name, code in EMOJI]


def spleen_glyphs():
    """[(char, px)] for the Spleen font, in spleen_8x16.py's order."""
    W = spleen_8x16.WIDTH
    return [(ch, [[bool(row & (0x80 >> x)) for x in range(W)] for row in rows])
            for ch, rows in spleen_8x16.GLYPHS.items()]


def render():
    out = []
    render_section(out, f"SPLEEN (Spleen 8x16; {spleen_8x16.WIDTH} columns x {HEIGHT} rows, "
                        "A-Z 0-9 and space only)",
                   spleen_glyphs(), PER_LINE)
    for _, label, desc, w, v, h in STYLES:
        render_section(out, f"{label} ({desc}; {w} columns x {HEIGHT} rows)",
                       [(ch, draw(ch, Glyph(w, v, h)).px) for ch in CHARS],
                       PER_LINE)
    render_section(out, f"EMOJI (same in every weight; {EMOJI_WIDTH} columns x {HEIGHT} rows)",
                   [(code, px) for _, code, px in emoji_glyphs()],
                   EMOJI_PER_LINE)
    return "\n".join(out)


# --- C output for the firmware --------------------------------------------

def columns(px):
    """Column-major, bit 15 = top row. font.cpp maps rows onto coils."""
    return [sum(1 << (15 - y) for y in range(HEIGHT) if px[y][x])
            for x in range(len(px[0]))]


def c_string(token):
    # Escape non-ASCII bytes (Æ Ø Å) so the source compiles the same
    # whatever encoding an editor saves it in.
    # A \x escape swallows any hex digits after it, so a high byte followed
    # by an ASCII character closes the literal and opens a new one.
    out, prev_high = "", False
    for b in token.encode("utf-8"):
        c = chr(b)
        if b >= 0x80:
            out += f"\\x{b:02X}"
        else:
            out += ('""' if prev_high else "") + ("\\" + c if c in '\\"' else c)
        prev_high = b >= 0x80
    return '"' + out + '"'


def c_table(lines, ident, entries):
    """entries: [(comment, [tokens], px)]. One FontGlyph per token, tokens
    of the same drawing sharing its columns."""
    col_ident = ident + "_COLUMNS"
    lines.append(f"static const uint16_t {col_ident}[] = {{")
    offsets, offset = [], 0
    for comment, tokens, px in entries:
        cols = columns(px)
        lines.append("  " + ", ".join(f"0x{c:04X}" for c in cols) +
                     f",  // {comment}")
        offsets.append((offset, len(cols)))
        offset += len(cols)
    lines.append("};")
    lines.append("")
    count = 0
    lines.append(f"const FontGlyph {ident}[] = {{")
    for (comment, tokens, _), (off, w) in zip(entries, offsets):
        for t in tokens:
            lines.append(f"  {{{c_string(t)}, {w}, &{col_ident}[{off}]}},")
            count += 1
    lines.append("};")
    lines.append(f"const size_t {ident}_COUNT = {count};")
    lines.append("")


def render_c():
    lines = [
        "// GENERATED by font_design/gen_font.py -- do not edit by hand.",
        "// Regenerate with: python3 font_design/gen_font.py --c src/font_data.cpp",
        '#include "font_data.h"',
        "",
    ]
    c_table(lines, "FONT_SPLEEN",
            [("space" if ch == ' ' else ch, [ch], px) for ch, px in spleen_glyphs()])
    for ident, _, desc, w, v, h in STYLES:
        c_table(lines, ident,
                [("space" if ch == ' ' else ch, [ch],
                  draw(ch, Glyph(w, v, h)).px) for ch in CHARS])
    c_table(lines, "FONT_EMOJI",
            [(code, [code], px) for _, code, px in emoji_glyphs()])
    return "\n".join(lines)


if __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["--c"]:
        text, args = render_c(), args[1:]
    else:
        text = render()
    if args:
        with open(args[0], "w", encoding="utf-8") as f:
            f.write(text)
    else:
        print(text)
