#pragma once
#include <vector>
#include "params.h"
#include "speed.h"
#include "types.h"

namespace drip {

// What a scan prints: one of a message's rendered columns, the default
// text on a loop, or the Lines test pattern.
struct ColumnSource {
  // A message's rendered columns (font_render()). Must outlive the scan.
  // Ignored for the test pattern.
  const std::vector<uint16_t> *columns = nullptr;

  // Wrap around forever instead of finishing after the last column.
  bool loop = false;

  // The Lines test pattern instead of `columns`: a full column every
  // Params::line_spacing columns, read as each column fires so a change
  // applies from the next one on. Never finishes.
  bool test_pattern = false;
};

// The firing strategy for the encoder-driven modes (Text, Lines): turns the
// encoder position into columns, decides when each one fires and for how
// long, and which window to be woken by next.
//
// This one fires a column the moment the position crosses into it, with
// the burst sized from the measured speed, and asks to be woken when the
// position reaches the next column. Alternatives (opening early ahead of a
// predicted crossing) would replace this class; the engine around it
// doesn't change.
//
// Columns are laid out one at a time, each as it's entered: its width is
// Params::counts_per_column, stretched at speed so a column never takes
// less than Params::min_column_us (up to Params::max_stretch_pct). So the
// text gets wider on the ground as the bike speeds up, rather than its
// columns getting too short for the valves.
//
// Printing only goes forwards. Rolling back fires nothing, and nothing
// fires again until the wheel is back past the furthest point it reached;
// then the next column carries on from there.
class ColumnScanner {
 public:
  enum class Result {
    Scanning,
    Finished,  // a non-looping source was scanned past its last column
  };

  // Starts scanning `src` with column 0 beginning one count forward of `s`.
  // Nothing fires until the wheel moves into it, so starting a scan while
  // standing still never sprays anything.
  void start(const Sample &s, const ColumnSource &src);

  // Fires the next column if `s` has reached it, and sets out.wake to wait
  // for the one after. Doesn't touch the coils otherwise: an open burst
  // closes on its own timer.
  Result step(Micros now, const Sample &s, const Params &p, const SpeedEstimator &speed,
              StepResult &out);

  // The column last scanned, as an index into the source (wrapped for a
  // looping one). -1 before the first column.
  int32_t column() const;

  // Columns the source has, 0 for the test pattern.
  int32_t total_columns() const;

  // Width of the last column entered, in counts: counts_per_column, or
  // more when stretched.
  int32_t column_width() const { return width_; }

  // Columns that fired while the previous burst was still open, i.e. bursts
  // outrunning the column period. Kept across scans; diagnostic only.
  uint32_t overruns() const { return overruns_; }

 private:
  uint16_t bits(int64_t column, const Params &p) const;
  int32_t width(const Params &p, const SpeedEstimator &speed) const;
  Micros burst_us(const Params &p, const SpeedEstimator &speed, int32_t width) const;

  ColumnSource src_;
  int64_t column_ = -1;     // last column entered; -1 before the first
  Position next_start_ = 0;  // where the column after it begins
  int32_t width_ = 0;
  bool fired_ = false;
  Micros burst_close_at_ = 0;
  uint32_t overruns_ = 0;
};

}  // namespace drip
