#pragma once
#include <Arduino.h>
#include "drip/engine.h"
#include "drip/ring.h"
#include "font.h"
#include "params.h"

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
void printer_begin(const Settings &settings);

enum class EnqueueResult {
  Ok,
  Empty,      // no glyphs to print (font_glyph_count() == 0)
  TooLong,    // more glyphs than the max text length
  QueueFull,  // already drip::Engine::QUEUE_DEPTH messages waiting
};

// Renders `text` in `font`, with the letter spacing setting, and queues it
// to print after whatever is queued already. Interrupts the default text
// straight away.
EnqueueResult printer_enqueue(const String &text, Font font, bool invert);

// For logs and error replies: "queued", "empty", "too long", "queue full".
const char *enqueue_result_name(EnqueueResult r);

void printer_set_mode(drip::Mode mode);

// Hands new settings to the print engine. They must have passed
// params_check(). A new letter spacing re-renders the default text (which
// restarts if it's printing); messages already queued keep theirs.
void printer_set_settings(const Settings &settings);

drip::Status printer_status();

// --- Telemetry ----------------------------------------------------------------
//
// What the print task did, recorded as it happens into a ring that any task
// can read without disturbing it: the print task never waits on a reader,
// and reading never wakes the print task. Every record gets the next
// sequence number, so a reader that falls behind is told exactly how many
// it missed.

struct Record {
  enum Kind : uint8_t {
    Coils,         // the coils were set: a column, a Trace pulse, Prime, closing
    Skipped,       // the wheel jumped past columns that never fired
    ModeChanged,
    MessageStarted,
    MessageFinished,
  };
  uint8_t kind;
  uint8_t overrun;     // Coils: the previous burst was still open
  uint16_t bits;       // Coils: which are open now, bit 15 = COIL1
  int32_t value;       // Coils: column index, or -1 if not a column.
                       // Skipped: how many. ModeChanged: drip::Mode.
                       // MessageStarted: 1 if the default text.
  int64_t at_us;       // esp_timer_get_time()
  uint32_t open_us;    // Coils: how long they stay open: 0 if bits is 0,
                       // HELD if until the next Coils record (Prime)
  float counts_per_s;  // Coils: speed estimate, 0 if there isn't one yet

  static const uint32_t HELD = UINT32_MAX;
};

const size_t HISTORY_SIZE = 512;
typedef drip::Ring<Record, HISTORY_SIZE> History;

// Copies up to `max` records from sequence number `since` on. Any task.
History::Read printer_history(uint32_t since, Record *out, size_t max);

// The sequence number the next record will get.
uint32_t printer_history_head();

// Changes whenever printer_status() would return something different (mode,
// current message, queue, settings), apart from the column and overrun
// counters. Cheap, any task: poll it to know when to fetch the status.
uint32_t printer_status_version();
