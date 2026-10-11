#pragma once
#include "types.h"

namespace drip {

// Everything the engine's behaviour depends on, as one value. The engine
// only ever sees a whole, validated Params (Engine::set_params()), so it
// can't fire a burst with half-old, half-new settings.
//
// The defaults match the firmware's NVS defaults (params.cpp).
struct Params {
  // Encoder counts per printed column. 400 = 100 detents of 4 counts.
  int32_t counts_per_column = 400;

  // Longest a column's valves stay open. Also the Trace pulse length.
  Micros max_burst_us = 20000;

  // Shortest burst the valves reliably open for. <= max_burst_us.
  Micros min_burst_us = 4000;

  // Percent of the measured column period the burst targets, so dots stay
  // separate at speed. Clamped to [min_burst_us, max_burst_us].
  uint32_t burst_duty_pct = 50;

  // Trace only: how long every valve stays closed between pulses.
  Micros trace_gap_us = 1000000;

  // Lines only: a full column every this many columns. 1 = every column.
  int32_t line_spacing = 10;

  // Pause between one queued message finishing and the next one starting.
  Micros message_pause_us = 2000000;

  // No column change for this long and the speed estimate is forgotten
  // (the bike stopped), so the next burst falls back to max_burst_us.
  Micros speed_timeout_us = 500000;
};

// nullptr if `p` is usable, otherwise a reason fit to show a user.
const char *params_validate(const Params &p);

}  // namespace drip
