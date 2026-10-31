// The fixed tick's schedule (M6D, src/runtime/tick_schedule.h): 30 ticks per second, late ticks catch up by at most
// two ticks, a larger backlog is dropped.
#include <catch2/catch_test_macros.hpp>

#include "runtime/tick_schedule.h"

using kuf::pacing::TickSchedule;
using Clock = TickSchedule::Clock;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

TEST_CASE("tick schedule: 30 ticks take exactly one second", "[pacing]") {
  TickSchedule s;
  const auto t0 = Clock::time_point{} + std::chrono::seconds(100);
  CHECK(s.Next(t0) == t0);  // the first tick at once
  Clock::time_point t = t0;
  for (int i = 1; i <= 3000; ++i) {
    t = s.Next(t);  // each tick starts on time
    if (i % 30 == 0) {
      CHECK(t - t0 == std::chrono::seconds(i / 30));
    }
  }
  CHECK(s.drops() == 0);
}

TEST_CASE("tick schedule: a late tick catches up by at most two ticks", "[pacing]") {
  TickSchedule s;
  const auto t0 = Clock::time_point{} + std::chrono::seconds(100);
  s.Next(t0);
  // 50 ms late (1.5 ticks): the next ticks keep their places on the schedule, so the game catches up.
  const auto late = t0 + milliseconds(50);
  const auto t1 = s.Next(late);
  CHECK(t1 == t0 + nanoseconds(TickSchedule::kTickNs));
  CHECK(t1 < late);  // it starts at once
  const auto t2 = s.Next(late);
  CHECK(t2 == t0 + nanoseconds(2 * TickSchedule::kTickNs));
  CHECK(s.drops() == 0);
}

TEST_CASE("tick schedule: after a stall the game continues from now, no faster than real time", "[pacing]") {
  TickSchedule s;
  const auto t0 = Clock::time_point{} + std::chrono::seconds(100);
  s.Next(t0);
  const auto stalled = t0 + milliseconds(500);  // a loading stall
  CHECK(s.Next(stalled) == stalled);
  CHECK(s.drops() == 1);
  CHECK(s.Next(stalled) == stalled + nanoseconds(TickSchedule::kTickNs));  // then 30 per second again
  s.Restart(stalled + std::chrono::seconds(3));
  CHECK(s.Next(stalled + std::chrono::seconds(3)) ==
        stalled + std::chrono::seconds(3) + nanoseconds(TickSchedule::kTickNs + 1));
}
