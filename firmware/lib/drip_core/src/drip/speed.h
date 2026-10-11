#pragma once
#include "types.h"

namespace drip {

// Estimates how fast the head moves over the ground from the samples the
// engine is woken with, as microseconds per encoder count.
//
// It doesn't need a sample per edge: any two samples at different positions
// give a speed, so it works with whatever window size the engine asks for.
// With column-sized windows that's one sample per column, the resolution
// the burst is sized at anyway. Each sample is weighted the same in the
// EMA, whatever distance it spans.
class SpeedEstimator {
 public:
  // Forget everything, e.g. when a mode switch makes old samples meaningless.
  void reset();

  // Feed the latest sample. Repeats of the previous position are ignored,
  // and a gap longer than `timeout` since the previous one (stopped)
  // invalidates the estimate rather than being averaged in.
  void update(const Sample &s, Micros timeout);

  // False until two samples close enough in time have been seen.
  bool valid() const { return valid_; }

  // Smoothed time per count. Only meaningful while valid().
  float us_per_count() const { return us_per_count_; }

 private:
  static constexpr float ALPHA = 0.3f;

  bool have_last_ = false;
  Sample last_ = {0, 0};
  bool valid_ = false;
  float us_per_count_ = 0;
};

}  // namespace drip
