#pragma once
#include <Arduino.h>
#include "mode.h"
#include "text_queue.h"

// Bench preview of what the head prints (Ticket 7), chosen at compile time:
//
//   -D CONSOLE_PREVIEW=1          every column that fires is drawn on Serial0
//                                 (the CP2102 port) as one line of 16 chars,
//                                 COIL1 on the left, '#' = open, '.' = closed.
//   -D CONSOLE_PREVIEW_DRY_RUN=1  set_coil_state() never touches the GPIOs, so
//                                 no valve opens in any mode.
//
// The `preview` and `preview-dry` envs in platformio.ini set these. In a
// normal build every function here is an empty inline, so the print engine
// calls them without #ifdefs and gets no task, queue or code from it.
//
// The print engine never writes to the UART itself: each call is a
// zero-timeout push onto a FreeRTOS queue that a low-priority task on core 0
// drains. A full queue drops the line (reported later as "!! dropped N
// lines") instead of delaying the valves.

#if defined(CONSOLE_PREVIEW) || defined(CONSOLE_PREVIEW_DRY_RUN)
// From setup(). Starts Serial0, prints the dry-run banner in dry-run builds,
// and starts the drain task in preview builds.
void console_preview_begin();
#else
inline void console_preview_begin() {}
#endif

#ifdef CONSOLE_PREVIEW
// A column's coil bits (bit 15 = COIL1), as written to the valves.
void console_preview_column(uint16_t bits);
// A new message became current: "-- HELLO [Drip] --".
void console_preview_message(const QueuedText &m, bool is_default);
// The toolbar mode changed: "-- mode: prime --".
void console_preview_mode(PrintMode mode);
// The encoder moved past n columns between two scans; they never fired.
void console_preview_skipped(int32_t n);
#else
inline void console_preview_column(uint16_t) {}
inline void console_preview_message(const QueuedText &, bool) {}
inline void console_preview_mode(PrintMode) {}
inline void console_preview_skipped(int32_t) {}
#endif
