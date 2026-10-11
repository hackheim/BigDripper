#include "scanner.h"

namespace drip {

static int64_t floor_mod(int64_t a, int64_t b) {
  int64_t m = a % b;
  return m < 0 ? m + b : m;
}

void ColumnScanner::start(const Sample &s, const ColumnSource &src) {
  src_ = src;
  column_ = -1;
  next_start_ = s.position + 1;
  width_ = 0;
  fired_ = false;
}

int32_t ColumnScanner::total_columns() const {
  if (src_.test_pattern || src_.columns == nullptr) return 0;
  return (int32_t)src_.columns->size();
}

int32_t ColumnScanner::column() const {
  if (!fired_) return -1;
  if (src_.loop && total_columns() > 0) return (int32_t)floor_mod(column_, total_columns());
  return (int32_t)column_;
}

uint16_t ColumnScanner::bits(int64_t column, const Params &p) const {
  if (src_.test_pattern) {
    int32_t spacing = p.line_spacing > 0 ? p.line_spacing : 1;
    return floor_mod(column, spacing) == 0 ? 0xFFFF : 0x0000;
  }
  int64_t i = src_.loop ? floor_mod(column, total_columns()) : column;
  return (*src_.columns)[(size_t)i];
}

// counts_per_column, widened at speed so the column takes at least
// min_column_us to cross, but never past max_stretch_pct of it.
int32_t ColumnScanner::width(const Params &p, const SpeedEstimator &speed) const {
  int32_t base = p.counts_per_column > 0 ? p.counts_per_column : 1;
  if (p.min_column_us <= 0 || !speed.valid() || speed.us_per_count() <= 0) return base;
  float want = (float)p.min_column_us / speed.us_per_count();
  float cap = (float)base * p.max_stretch_pct / 100.0f;
  if (want > cap) want = cap;
  return want > (float)base ? (int32_t)(want + 0.5f) : base;
}

// Scaled to the time the column takes to cross, so dots stay short and
// separate at speed, capped at max_burst_us (also used until there's a
// speed estimate) and floored at the shortest burst the valves reliably
// open for.
Micros ColumnScanner::burst_us(const Params &p, const SpeedEstimator &speed, int32_t width) const {
  Micros max_us = p.max_burst_us;
  Micros min_us = p.min_burst_us < max_us ? p.min_burst_us : max_us;
  if (!speed.valid()) return max_us;
  float target = (p.burst_duty_pct / 100.0f) * speed.us_per_count() * width;
  if (target > (float)max_us) return max_us;
  if (target < (float)min_us) return min_us;
  return (Micros)target;
}

ColumnScanner::Result ColumnScanner::step(Micros now, const Sample &s, const Params &p,
                                          const SpeedEstimator &speed, StepResult &out) {
  // Nothing to do until the position reaches the next column; moving back
  // never wakes us.
  if (s.position < next_start_) {
    out.wake = {POSITION_MIN, next_start_ - 1, NEVER};
    return Result::Scanning;
  }

  // Into the next column, or past several if the wheel got ahead of us:
  // they're all laid out at the width for the current speed.
  int32_t w = width(p, speed);
  int64_t n = (s.position - next_start_) / w + 1;
  int64_t prev = column_;
  column_ += n;
  next_start_ += n * w;
  width_ = w;
  out.wake = {POSITION_MIN, next_start_ - 1, NEVER};

  int32_t total = total_columns();
  bool finite = !src_.test_pattern && !src_.loop;
  if (finite && column_ >= total) return Result::Finished;
  if (!src_.test_pattern && total == 0) return Result::Scanning;  // nothing to print

  // Columns the wheel moved past between two steps never fire; say so.
  // For a message only the ones it actually has count.
  int64_t from = prev + 1, to = column_ - 1;
  if (finite && to > total - 1) to = total - 1;
  if (to >= from) out.add_event(Event::ColumnsSkipped, (int32_t)(to - from + 1));

  uint16_t b = bits(column_, p);
  if (burst_close_at_ > now) {
    overruns_++;
    out.add_event(Event::Overrun);
  }
  // A blank column still replaces whatever is open: the head has moved
  // off the previous column.
  Micros close_at = b ? now + burst_us(p, speed, w) : now;
  out.set_coils(b, close_at);
  burst_close_at_ = close_at;
  fired_ = true;
  out.add_event(Event::ColumnFired, column());
  return Result::Scanning;
}

}  // namespace drip
