#include "scanner.h"

namespace drip {

// Rounding towards negative infinity, so the column straddling the origin
// isn't twice as wide as the others. b > 0.
static int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b < 0) ? q - 1 : q;
}

static int64_t floor_mod(int64_t a, int64_t b) {
  int64_t m = a % b;
  return m < 0 ? m + b : m;
}

void ColumnScanner::start(const Sample &s, const ColumnSource &src, const Params &p) {
  src_ = src;
  counts_per_column_ = p.counts_per_column > 0 ? p.counts_per_column : 1;
  origin_ = s.position + 1;
  // The column we're standing in counts as done, so only moving fires.
  last_column_ = column_at(s.position);
  fired_ = false;
}

int64_t ColumnScanner::column_at(Position p) const {
  return floor_div(p - origin_, counts_per_column_);
}

int32_t ColumnScanner::total_columns() const {
  if (src_.test_pattern || src_.columns == nullptr) return 0;
  return (int32_t)src_.columns->size();
}

int32_t ColumnScanner::column() const {
  if (!fired_) return -1;
  if (src_.loop && total_columns() > 0) return (int32_t)floor_mod(last_column_, total_columns());
  return (int32_t)last_column_;
}

uint16_t ColumnScanner::bits(int64_t column, const Params &p) const {
  if (src_.test_pattern) {
    int32_t spacing = p.line_spacing > 0 ? p.line_spacing : 1;
    return floor_mod(column, spacing) == 0 ? 0xFFFF : 0x0000;
  }
  int64_t i = src_.loop ? floor_mod(column, total_columns()) : column;
  return (*src_.columns)[(size_t)i];
}

// Scaled to the measured column period so dots stay short and separate at
// speed, capped at max_burst_us (also used until there's a speed estimate)
// and floored at the shortest burst the valves reliably open for.
Micros ColumnScanner::burst_us(const Params &p, const SpeedEstimator &speed) const {
  Micros max_us = p.max_burst_us;
  Micros min_us = p.min_burst_us < max_us ? p.min_burst_us : max_us;
  if (!speed.valid()) return max_us;
  float target = (p.burst_duty_pct / 100.0f) * speed.us_per_count() * counts_per_column_;
  if (target > (float)max_us) return max_us;
  if (target < (float)min_us) return min_us;
  return (Micros)target;
}

ColumnScanner::Result ColumnScanner::step(Micros now, const Sample &s, const Params &p,
                                          const SpeedEstimator &speed, StepResult &out) {
  if (p.counts_per_column > 0 && p.counts_per_column != counts_per_column_) {
    // Column width changed mid-scan: keep the current column where it is
    // and lay the following ones out at the new width from here.
    counts_per_column_ = p.counts_per_column;
    origin_ = s.position - last_column_ * counts_per_column_;
  }

  int64_t c = column_at(s.position);
  Position lo = origin_ + c * counts_per_column_;
  out.wake = {lo, lo + counts_per_column_ - 1, NEVER};
  if (c == last_column_) return Result::Scanning;

  int64_t prev = last_column_;
  last_column_ = c;
  int32_t total = total_columns();
  bool finite = !src_.test_pattern && !src_.loop;

  if (finite && c >= total) return Result::Finished;
  if (!src_.test_pattern && total == 0) return Result::Scanning;  // nothing to print

  // Columns the wheel moved past between two steps never fire; say so.
  // For a message only the ones it actually has count.
  int64_t from = (prev < c ? prev : c) + 1;
  int64_t to = (prev < c ? c : prev) - 1;
  if (finite) {
    if (from < 0) from = 0;
    if (to > total - 1) to = total - 1;
  }
  if (to >= from) out.add_event(Event::ColumnsSkipped, (int32_t)(to - from + 1));

  if (finite && c < 0) return Result::Scanning;  // behind the start

  uint16_t b = bits(c, p);
  if (burst_close_at_ > now) {
    overruns_++;
    out.add_event(Event::Overrun);
  }
  // A blank column still replaces whatever is open: the head has moved
  // off the previous column.
  Micros close_at = b ? now + burst_us(p, speed) : now;
  out.set_coils(b, close_at);
  burst_close_at_ = close_at;
  fired_ = true;
  out.add_event(Event::ColumnFired, column());
  return Result::Scanning;
}

}  // namespace drip
