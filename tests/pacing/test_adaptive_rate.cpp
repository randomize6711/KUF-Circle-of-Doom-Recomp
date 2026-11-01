// M8 adaptive frame rate (src/runtime/adaptive_rate.h): the frames per tick follow the GPU's time per frame, down after
// half a second, up one step after two seconds, never above the limit or below one frame per tick.
#include <catch2/catch_test_macros.hpp>

#include "runtime/adaptive_rate.h"

using kuf::pacing::AdaptiveRate;

namespace {
constexpr double kBudget = 1000.0 / 30.0 * 0.93;  // the default share of a tick (31.0 ms)

int Run(AdaptiveRate& a, int ticks, int limit_n, double gpu_ms) {
  int n = 0;
  for (int i = 0; i < ticks; ++i) n = a.Update(limit_n, gpu_ms, kBudget);
  return n;
}
}  // namespace

TEST_CASE("adaptive rate: the limit while the GPU keeps up, or before it is measured", "[pacing]") {
  AdaptiveRate a;
  CHECK(a.Update(4, 0.0, kBudget) == 4);  // not measured
  CHECK(Run(a, 600, 4, 6.0) == 4);       // 4 x 6 ms fits a tick
  CHECK(Run(a, 600, 4, 7.7) == 4);       // 4 x 7.7 = 30.8 ms: still within the budget
}

TEST_CASE("adaptive rate: down to the sustained rate after half a second", "[pacing]") {
  AdaptiveRate a;
  CHECK(Run(a, AdaptiveRate::kTicksDown - 1, 4, 10.5) == 4);  // not yet
  CHECK(a.Update(4, 10.5, kBudget) == 2);                      // 31.0 / 10.5 = 2.95: 2 frames per tick (60 fps)
  AdaptiveRate b;
  CHECK(Run(b, AdaptiveRate::kTicksDown, 4, 10.0) == 3);  // 3.1: 90 fps
  AdaptiveRate c;
  CHECK(Run(c, AdaptiveRate::kTicksDown, 4, 40.0) == 1);  // never below one frame per tick
}

TEST_CASE("adaptive rate: a short spike does not change the rate", "[pacing]") {
  AdaptiveRate a;
  Run(a, 300, 4, 6.0);
  for (int i = 0; i < 20; ++i) {
    // 10 ticks slow, 1 normal: the run of slow ticks restarts each time
    for (int k = 0; k < AdaptiveRate::kTicksDown - 5; ++k) CHECK(a.Update(4, 14.0, kBudget) == 4);
    CHECK(a.Update(4, 6.0, kBudget) == 4);
  }
}

TEST_CASE("adaptive rate: up one step after two seconds, up to the limit", "[pacing]") {
  AdaptiveRate a;
  CHECK(Run(a, AdaptiveRate::kTicksDown, 4, 14.0) == 2);  // 31 / 14 = 2.2
  CHECK(Run(a, AdaptiveRate::kTicksUp - 1, 4, 6.0) == 2);
  CHECK(a.Update(4, 6.0, kBudget) == 3);  // one step
  CHECK(Run(a, AdaptiveRate::kTicksUp, 4, 6.0) == 4);
  CHECK(Run(a, 1000, 4, 1.0) == 4);  // never above the limit
}

TEST_CASE("adaptive rate: a lower limit applies at once; reset starts from the limit", "[pacing]") {
  AdaptiveRate a;
  CHECK(Run(a, 10, 4, 6.0) == 4);
  CHECK(a.Update(2, 6.0, kBudget) == 2);  // the limit changed to 60
  a.Reset();
  CHECK(a.Update(8, 0.0, kBudget) == 8);  // 240 from the start
}
