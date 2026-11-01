// M8 adaptive frame rate (--kuf_frame_rate_adaptive, src/runtime/frame_pacing.cpp; docs/PERFORMANCE.md section 7):
// the frames per game tick the GPU sustains at a frame-rate limit that is a multiple of 30. Header-only and free of the
// runtime so the unit tests (tests/pacing) cover its rules.
#pragma once

#include <algorithm>

namespace kuf::pacing {

class AdaptiveRate {
 public:
  static constexpr int kTicksDown = 15;  // half a second below the rate in effect: down to the sustained rate
  static constexpr int kTicksUp = 60;    // two seconds above it: up one step

  // Once per game tick. limit_n: the limit's frames per tick (limit / 30); gpu_ms: the GPU's recent time per frame
  // (0: not measured, nothing changes); budget_ms: the share of a tick the frames may use. Returns the frames per tick
  // to draw.
  int Update(int limit_n, double gpu_ms, double budget_ms) {
    limit_n = std::max(1, limit_n);
    n_ = n_ > 0 ? std::min(n_, limit_n) : limit_n;
    if (gpu_ms <= 0.0) {
      return n_;
    }
    const int sustained = std::clamp(int(budget_ms / gpu_ms), 1, limit_n);
    below_ = sustained < n_ ? below_ + 1 : 0;
    above_ = sustained > n_ ? above_ + 1 : 0;
    if (below_ >= kTicksDown) {
      n_ = sustained;
      below_ = above_ = 0;
    } else if (above_ >= kTicksUp) {
      ++n_;
      below_ = above_ = 0;
    }
    return n_;
  }
  void Reset() { n_ = below_ = above_ = 0; }
  int n() const { return n_; }

 private:
  int n_ = 0;  // frames per tick in effect, 0 = not started (the limit)
  int below_ = 0, above_ = 0;  // consecutive ticks the sustained rate was below / above it
};

}  // namespace kuf::pacing
