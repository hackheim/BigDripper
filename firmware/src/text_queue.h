#pragma once
#include <Arduino.h>
#include <vector>

// FIFO of texts waiting to be printed on the ground. The web server task
// pushes onto the back; the print engine (on VALVE_CORE, once it exists)
// reads the front and calls text_queue_advance() when the encoder has
// scanned past it. All of it is mutex-protected so it's safe to call from
// either core.

const size_t TEXT_QUEUE_MAX_DEPTH = 8;
const size_t TEXT_QUEUE_MAX_LEN = 64;

// Adds a text to the back of the queue. Returns false (no-op) if the text is
// empty, too long, or the queue is already at TEXT_QUEUE_MAX_DEPTH.
bool text_queue_push(const String &text);

// The text currently being printed, i.e. the front of the queue. Empty
// during the inter-print pause after text_queue_advance() and when the
// queue has nothing left.
String text_queue_current();

// Snapshot of everything waiting behind the current text, in print order.
std::vector<String> text_queue_pending();

// Called by the print engine once the encoder has scanned past the current
// text's full length. Pops it and starts a pause; the next text (if any)
// becomes current once TEXT_QUEUE_PAUSE_MS has elapsed. No-op if the queue
// is already empty.
void text_queue_advance();

const uint32_t TEXT_QUEUE_PAUSE_MS = 2000;
