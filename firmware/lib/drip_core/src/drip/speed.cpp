#include "speed.h"

namespace drip {

constexpr float SpeedEstimator::ALPHA;

void SpeedEstimator::reset() {
  have_last_ = false;
  valid_ = false;
}

void SpeedEstimator::update(const Sample &s, Micros timeout) {
  if (have_last_ && s.position == last_.position) return;

  if (have_last_) {
    Micros dt = s.at - last_.at;
    Position dp = s.position - last_.position;
    if (dp < 0) dp = -dp;
    if (dt > timeout || dt <= 0) {
      // Stopped for a while (or an out-of-order sample): the first sample
      // after the gap is a new starting point, not a speed.
      valid_ = false;
    } else {
      float sample = (float)dt / (float)dp;
      us_per_count_ = valid_ ? ALPHA * sample + (1.0f - ALPHA) * us_per_count_ : sample;
      valid_ = true;
    }
  }
  last_ = s;
  have_last_ = true;
}

}  // namespace drip
