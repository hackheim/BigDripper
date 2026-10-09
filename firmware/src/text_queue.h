#pragma once
#include <Arduino.h>
#include <vector>
#include "font.h"

// FIFO of texts waiting to be printed on the ground. The web server task
// pushes onto the back; the print engine (on VALVE_CORE, once it exists)
// reads the front and calls text_queue_advance() when the encoder has
// scanned past it. All of it is mutex-protected so it's safe to call from
// either core.

const size_t TEXT_QUEUE_MAX_DEPTH = 8;

// A message and the font it was queued with; it prints in that font.
struct QueuedText {
  String text;
  Font font;
};

// Adds a text to the back of the queue. Returns false (no-op) if the text
// prints nothing (empty, or only `*` markup), prints more than
// params_get_max_text_len() glyphs (font_glyph_count()), or the queue is
// already at TEXT_QUEUE_MAX_DEPTH.
bool text_queue_push(const String &text, Font font);

// The message currently being printed, i.e. the front of the queue. Its
// text is empty during the inter-print pause after text_queue_advance() and
// when the queue has nothing left.
QueuedText text_queue_current();

// True when there's nothing to print at all: the queue is empty and no
// inter-print pause is running. The print engine plays
// TEXT_QUEUE_DEFAULT_TEXT on repeat while this holds.
bool text_queue_is_idle();

// Printed on a loop, in TEXT_QUEUE_DEFAULT_FONT, whenever the queue is idle.
extern const char TEXT_QUEUE_DEFAULT_TEXT[];
const Font TEXT_QUEUE_DEFAULT_FONT = Font::Drip;

// Snapshot of everything waiting behind the current text, in print order.
std::vector<QueuedText> text_queue_pending();

// Called by the print engine once the encoder has scanned past the current
// text's full length. Pops it and starts a pause; the next text (if any)
// becomes current once params_get_print_pause_ms() has elapsed. No-op if the
// queue is already empty.
void text_queue_advance();
