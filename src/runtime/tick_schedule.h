// The fixed tick's schedule (M6D, frame_pacing.cpp): the times at which the game may start its ticks, 30 per second
// from an absolute start (every third step is one nanosecond longer, so 30 ticks take exactly one second). A tick
// that starts late starts at once (the game catches up), but a backlog of more than kMaxBacklogTicks is dropped: the
// schedule continues from now, so after a stall (loading, a slow frame) the game does not run faster than real time.
#pragma once

#include <chrono>
#include <cstdint>

namespace kuf::pacing {

class TickSchedule {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr int64_t kTickNs = 1000000000 / 30;  // 33,333,333
  static constexpr int kMaxBacklogTicks = 2;

  // The time the next tick may start, given the current time (the first call returns `now`).
  Clock::time_point Next(Clock::time_point now) {
    if (!started_) {
      started_ = true;
      next_ = now;
      return next_;
    }
    next_ += std::chrono::nanoseconds(kTickNs + (++steps_ % 3 == 0 ? 1 : 0));
    if (now - next_ > std::chrono::nanoseconds(kTickNs * kMaxBacklogTicks)) {
      next_ = now;
      ++drops_;
    }
    return next_;
  }

  // Starts again from `now` (after the game was held on purpose).
  void Restart(Clock::time_point now) { next_ = now; }

  // When the tick after the one Next() last returned is due (without a backlog drop).
  Clock::time_point Following() const { return next_ + std::chrono::nanoseconds(kTickNs); }

  uint64_t drops() const { return drops_; }

 private:
  bool started_ = false;
  Clock::time_point next_{};
  uint64_t steps_ = 0;
  uint64_t drops_ = 0;
};

}  // namespace kuf::pacing
