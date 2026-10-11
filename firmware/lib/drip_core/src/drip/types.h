#pragma once
#include <cstdint>

// The vocabulary between the print engine core (engine.h) and the firmware
// around it. The core never reads a clock, a pin or a register: the shell
// tells it where the encoder is and what time it is, and carries out what it
// asks for. That's what lets the whole thing run in host tests.

namespace drip {

// Microseconds on the esp_timer_get_time() timebase. Signed so differences
// don't need care, and 64-bit so it never wraps.
typedef int64_t Micros;

// Raw encoder counts (every quadrature edge, not detents), extended to 64
// bits by the shell. Only differences matter: the core never assumes any
// particular value is "zero".
typedef int64_t Position;

const Micros NEVER = INT64_MAX;
const Position POSITION_MIN = INT64_MIN;
const Position POSITION_MAX = INT64_MAX;

// Where the encoder was, and when it got there. `at` is the time the
// position was reached (the edge or threshold interrupt's timestamp), not
// when the engine got round to looking, so speed estimates aren't polluted
// by task wake-up jitter. The two must be a consistent pair.
struct Sample {
  Position position;
  Micros at;
};

// What the engine wants to be woken by next. The shell calls step() again
// as soon as any one of these holds:
//   - the position leaves [lo, hi]
//   - the time reaches `deadline`
//   - a command was given (Engine::set_mode(), enqueue(), set_params())
// A window of [POSITION_MIN, POSITION_MAX] means "position doesn't matter",
// a deadline of NEVER means "nothing time-based is pending". Standing still
// in a scan mode is a column-sized window and NEVER: no wake-ups at all
// until the wheel moves.
struct Wake {
  Position lo;
  Position hi;
  Micros deadline;
};

// Coil state for the shell to apply right away, replacing whatever was
// there (including a burst that hasn't closed yet). The shell switches
// every coil off at `close_at` unless another command replaces this one
// first; NEVER holds the state until the next command. bits == 0 just
// closes everything.
//
// bit 15 = COIL1 ... bit 0 = COIL16, as font_render() produces.
struct CoilCommand {
  uint16_t bits;
  Micros close_at;
};

// What the print engine is doing. Exactly one at a time.
enum class Mode : uint8_t {
  Off,    // nothing prints, valves closed, the queue is left where it is
  Prime,  // hold every valve open, for flushing/priming the system
  Trace,  // pulse every valve on a timer: burst on, trace gap off, repeat
  Lines,  // encoder-driven test pattern: a full column every N columns
  Text,   // print the text queue, or the default text while it's empty
};

// Things that happened during a step, for the console preview and logging.
// Nothing in the engine depends on anyone reading them.
struct Event {
  enum Kind : uint8_t {
    ModeChanged,      // value: the new Mode
    MessageStarted,   // value: 1 if it's the default text, 0 if queued
    MessageFinished,  // a queued message was scanned to its end
    ColumnFired,      // value: column index; the bits are in StepResult::coils
    ColumnsSkipped,   // value: how many columns the wheel jumped over unfired
    Overrun,          // the previous burst was still open when this column fired
  };
  Kind kind;
  int32_t value;
};

// Everything one Engine::step() asks of the shell.
struct StepResult {
  bool has_coils = false;  // false: leave the coils (and any close timer) alone
  CoilCommand coils = {0, NEVER};
  Wake wake = {POSITION_MIN, POSITION_MAX, NEVER};

  static const int MAX_EVENTS = 8;
  Event events[MAX_EVENTS];
  int event_count = 0;

  void set_coils(uint16_t bits, Micros close_at) {
    has_coils = true;
    coils = {bits, close_at};
  }
  // Drops events past MAX_EVENTS rather than failing: they're diagnostics.
  void add_event(Event::Kind kind, int32_t value = 0) {
    if (event_count < MAX_EVENTS) events[event_count++] = {kind, value};
  }
};

}  // namespace drip
