#include "params.h"

namespace drip {

const char *params_validate(const Params &p) {
  if (p.counts_per_column < 1) return "Counts per column must be at least 1.";
  if (p.max_burst_us < 0) return "Max column burst can't be negative.";
  if (p.min_burst_us < 1000 || p.min_burst_us > p.max_burst_us) {
    return "Min column burst must be between 1 ms and the max column burst.";
  }
  if (p.burst_duty_pct < 5 || p.burst_duty_pct > 95) return "Burst duty must be between 5 and 95.";
  if (p.trace_gap_us < 0) return "Trace gap can't be negative.";
  if (p.line_spacing < 1) return "Line spacing must be at least 1 column.";
  if (p.message_pause_us < 0) return "Pause between messages can't be negative.";
  if (p.speed_timeout_us < 1) return "Speed timeout must be positive.";
  if (p.min_column_us < 0) return "Min column time can't be negative.";
  if (p.max_stretch_pct < 100 || p.max_stretch_pct > 1000) {
    return "Max stretch must be between 100 and 1000 %.";
  }
  return nullptr;
}

}  // namespace drip
