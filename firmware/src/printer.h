#pragma once
#include <Arduino.h>
#include "drip/engine.h"
#include "font.h"

// The print engine (lib/drip_core) in its own task on PRINT_CORE, together
// with the two interrupts it depends on: the encoder's (PCNT, encoder.h)
// and the burst close timer's (coils.h). WiFi, the web server and
// everything else stays on the other core.
//
// The functions below are the only way in. Each one is a request the print
// task carries out between two engine steps and answers before the call
// returns, so the engine itself never needs a lock and the caller always
// sees the result of what it asked for. Safe to call from any task except
// the print task itself.

#define PRINT_CORE 1

// Starts the print task with the given settings. Printing starts in Text,
// with nothing fired until the wheel moves.
void printer_begin(const drip::Params &params);

enum class EnqueueResult {
  Ok,
  Empty,      // no glyphs to print (font_glyph_count() == 0)
  TooLong,    // more glyphs than the max text length
  QueueFull,  // already drip::Engine::QUEUE_DEPTH messages waiting
};

// Renders `text` in `font` and queues it to print after whatever is queued
// already. Interrupts the default text straight away.
EnqueueResult printer_enqueue(const String &text, Font font, bool invert);

// For logs and error replies: "queued", "empty", "too long", "queue full".
const char *enqueue_result_name(EnqueueResult r);

void printer_set_mode(drip::Mode mode);

// Must have passed drip::params_validate() (params_check() does).
void printer_set_params(const drip::Params &params);

drip::Status printer_status();
