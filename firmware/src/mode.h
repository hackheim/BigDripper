#pragma once
#include <Arduino.h>

// What the print engine is doing. Exactly one mode at a time: turning one on
// turns the others off, and Off means nothing prints at all (valves closed,
// text queue left where it is).
//
// Set by the web server (on WEB_CORE) and read every cycle by the print
// engine (print_engine_run(), on VALVE_CORE), so it's a plain atomic rather
// than mutex-protected like the rest of the shared state in this project.
//
// Starts as Text and isn't persisted, so a reboot always comes back printing
// text with every valve closed -- in particular Prime never comes back on by
// itself after a reset.
enum class PrintMode {
  Off,
  Prime,  // hold every valve open, for flushing/priming the system
  Trace,  // pulse every valve: column burst on, trace gap off, repeat
  Lines,  // encoder-driven test pattern (test_pattern_bits())
  Text,   // print the text queue
};

PrintMode mode_get();
void mode_set(PrintMode mode);

// "off", "prime", "trace", "lines", "text" -- the names /mode and /status use.
const char *mode_name(PrintMode mode);

// Inverse of mode_name(). Returns false (and leaves *out alone) for anything
// else.
bool mode_from_name(const String &name, PrintMode *out);
