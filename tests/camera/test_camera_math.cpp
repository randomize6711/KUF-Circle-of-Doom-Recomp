// The modern camera's math (src/runtime/camera_math.cpp): stick response, vertical view, basis restore.

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "runtime/camera_math.h"

using Catch::Approx;
using namespace kuf::camera;

namespace {

double Degrees(double radians) { return radians * 180.0 / M_PI; }
double Length(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }

// The game's default action camera, as traced (eye 110 units up, ~700 behind the target near the feet).
const Vec3 kEye{1863.74, 109.5, 2276.43}, kTarget{1370.7, -0.5, 1781.74};

}  // namespace

TEST_CASE("stick: inside the dead zone nothing, full deflection full speed, direction kept", "[camera]") {
  CHECK(ShapeStick(0, 0, 0.25).x == 0);
  CHECK(ShapeStick(8000, 0, 0.25).x == 0);  // 24 % < 25 %
  const auto full = ShapeStick(32767, 0, 0.25);
  CHECK(full.x == Approx(1.0));
  CHECK(full.y == Approx(0.0));
  const auto diagonal = ShapeStick(23170, 23170, 0.25);  // full deflection at 45 degrees
  CHECK(diagonal.x == Approx(diagonal.y));
  CHECK(std::hypot(diagonal.x, diagonal.y) == Approx(1.0).epsilon(0.001));
  CHECK(ShapeStick(-32768, 0, 0.25).x == Approx(-1.0));
}

TEST_CASE("stick: smooth response, no jump at the dead zone edge, monotonic", "[camera]") {
  double previous = 0;
  for (int raw = 8200; raw <= 32767; raw += 500) {
    const double v = ShapeStick(int16_t(raw), 0, 0.25).x;
    CHECK(v >= previous);
    previous = v;
  }
  CHECK(ShapeStick(8300, 0, 0.25).x < 0.01);           // just outside the dead zone: barely moving
  // 62.5 % deflection with a 25 % dead zone is half of the remaining travel: 0.5 ^ 1.5.
  CHECK(ShapeStick(20480, 0, 0.25).x == Approx(std::pow(0.5, kStickExponent)).epsilon(0.001));
  CHECK(ShapeStick(16384, 0, 0.0).x == Approx(std::pow(0.5, kStickExponent)).epsilon(0.001));  // no dead zone
}

TEST_CASE("speeds follow the setting levels and are clamped", "[camera]") {
  // Levels 1..10 are the first M6B build's (saved settings keep their speed): fine, even steps.
  for (int level = 1; level <= 10; ++level) {
    CHECK(Degrees(MouseRadiansPerCount(level)) == Approx(0.025 * level));
    CHECK(Degrees(StickRadiansPerSecond(level)) == Approx(15.0 * level));
  }
  CHECK(Degrees(MouseRadiansPerCount(4)) == Approx(0.1));   // the defaults
  CHECK(Degrees(StickRadiansPerSecond(5)) == Approx(75));
  // 11..20: each level a fixed factor faster, to the new maxima (6x and 4x the old ones).
  CHECK(Degrees(MouseRadiansPerCount(20)) == Approx(1.5));
  CHECK(Degrees(StickRadiansPerSecond(20)) == Approx(600));
  for (int level = 11; level <= 20; ++level) {
    const double mouse = MouseRadiansPerCount(level) / MouseRadiansPerCount(level - 1);
    const double stick = StickRadiansPerSecond(level) / StickRadiansPerSecond(level - 1);
    CHECK(mouse == Approx(std::pow(6.0, 0.1)));
    CHECK(stick == Approx(std::pow(4.0, 0.1)));
  }
  // Relative steps stay small at the top (about 20 % for the mouse, 15 % for the stick: no jump past level 10).
  CHECK(MouseRadiansPerCount(11) / MouseRadiansPerCount(10) < 1.25);
  CHECK(StickRadiansPerSecond(11) / StickRadiansPerSecond(10) < 1.25);
  CHECK(Degrees(MouseRadiansPerCount(0)) == Approx(0.025));  // below range: level 1
  CHECK(Degrees(MouseRadiansPerCount(99)) == Approx(1.5));   // above range: level 20
  CHECK(Degrees(StickRadiansPerSecond(99)) == Approx(600));
}

TEST_CASE("position smoothing around the target: the game's straight line without a turn, a circle with one",
          "[camera]") {
  // The same direction: exactly the game's eye + (new - eye) / n.
  const Vec3 a{0, 100, 700}, b{0, 50, 400};
  const Vec3 s = SmoothAround(a, b, 0.25);
  CHECK(s.x == 0);
  CHECK(s.y == Approx(87.5));
  CHECK(s.z == Approx(625));
  // A 150-degree turn in one frame: a straight line would pass within 181 units of the target (707 * cos 75);
  // around it every step keeps the distance.
  const Vec3 from{0, 0, 707};
  const Vec3 to{707 * std::sin(Radians(150)), 0, 707 * std::cos(Radians(150))};
  Vec3 now = from;
  for (int frame = 0; frame < 30; ++frame) {
    now = SmoothAround(now, to, 0.25);
    CHECK(std::hypot(now.x, now.z) == Approx(707));
  }
  CHECK(now.x == Approx(to.x).margin(0.5));
  CHECK(now.z == Approx(to.z).margin(0.5));
  // The first step turns a quarter of the way (37.5 degrees), the short way round.
  const Vec3 first = SmoothAround(from, to, 0.25);
  CHECK(Degrees(std::atan2(first.x, first.z)) == Approx(37.5));
  const Vec3 left{707 * std::sin(Radians(-170)), 0, 707 * std::cos(Radians(-170))};
  const Vec3 right{707 * std::sin(Radians(170)), 0, 707 * std::cos(Radians(170))};
  CHECK(std::abs(Degrees(std::atan2(SmoothAround(left, right, 0.5).x, SmoothAround(left, right, 0.5).z))) ==
        Approx(180).margin(1e-6));
  // Changing distance and direction together: the distance moves the same fraction.
  const Vec3 near{400 * std::sin(Radians(90)), 0, 400 * std::cos(Radians(90))};
  CHECK(std::hypot(SmoothAround(from, near, 0.25).x, SmoothAround(from, near, 0.25).z) == Approx(707 - 0.25 * 307));
}

TEST_CASE("a collision correction turns with the camera's arm", "[camera]") {
  // The arm turns 90 degrees about the vertical: a correction towards the pivot stays towards it.
  const Vec3 r = RotateWith({0, 0, -100}, {0, 0, 700}, {700, 0, 0});
  CHECK(r.x == Approx(-100));
  CHECK(r.y == Approx(0).margin(1e-9));
  CHECK(r.z == Approx(0).margin(1e-9));
  // An upward push stays upward under a horizontal turn; lengths are kept.
  const Vec3 up = RotateWith({0, 30, 0}, {0, 0, 1}, {std::sin(1.0), 0, std::cos(1.0)});
  CHECK(up.y == Approx(30));
  CHECK(std::hypot(up.x, up.z) == Approx(0).margin(1e-9));
  const Vec3 any = RotateWith({3, -4, 12}, {1, 2, 3}, {-2, 0.5, 1});
  CHECK(std::hypot(any.x, any.y, any.z) == Approx(13));
  // No turn, or a degenerate arm: unchanged.
  const Vec3 same = RotateWith({3, -4, 12}, {1, 2, 3}, {2, 4, 6});
  CHECK(same.x == Approx(3));
  CHECK(same.z == Approx(12));
  CHECK(RotateWith({3, -4, 12}, {0, 0, 0}, {1, 0, 0}).x == 3);
}

TEST_CASE("field of view: the setting is the gameplay camera's horizontal angle at 16:9", "[camera]") {
  // The game's own: unchanged (0.6 rad vertical = 57.6 degrees horizontal at 16:9).
  CHECK(VerticalFovFor(0.6, 0) == 0.6);
  CHECK(Degrees(HorizontalFov(0.6, 16.0 / 9.0)) == Approx(57.6).margin(0.05));
  for (int h = 60; h <= 110; h += 5) {
    const double v = VerticalFovFor(0.6, h);
    CHECK(Degrees(HorizontalFov(v, 16.0 / 9.0)) == Approx(h).margin(1e-9));  // exact at 16:9
    // Another key (the game's narrower 0.5 rad camera) keeps its ratio to the gameplay camera (tan of half angles).
    const double other = VerticalFovFor(0.5, h);
    CHECK(std::tan(other / 2) / std::tan(0.25) == Approx(std::tan(v / 2) / std::tan(0.3)));
    // Wider screens: the vertical angle is kept, so 21:9 sees more at the sides and nothing is stretched.
    CHECK(HorizontalFov(v, 21.0 / 9.0) > HorizontalFov(v, 16.0 / 9.0));
    CHECK(std::tan(HorizontalFov(v, 21.0 / 9.0) / 2) / std::tan(v / 2) == Approx(21.0 / 9.0));
  }
  CHECK(Degrees(VerticalFovFor(0.6, 75)) == Approx(46.69).margin(0.01));  // 2 atan(tan(37.5) * 9 / 16)
  CHECK(VerticalFovFor(0.6, 60) < VerticalFovFor(0.6, 110));
  CHECK(VerticalFovFor(0.0, 90) == 0.0);  // degenerate key: left alone
}

// The pivot's height above the character's feet for the traced camera (key target offset) less the clearance.
constexpr double kDepth = 180.0 - 30.0, kMinDistance = 80.0;

TEST_CASE("orbit: offset 0 is exactly the game's camera", "[camera]") {
  const auto view = ApplyOrbit(kEye, kTarget, 0.0, Radians(85), kDepth, kMinDistance);
  CHECK(view.eye.x == Approx(kEye.x));
  CHECK(view.eye.y == Approx(kEye.y));
  CHECK(view.eye.z == Approx(kEye.z));
  CHECK(view.target.y == kTarget.y);
  CHECK(view.offset == Approx(0).margin(1e-12));
}

TEST_CASE("orbit: looking down raises the eye over the character to the limit, distance kept", "[camera]") {
  const double base = ViewPitch(kEye, kTarget);
  CHECK(Degrees(base) == Approx(-9.0).margin(0.2));
  const auto view = ApplyOrbit(kEye, kTarget, Radians(-60), Radians(85), kDepth, kMinDistance);
  CHECK(Degrees(ViewPitch(view.eye, view.target)) == Approx(Degrees(base) - 60).margin(0.01));
  CHECK(Length(view.eye, view.target) == Approx(Length(kEye, kTarget)));
  const auto limited = ApplyOrbit(kEye, kTarget, Radians(-150), Radians(85), kDepth, kMinDistance);
  CHECK(Degrees(ViewPitch(limited.eye, limited.target)) == Approx(-85).margin(0.01));
  CHECK(Degrees(limited.offset) == Approx(-85 - Degrees(base)).margin(0.01));
  CHECK(limited.eye.y > kTarget.y + 0.99 * Length(kEye, kTarget));  // nearly straight above
  CHECK(std::atan2(limited.eye.x - kTarget.x, limited.eye.z - kTarget.z) ==
        Approx(std::atan2(kEye.x - kTarget.x, kEye.z - kTarget.z)));  // yaw unchanged
}

TEST_CASE("orbit: looking up lowers the eye below the pivot, still looking at it (seen from below)", "[camera]") {
  const double base = ViewPitch(kEye, kTarget);
  for (double up : {5.0, 20.0, 45.0, 70.0, 85.0}) {
    const auto view = ApplyOrbit(kEye, kTarget, Radians(up) - base, Radians(85), kDepth, kMinDistance);
    CAPTURE(up);
    CHECK(view.target.x == kTarget.x);  // always aimed at the pivot: never a tilt away from the character
    CHECK(view.target.y == kTarget.y);
    CHECK(Degrees(ViewPitch(view.eye, view.target)) == Approx(up).margin(0.01));
    CHECK(view.eye.y < kTarget.y);                  // below the pivot
    CHECK(kTarget.y - view.eye.y <= kDepth + 1e-9);  // never below the ground limit
    CHECK(Length(view.eye, view.target) >= kMinDistance - 1e-9);
    CHECK(Length(view.eye, view.target) <= Length(kEye, kTarget) + 1e-9);
  }
  // Looking straight up the camera is close under the character, near the ground limit.
  const auto top = ApplyOrbit(kEye, kTarget, Radians(200), Radians(85), kDepth, kMinDistance);
  CHECK(Degrees(ViewPitch(top.eye, top.target)) == Approx(85).margin(0.01));
  CHECK(kTarget.y - top.eye.y > kDepth - 1.0);
  CHECK(Length(top.eye, top.target) < 200);
}

TEST_CASE("orbit: continuous and smooth over the whole range, slow and fast (no stop, no jump)", "[camera]") {
  // 0.05-degree steps from the lower to the upper limit: the eye moves every step (never stops while the angle
  // changes), by a bounded amount (along the orbit, plus inwards where the ground limit shortens it), and its
  // step changes smoothly (no kink where the ground limit takes over).
  const double first = -85 - Degrees(ViewPitch(kEye, kTarget));  // the offset of the lower limit
  Vec3 previous = ApplyOrbit(kEye, kTarget, Radians(first), Radians(85), kDepth, kMinDistance).eye;
  double previous_step = -1, max_jerk = 0, min_step = 1e9;
  const double radius = Length(kEye, kTarget), step_angle = Radians(0.05);
  for (double deg = first + 0.05; deg <= first + 170 - 0.05; deg += 0.05) {
    const auto view = ApplyOrbit(kEye, kTarget, Radians(deg), Radians(85), kDepth, kMinDistance);
    const double step = Length(view.eye, previous);
    CHECK(step <= radius * step_angle * 2.5);
    min_step = std::min(min_step, step);
    if (previous_step >= 0) max_jerk = std::max(max_jerk, std::abs(step - previous_step));
    previous_step = step;
    previous = view.eye;
  }
  CHECK(min_step > 0.05);  // always moving along the orbit
  CHECK(max_jerk < 0.05);  // the per-step movement changes by less than 5 % of a typical step: no kink
  // Fast: whole-range jumps land on the same orbit as slow movement (the position depends only on the angle).
  const auto fast = ApplyOrbit(kEye, kTarget, Radians(60), Radians(85), kDepth, kMinDistance);
  auto slow = ApplyOrbit(kEye, kTarget, 0, Radians(85), kDepth, kMinDistance);
  for (int i = 1; i <= 600; ++i) slow = ApplyOrbit(kEye, kTarget, Radians(0.1 * i), Radians(85), kDepth, kMinDistance);
  CHECK(fast.eye.x == Approx(slow.eye.x));
  CHECK(fast.eye.y == Approx(slow.eye.y));
}

TEST_CASE("orbit: the range setting limits both directions", "[camera]") {
  for (int range : {45, 60, 75, 85}) {
    const auto up = ApplyOrbit(kEye, kTarget, Radians(170), Radians(range), kDepth, kMinDistance);
    const auto down = ApplyOrbit(kEye, kTarget, Radians(-170), Radians(range), kDepth, kMinDistance);
    CHECK(Degrees(ViewPitch(up.eye, up.target)) == Approx(range).margin(0.01));
    CHECK(Degrees(ViewPitch(down.eye, down.target)) == Approx(-range).margin(0.01));
  }
}

TEST_CASE("orbit: the soft ground limit is continuous with a continuous slope", "[camera]") {
  const double knee = kDepth - kGroundBlend;
  CHECK(SoftDepth(knee - 10, kDepth) == knee - 10);
  CHECK(SoftDepth(knee, kDepth) == Approx(knee));
  const double h = 1e-4;
  CHECK((SoftDepth(knee + h, kDepth) - SoftDepth(knee, kDepth)) / h == Approx(1.0).epsilon(1e-3));
  double previous = SoftDepth(0, kDepth);
  for (double d = 1; d < 5000; d += 1) {
    const double v = SoftDepth(d, kDepth);
    CHECK(v >= previous);
    CHECK(v <= kDepth);
    previous = v;
  }
}

TEST_CASE("orbit: near the ground the upward look eases to a stop before the character, then backs off", "[camera]") {
  CHECK(LimitUpward(0.01, false, 30, 1.0 / 30) == 0.01);         // looking down: never limited
  CHECK(LimitUpward(0.01, true, 500, 1.0 / 30) == 0.01);         // far: free
  CHECK(LimitUpward(-0.01, true, 100, 1.0 / 30) == -0.01);       // looking back down: always allowed
  CHECK(LimitUpward(0.01, true, kNearDistance, 1.0 / 30) == 0);  // at the near distance: stopped
  double previous = 0;
  for (double d = kNearDistance; d <= kComfortDistance; d += 1) {  // smooth, monotonic in between
    const double v = LimitUpward(0.01, true, d, 1.0 / 30);
    CHECK(v >= previous);
    CHECK(v - previous < 0.0006);
    previous = v;
  }
  CHECK(previous == Approx(0.01));
  CHECK(LimitUpward(0, true, 40, 1.0 / 30) < 0);  // too close: eases back down
  CHECK(LimitUpward(0, true, 40, 1.0 / 30) > -kBackOff / 30);
}

TEST_CASE("orbit: degenerate input leaves the camera unchanged", "[camera]") {
  const auto same = ApplyOrbit(kTarget, kTarget, 1.0, Radians(85), kDepth, kMinDistance);
  CHECK(same.eye.x == kTarget.x);
  CHECK(same.offset == 0);
  const Vec3 above{kTarget.x, kTarget.y + 500, kTarget.z};
  CHECK(ApplyOrbit(above, kTarget, 0.5, Radians(85), kDepth, kMinDistance).offset == 0);
}

TEST_CASE("unpitch: a pitched camera basis is turned back to the game's orientation", "[camera]") {
  // A level camera looking along +z (right = up x forward = +x), then pitched up by 50 degrees.
  const Vec3 right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, 1};
  const double delta = Radians(50);
  const Basis pitched{right, RotateAbout(up, right, -delta), RotateAbout(forward, right, -delta), {1, 2, 3}};
  CHECK(pitched.forward.y == Approx(std::sin(delta)));  // negative right-hand turn about right = up
  const Basis back = UnpitchBasis(pitched, delta);
  CHECK(back.forward.x == Approx(0).margin(1e-9));
  CHECK(back.forward.y == Approx(0).margin(1e-9));
  CHECK(back.forward.z == Approx(1));
  CHECK(back.up.y == Approx(1));
  CHECK(back.right.x == 1);
  CHECK(back.position.y == 2);
}

TEST_CASE("recentring waits, then eases back to the default view", "[camera]") {
  CHECK(Recenter(0.8, 0.5, 1.0 / 30) == 0.8);  // within the delay: unchanged
  double offset = 0.8;
  for (int frame = 0; frame < 30; ++frame) {
    offset = Recenter(offset, 2.0, 1.0 / 30);
  }
  CHECK(offset < 0.8 * 0.1);
  CHECK(offset > 0);
}
