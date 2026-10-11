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
// the burst sized from the measured speed, and asks to be woken at the
// column's edges. Alternatives (opening early ahead of a predicted
// crossing, printing only while moving forwards) would replace this class;
// the engine around it doesn't change.
//
// Columns are laid out from an origin: column c covers positions
// [origin + c * counts_per_column, origin + (c + 1) * counts_per_column).
class ColumnScanner {
 public:
  enum class Result {
    Scanning,
    Finished,  // a non-looping source was scanned past its last column
  };

  // Starts scanning `src` with column 0 beginning one count forward of `s`.
  // Nothing fires until the wheel moves into it, so starting a scan while
  // standing still never sprays anything.
  void start(const Sample &s, const ColumnSource &src, const Params &p);

  // Fires the column `s` is in, if it's a new one, and sets out.wake to its
  // edges. Doesn't touch the coils otherwise: an open burst closes on its
  // own timer.
  Result step(Micros now, const Sample &s, const Params &p, const SpeedEstimator &speed,
              StepResult &out);

  // The column last scanned, as an index into the source (wrapped for a
  // looping one). -1 before the first column.
  int32_t column() const;

  // Columns the source has, 0 for the test pattern.
  int32_t total_columns() const;

  // Columns that fired while the previous burst was still open, i.e. bursts
  // outrunning the column period. Kept across scans; diagnostic only.
  uint32_t overruns() const { return overruns_; }

 private:
  int64_t column_at(Position p) const;
  uint16_t bits(int64_t column, const Params &p) const;
  Micros burst_us(const Params &p, const SpeedEstimator &speed) const;

  ColumnSource src_;
  Position origin_ = 0;
  int32_t counts_per_column_ = 1;
  int64_t last_column_ = -1;
  bool fired_ = false;
  Micros burst_close_at_ = 0;
  uint32_t overruns_ = 0;
};

}  // namespace drip
