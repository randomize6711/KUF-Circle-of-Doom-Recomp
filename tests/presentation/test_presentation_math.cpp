// Presentation interpolation math (src/runtime/presentation_math.cpp, M6D D3b-3): rigid transforms, the camera
// around its pivot, angle wrap, the interpolation factor.

#include <cmath>
#include <string>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "runtime/presentation_math.h"

using Catch::Approx;
using namespace kuf::presentation;

namespace {

double Degrees(double radians) { return radians * 180.0 / M_PI; }
double Distance(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }
double MaxDiff(const Mat4& a, const Mat4& b) {
  double e = 0;
  for (int i = 0; i < 16; ++i) e = std::max(e, double(std::abs(a[i] - b[i])));
  return e;
}

// A yaw rotation (about y, the game's heading convention: forward = (sin, 0, cos)) with a uniform scale.
Mat4 Yawed(double yaw, double scale, const Vec3& position) {
  const double s = std::sin(yaw), c = std::cos(yaw);
  return {float(c * scale), 0, float(-s * scale), 0, 0, float(scale), 0, 0, float(s * scale), 0, float(c * scale), 0,
          float(position.x), float(position.y), float(position.z), 1};
}

// A camera at `distance` from `pivot`, the eye's offset at azimuth `az` and elevation `el`, aimed at the pivot.
CameraTick Orbit(const Vec3& pivot, double az, double el, double distance) {
  const Vec3 offset{std::sin(az) * std::cos(el) * distance, std::sin(el) * distance,
                    std::cos(az) * std::cos(el) * distance};
  const Vec3 eye{pivot.x + offset.x, pivot.y + offset.y, pivot.z + offset.z};
  CameraTick t;
  t.matrix = LookAt({-offset.x, -offset.y, -offset.z}, eye);
  t.have_pivot = true;
  t.pivot = pivot;
  return t;
}

double AimOff(const Mat4& m, const Vec3& pivot) {  // degrees between the view's forward and eye -> pivot
  const Vec3 e = Position(m), f = Forward(m), d{pivot.x - e.x, pivot.y - e.y, pivot.z - e.z};
  const double c = (f.x * d.x + f.y * d.y + f.z * d.z) / (std::hypot(f.x, f.y, f.z) * std::hypot(d.x, d.y, d.z));
  return Degrees(std::acos(std::clamp(c, -1.0, 1.0)));
}

}  // namespace

TEST_CASE("rigid: decompose and compose give the matrix back (rotation, scale, position)", "[presentation]") {
  const Mat4 m = Yawed(2.4, 3.0, {-37234.3, 3604.2, 11313.0});
  const Rigid r = Decompose(m);
  REQUIRE(r.valid);
  CHECK(r.scale.x == Approx(3.0));
  CHECK(MaxDiff(Compose(r), m) < 1e-3);
  Mat4 reflected = m;
  for (int i = 0; i < 3; ++i) reflected[i] = -reflected[i];
  CHECK_FALSE(Decompose(reflected).valid);
  CHECK_FALSE(Decompose(Mat4{}).valid);
}

TEST_CASE("rigid: end points exact, the rotation the shorter way across the wrap", "[presentation]") {
  const Mat4 a = Yawed(170 * M_PI / 180, 1.0, {0, 0, 0});
  const Mat4 b = Yawed(-170 * M_PI / 180, 1.0, {20, 0, 0});
  CHECK(InterpolateRigid(a, b, 0) == a);
  CHECK(InterpolateRigid(a, b, 1) == b);
  const Mat4 mid = InterpolateRigid(a, b, 0.5);
  const Vec3 f = Forward(mid);
  CHECK(std::abs(Degrees(Azimuth(f))) == Approx(180).margin(0.01));  // through 180, not through 0
  CHECK(Position(mid).x == Approx(10));
  const Rigid ra = Decompose(a), rb = Decompose(b);
  CHECK(Degrees(AngleBetween(ra.rotation, rb.rotation)) == Approx(20).margin(0.01));
  CHECK(Degrees(AngleBetween(ra.rotation, Decompose(mid).rotation)) == Approx(10).margin(0.01));
}

TEST_CASE("camera: the game's look-at (rows of a traced gameplay camera)", "[presentation]") {
  // Shadow Depths, tick 2000 of the timing route (docs/HIGH_FPS.md 15.1).
  const Mat4 traced{0.127f, 0.000f, 0.992f, 0, -0.143f, 0.990f, 0.018f, 0, -0.982f, -0.144f, 0.126f, 0,
                    -195.266f, 248.904f, 8.334f, 1};
  CHECK(LookAtError(traced) < 2e-3);  // printed with 3 decimals
  const Mat4 rebuilt = LookAt(Forward(traced), Position(traced));
  CHECK(LookAtError(rebuilt) < 1e-6);
  Mat4 rolled = Yawed(0.3, 1.0, {0, 0, 0});
  const Mat4 roll{float(std::cos(0.2)), float(std::sin(0.2)), 0, 0, float(-std::sin(0.2)), float(std::cos(0.2)), 0, 0,
                  0, 0, 1, 0, 0, 0, 0, 1};
  Mat4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r[i * 4 + j] += roll[i * 4 + k] * rolled[k * 4 + j];
  CHECK(LookAtError(r) > 0.1);
}

TEST_CASE("camera: around the pivot, end points exact, aimed at the pivot, no chord", "[presentation]") {
  const Vec3 pivot{100, 50, -30};
  const CameraTick a = Orbit(pivot, 0.0, -0.15, 707.0);
  const CameraTick b = Orbit({110, 50, -30}, M_PI / 2, 0.6, 707.0);  // a 90-degree turn and a look down
  CHECK(InterpolateCamera(a, b, 0, NAN, false) == a.matrix);
  CHECK(InterpolateCamera(a, b, 1, NAN, false) == b.matrix);
  for (double t = 0.1; t < 1; t += 0.1) {
    const Mat4 m = InterpolateCamera(a, b, t, NAN, false);
    const Vec3 p{100 + 10 * t, 50, -30};
    CHECK(AimOff(m, p) < 0.01);
    CHECK(Distance(Position(m), p) == Approx(707.0).margin(0.01));  // a straight line would come to 500
    CHECK(LookAtError(m) < 1e-5);
  }
  const Mat4 mid = InterpolateCamera(a, b, 0.5, NAN, false);
  const Vec3 offset{Position(mid).x - 105, Position(mid).y - 50, Position(mid).z + 30};
  CHECK(Degrees(Azimuth(offset)) == Approx(45).margin(0.01));
  CHECK(Degrees(Elevation(offset)) == Approx(Degrees(0.225)).margin(0.01));
}

TEST_CASE("camera: azimuth across the wrap the shorter way, a known turn the way it was made", "[presentation]") {
  const Vec3 pivot{0, 0, 0};
  const CameraTick a = Orbit(pivot, M_PI * 179 / 180, 0, 500);
  const CameraTick b = Orbit(pivot, -M_PI * 179 / 180, 0, 500);
  const auto mid_azimuth = [&](double turn) {
    const Vec3 e = Position(InterpolateCamera(a, b, 0.5, turn, false));
    return Degrees(Azimuth(e));
  };
  CHECK(std::abs(mid_azimuth(NAN)) == Approx(180).margin(0.01));        // 2 degrees through 180
  CHECK(std::abs(mid_azimuth(2 * M_PI / 180)) == Approx(180).margin(0.01));
  CHECK(mid_azimuth(-358 * M_PI / 180) == Approx(0).margin(0.01));     // the long way: a fast flick
  // The view's azimuth: the eye's offset turned by the same amount.
  const Mat4 m = InterpolateCamera(a, b, 0.25, -358 * M_PI / 180, false);
  CHECK(Degrees(Azimuth(Position(m))) == Approx(89.5).margin(0.01));
}

TEST_CASE("camera: a collision pull-in is presented at once", "[presentation]") {
  const Vec3 pivot{0, 0, 0};
  const CameraTick a = Orbit(pivot, 0.2, -0.1, 1415);
  const CameraTick b = Orbit(pivot, 0.25, -0.1, 260);
  for (double t = 0.1; t < 1; t += 0.2) {
    CHECK(Distance(Position(InterpolateCamera(a, b, t, NAN, true)), pivot) == Approx(260).margin(0.01));
    CHECK(Distance(Position(InterpolateCamera(a, b, t, NAN, false)), pivot) == Approx(1415 + (260 - 1415) * t).margin(0.01));
  }
  CHECK(InterpolateCamera(a, b, 0, NAN, true) == a.matrix);  // the tick before keeps its own camera
}

TEST_CASE("camera: without a pivot, and a rolled camera as a rigid transform", "[presentation]") {
  CameraTick a, b;
  a.matrix = LookAt({0, -0.1, 1}, {0, 100, 0});
  b.matrix = LookAt({1, -0.1, 0}, {50, 100, 0});
  const Mat4 m = InterpolateCamera(a, b, 0.5, NAN, false);
  CHECK(Position(m).x == Approx(25));
  CHECK(Degrees(Azimuth(Forward(m))) == Approx(45).margin(0.01));
  CHECK(LookAtError(m) < 1e-5);
  CameraTick c = a;
  c.matrix[1] = 0.3f;  // rolled: not a look-at
  const Mat4 r = InterpolateCamera(c, b, 0.5, NAN, false);
  CHECK(MaxDiff(r, InterpolateRigid(c.matrix, b.matrix, 0.5)) == 0);
}

TEST_CASE("alpha: the fraction of a tick, never beyond the newest tick", "[presentation]") {
  const double tick = 1.0 / 30;
  CHECK(Alpha(0, tick) == 0);
  CHECK(Alpha(tick / 4, tick) == Approx(0.25));
  CHECK(Alpha(tick * 1.5, tick) == 1);  // a late tick: hold, no extrapolation
  CHECK(Alpha(-0.001, tick) == 0);
  CHECK(Alpha(NAN, tick) == 1);
}

namespace {

// Two consecutive gameplay ticks: the modern camera, the player model 14 units further on.
std::pair<Snapshot, Snapshot> Ticks() {
  Snapshot a;
  a.valid = true;
  a.tick = 1000;
  a.world = true;
  a.renderer = 0x44000000;
  a.view = 0x44BF3A50;
  a.have_camera = a.fresh = true;
  a.camera = LookAt({0, -0.15, 1}, {0, 250, -700});
  a.fov = 0.6f;
  a.fov_setting = 0;
  a.view_info.serial = 10;
  a.view_info.camera = 0x44BF2ED0;
  a.view_info.modern = true;
  a.view_info.turn = 0;
  a.player = 0x46E1D900;
  a.player_index = 35;
  a.player_matrix = Yawed(0.3, 1.0, {0, -130, 0});
  Snapshot b = a;
  b.tick = 1001;
  b.view_info.serial = 11;
  b.camera = LookAt({0.05, -0.15, 1}, {14, 250, -700});
  b.player_matrix = Yawed(0.3, 1.0, {14, -130, 0});
  return {a, b};
}

}  // namespace

TEST_CASE("discontinuities: consecutive gameplay ticks are interpolated", "[presentation]") {
  auto [a, b] = Ticks();
  const Interval i = Decide(a, b);
  CHECK(i.camera_cut == nullptr);
  CHECK(i.player_cut == nullptr);
  CHECK(i.fov);
  CHECK(i.turn == 0);
  CHECK_FALSE(i.pulled_in);
}

TEST_CASE("discontinuities: each explicit signal cuts, and only what it concerns", "[presentation]") {
  const auto cut = [](auto change) {
    auto [a, b] = Ticks();
    change(a, b);
    const Interval i = Decide(a, b);
    return std::pair<std::string, std::string>(i.camera_cut ? i.camera_cut : "", i.player_cut ? i.player_cut : "");
  };
  CHECK(cut([](Snapshot& a, Snapshot&) { a.valid = false; }) == std::pair<std::string, std::string>("first", "first"));
  CHECK(cut([](Snapshot&, Snapshot& b) { b.tick += 1; }).first == "gap");  // a tick without a frame (loading)
  CHECK(cut([](Snapshot&, Snapshot& b) { b.world = false; }) == std::pair<std::string, std::string>("not world", "not world"));
  CHECK(cut([](Snapshot&, Snapshot& b) { b.view = 0x45000000; }).first == "view");       // another level
  CHECK(cut([](Snapshot&, Snapshot& b) { b.fresh = false; }).first == "no view");      // no camera this tick
  CHECK(cut([](Snapshot&, Snapshot& b) { b.view_info.event = true; }) ==
        std::pair<std::string, std::string>("event", "event"));                          // a cut scene starts
  CHECK(cut([](Snapshot& a, Snapshot&) { a.view_info.event = true; }).first == "event");  // ... or ends
  CHECK(cut([](Snapshot&, Snapshot& b) { b.view_info.camera = 1; }).first == "camera");
  // The game's camera takes over from the modern camera, or back (the new game's opening: 1,615 units in a tick).
  CHECK(cut([](Snapshot&, Snapshot& b) { b.view_info.modern = false; }) ==
        std::pair<std::string, std::string>("handover", ""));
  CHECK(cut([](Snapshot& a, Snapshot&) { a.view_info.modern = false; }).first == "handover");
  CHECK(cut([](Snapshot&, Snapshot& b) { b.view_info.snapped = true; }) ==
        std::pair<std::string, std::string>("snapped", ""));                             // centring: the player moves on
  CHECK(cut([](Snapshot&, Snapshot& b) { b.player = 0x47000000; }).second == "object");  // respawned model
  CHECK(cut([](Snapshot&, Snapshot& b) { b.player = 0; }).second == "none");
  // The modern camera presents a pull-in at once (InterpolateCamera); the game's own camera is cut.
  auto [a, b] = Ticks();
  b.view_info.pulled_in = true;
  CHECK(Decide(a, b).camera_cut == nullptr);
  CHECK(Decide(a, b).pulled_in);
  // ... unless the view's direction jumped too (the fight: pulled in from 880 to 211 units, tilted 55 degrees down).
  const Mat4 pulled = b.camera;
  b.camera = LookAt({0.05, -1.5, 0.4}, {14, 600, -150});
  CHECK(std::string(Decide(a, b).camera_cut) == "pulled in");
  b.camera = pulled;
  a.view_info.modern = b.view_info.modern = false;
  CHECK(std::string(Decide(a, b).camera_cut) == "pulled in");
  // A field of view setting changed: the camera moves on, the field of view is taken at once.
  auto [c, d] = Ticks();
  d.fov_setting = 90;
  CHECK(Decide(c, d).camera_cut == nullptr);
  CHECK_FALSE(Decide(c, d).fov);
}

TEST_CASE("discontinuities: backstops for what the game does not signal", "[presentation]") {
  auto [a, b] = Ticks();
  b.player_matrix = Yawed(0.3, 1.0, {kPlayerJump + 1, -130, 0});
  CHECK(std::string(Decide(a, b).player_cut) == "teleport");
  b.player_matrix = Yawed(0.3 + kPlayerTurn + 0.1, 1.0, {14, -130, 0});
  CHECK(std::string(Decide(a, b).player_cut) == "turn");
  b.player_matrix = Yawed(0.3 + kPlayerTurn - 0.1, 1.0, {14, -130, 0});
  CHECK(Decide(a, b).player_cut == nullptr);
  b.camera = LookAt({0, -0.15, 1}, {kCameraJump + 1, 250, -700});
  CHECK(std::string(Decide(a, b).camera_cut) == "jump");
  // Event cameras (no explicit cut signal): a new shot.
  a.view_info.event = b.view_info.event = true;
  a.view_info.modern = b.view_info.modern = false;
  b.camera = LookAt({0, -0.15, 1}, {kEventMove + 1, 250, -700});
  CHECK(std::string(Decide(a, b).camera_cut) == "shot");
  b.camera = LookAt({1, -0.15, 0.3}, {0, 250, -700});
  CHECK(std::string(Decide(a, b).camera_cut) == "shot");
  b.camera = LookAt({0.1, -0.15, 1}, {20, 250, -700});
  CHECK(Decide(a, b).camera_cut == nullptr);  // a moving event camera
  // The game's own camera: a new shot breaks its motion (the new game's opening: 0.7 units, then 485, then 205 back);
  // a fast smooth flight (the title: 159, 145, 131 units per tick) and the modern camera's own flicks are not cut.
  auto [c, d] = Ticks();
  c.view_info.modern = d.view_info.modern = false;
  c.have_step = d.have_step = true;
  c.eye_step = {0.7, 0, 0};
  d.eye_step = {306, -61, -371};
  CHECK(std::string(Decide(c, d).camera_cut) == "shot");
  c.eye_step = d.eye_step;
  d.eye_step = {-169, -2, 116};
  CHECK(std::string(Decide(c, d).camera_cut) == "shot");
  c.eye_step = {159, 0, 0};
  d.eye_step = {145, 0, 0};
  CHECK(Decide(c, d).camera_cut == nullptr);
  c.have_step = false;  // no step before (the first tick of a camera): nothing to compare
  d.eye_step = {485, 0, 0};
  CHECK(Decide(c, d).camera_cut == nullptr);
  c.have_step = true;
  c.eye_step = {0, 0, 0};
  c.view_info.modern = d.view_info.modern = true;
  CHECK(Decide(c, d).camera_cut == nullptr);  // the modern camera: the player's one-tick flick around the pivot
}

TEST_CASE("objects (D4a): the same object moving is interpolated", "[presentation]") {
  ObjectTick a{123, 0x40001000, 0x821010E0, 7, Yawed(0.3, 1.0, {8236.9, 577.9, 108.8})};
  ObjectTick b = a;
  b.matrix = Yawed(0.5, 1.0, {8335.4, 577.9, 108.8});  // a knock-back (the fight's largest step, 101 units)
  CHECK(ObjectCut(a, b) == nullptr);
  const Mat4 half = InterpolateRigid(Decompose(a.matrix), Decompose(b.matrix), 0.5);
  CHECK(Distance(Position(half), {8286.15, 577.9, 108.8}) < 0.01);
  CHECK(Azimuth(Forward(half)) == Approx(0.4).margin(1e-5));
  CHECK(MaxDiff(InterpolateRigid(Decompose(a.matrix), Decompose(b.matrix), 0.0), a.matrix) < 1e-2);
  CHECK(MaxDiff(InterpolateRigid(Decompose(a.matrix), Decompose(b.matrix), 1.0), b.matrix) < 1e-2);
  // Large rotations: the shorter way across the wrap, up to the backstop.
  a.matrix = Yawed(M_PI - 0.2, 1.0, {0, 0, 0});
  b.matrix = Yawed(-M_PI + 0.2, 1.0, {0, 0, 0});
  CHECK(ObjectCut(a, b) == nullptr);
  CHECK(std::abs(std::remainder(Azimuth(Forward(InterpolateRigid(Decompose(a.matrix), Decompose(b.matrix), 0.5))) - M_PI,
                                2 * M_PI)) < 1e-4);
  b.matrix = Yawed(M_PI - 0.2 + kObjectTurn - 0.05, 1.0, {0, 0, 0});
  CHECK(ObjectCut(a, b) == nullptr);
}

TEST_CASE("objects (D4a): another object at the same index is never interpolated", "[presentation]") {
  const ObjectTick a{40, 0x40001000, 0x821010E0, 3, Yawed(0.0, 1.0, {10, 0, 10})};
  ObjectTick b = a;
  b.matrix = Yawed(0.0, 1.0, {11, 0, 10});
  b.generation = 4;  // released and created again at the same address and class (a pooled slot)
  CHECK(std::string(ObjectCut(a, b)) == "object");
  b = a;
  b.object = 0x40002000;  // another address
  CHECK(std::string(ObjectCut(a, b)) == "object");
  b = a;
  b.vtable = 0x82101540;  // another class
  CHECK(std::string(ObjectCut(a, b)) == "object");
  b = a;
  b.index = 41;
  CHECK(std::string(ObjectCut(a, b)) == "object");
}

TEST_CASE("objects (D4a): teleports, instant turns and matrices that are not rigid are cut", "[presentation]") {
  const ObjectTick a{50, 0x40001000, 0x82101540, 0, Yawed(0.0, 2.0, {0, 0, 0})};
  ObjectTick b = a;
  b.matrix = Yawed(0.0, 2.0, {kObjectJump + 1, 0, 0});
  CHECK(std::string(ObjectCut(a, b)) == "teleport");
  b.matrix = Yawed(kObjectTurn + 0.1, 2.0, {1, 0, 0});
  CHECK(std::string(ObjectCut(a, b)) == "turn");
  b.matrix = Yawed(0.1, 2.0, {1, 0, 0});
  CHECK(ObjectCut(a, b) == nullptr);
  CHECK(IsRigid(b.matrix));
  b.matrix[4] += 0.5f;  // sheared: interpolating it would change its shape
  CHECK_FALSE(IsRigid(b.matrix));
  CHECK(std::string(ObjectCut(a, b)) == "matrix");
  b.matrix = Yawed(0.1, 2.0, {1, 0, 0});
  for (int i = 0; i < 3; ++i) b.matrix[i] = -b.matrix[i];  // reflected
  CHECK(std::string(ObjectCut(a, b)) == "matrix");
  b.matrix = Yawed(0.1, 2.0, {1, 0, 0});
  b.matrix[3] = 0.25f;  // a projective column
  CHECK(std::string(ObjectCut(a, b)) == "matrix");
  b.matrix = Yawed(0.1, 2.0, {1, 0, 0});
  b.matrix[13] = NAN;
  CHECK(std::string(ObjectCut(a, b)) == "matrix");  // not finite: never interpolated
}

namespace {

// A chain of `n` bones `length` apart along each parent's forward, each bone turned by `yaw` in its parent's space
// (the root by `root_yaw`), in model space as the game's palettes hold them.
PoseTick Chain(int n, double length, double root_yaw, double yaw) {
  PoseTick p;
  p.object = 0x40002000;
  p.skeleton = 0x40003000;
  p.base = {{7, 10, 1}};
  Mat4 parent{};
  for (int i = 0; i < n; ++i) {
    p.parents.push_back(i - 1);
    const Mat4 local = i == 0 ? Yawed(root_yaw, 1.0, {0, 0, 0}) : Yawed(yaw, 1.0, {0, 0, float(length)});
    Mat4 model{};
    if (i == 0) {
      model = local;
    } else {
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          double v = 0;
          for (int k = 0; k < 4; ++k) v += double(local[r * 4 + k]) * parent[k * 4 + c];
          model[r * 4 + c] = float(v);
        }
      }
    }
    p.bones.push_back(model);
    parent = model;
  }
  return p;
}

}  // namespace

TEST_CASE("poses (D4b): bones are interpolated in their parent's space, the ends are the ticks' palettes", "[presentation]") {
  const PoseTick a = Chain(4, 10.0, 0.0, 0.0);
  PoseTick b = Chain(4, 10.0, 0.4, 0.5);  // each joint bends: the tip sweeps an arc
  b.base[0].time = 11;
  const PosePair p = PreparePose(a, b);
  REQUIRE(p.valid);
  CHECK(p.local);
  CHECK(PoseCut(a, b, p) == nullptr);
  CHECK(p.turn_max == Approx(0.5).margin(1e-4));
  std::vector<Mat4> out;
  InterpolatePose(a, b, p, 0.0, &out);
  for (size_t i = 0; i < out.size(); ++i) CHECK(MaxDiff(out[i], a.bones[i]) == 0);
  InterpolatePose(a, b, p, 1.0, &out);
  for (size_t i = 0; i < out.size(); ++i) CHECK(MaxDiff(out[i], b.bones[i]) == 0);
  // The middle: every joint half bent, so the pose is the chain at half the angles; bone lengths kept.
  InterpolatePose(a, b, p, 0.5, &out);
  const PoseTick half = Chain(4, 10.0, 0.2, 0.25);
  for (size_t i = 0; i < out.size(); ++i) CHECK(MaxDiff(out[i], half.bones[i]) < 1e-4);
  for (size_t i = 1; i < out.size(); ++i) {
    CHECK(Distance(Position(out[i]), Position(out[i - 1])) == Approx(10.0).margin(1e-4));
  }
  for (const double t : {0.1, 0.25, 0.5, 0.9}) {  // the palettes are at the alpha asked for
    InterpolatePose(a, b, p, t, &out);
    CHECK(PoseProgress(p, out) == Approx(t).margin(1e-3));
  }
  InterpolatePose(a, b, p, 0.5, &out);
  // Each bone on its own in model space would cut the arc: the tip nearer the root than the chain allows.
  const Mat4 tip = InterpolateRigid(a.bones[3], b.bones[3], 0.5);
  CHECK(Distance(Position(tip), Position(half.bones[3])) > 0.5);
}

TEST_CASE("poses (D4b): another skeleton, a switch without a crossfade and bones that do not decompose are cut",
          "[presentation]") {
  const PoseTick a = Chain(3, 10.0, 0.0, 0.0);
  PoseTick b = Chain(3, 10.0, 0.1, 0.1);
  CHECK(PoseCut(a, b, PreparePose(a, b)) == nullptr);
  b.skeleton = 0x40004000;
  CHECK(std::string(PoseCut(a, b, PreparePose(a, b))) == "skeleton");
  b = Chain(4, 10.0, 0.1, 0.1);
  CHECK(std::string(PoseCut(a, b, PreparePose(a, b))) == "skeleton");
  // Another clip: a crossfade (the old clip kept as a blend layer) is interpolated, a switch without one is not.
  b = Chain(3, 10.0, 0.1, 0.1);
  b.base = {{9, 0, 1}};
  CHECK(std::string(PoseCut(a, b, PreparePose(a, b))) == "snap");
  b.blend = {{7, 11, 6.0f / 7}};
  CHECK(PoseCut(a, b, PreparePose(a, b)) == nullptr);
  {  // the fade ending while the base clip changes (measured in the fight): still the game's crossfade
    PoseTick fading = a;
    fading.base = {{0, 4, 1}};
    fading.blend = {{8, 18, 2.0f / 7}};
    PoseTick ended = b;
    ended.base = {{7, 1, 1}};
    ended.blend.clear();
    CHECK(PoseCut(fading, ended, PreparePose(fading, ended)) == nullptr);
  }
  // The same clip from its start again (a loop) is interpolated; a bone turned at once beyond the backstop is shown at
  // the new tick's angle while the rest of the pose is interpolated.
  b = Chain(3, 10.0, 0.1, 0.1);
  b.base[0].time = 0;
  CHECK(PoseCut(a, b, PreparePose(a, b)) == nullptr);
  {
    // bone 2 turned by more than the backstop in its parent's space (bone 1 not turned)
    PoseTick c = Chain(3, 10.0, 0.4, 0.0);
    const Mat4 turned = Yawed(kBoneTurn + 0.2, 1.0, {0, 0, 10});
    for (int r = 0; r < 4; ++r) {
      for (int k = 0; k < 4; ++k) {
        double v = 0;
        for (int j = 0; j < 4; ++j) v += double(turned[r * 4 + j]) * c.bones[1][j * 4 + k];
        c.bones[2][r * 4 + k] = float(v);
      }
    }
    const PosePair p = PreparePose(a, c);
    REQUIRE(p.valid);
    CHECK(p.held == 1);
    CHECK(p.hold[2]);
    CHECK(PoseCut(a, c, p) == nullptr);
    std::vector<Mat4> out;
    InterpolatePose(a, c, p, 0.5, &out);
    const PoseTick half = Chain(3, 10.0, 0.2, 0.0);
    CHECK(MaxDiff(out[1], half.bones[1]) < 1e-4);  // the rest interpolated
    // the held bone: the new tick's local turn on the interpolated parent
    const Mat4 local = Yawed(kBoneTurn + 0.2, 1.0, {0, 0, 10});
    Mat4 expect{};
    for (int r = 0; r < 4; ++r) {
      for (int k = 0; k < 4; ++k) {
        double v = 0;
        for (int j = 0; j < 4; ++j) v += double(local[r * 4 + j]) * out[1][j * 4 + k];
        expect[r * 4 + k] = float(v);
      }
    }
    CHECK(MaxDiff(out[2], expect) < 1e-4);
  }
  // A sheared bone: not interpolated.
  b = Chain(3, 10.0, 0.1, 0.1);
  b.bones[1][4] += 0.5f;
  CHECK(std::string(PoseCut(a, b, PreparePose(a, b))) == "matrix");
  // Without a usable hierarchy each bone is interpolated in model space.
  b = Chain(3, 10.0, 0.1, 0.1);
  PoseTick c = a;
  c.parents.clear();
  b.parents.clear();
  const PosePair p = PreparePose(c, b);
  CHECK(p.valid);
  CHECK_FALSE(p.local);
}

namespace {
Mat4 Mul(const Mat4& a, const Mat4& b) {  // row vectors: a then b
  Mat4 r{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += double(a[i * 4 + k]) * b[k * 4 + j];
      r[i * 4 + j] = float(v);
    }
  }
  return r;
}
// A part hanging from the chain's last bone: bone 0 fixed to it, bone 1 swinging below bone 0 by `swing` (radians).
std::vector<Mat4> Part(const PoseTick& model, double swing) {
  const Mat4 fixed = Yawed(0.0, 1.0, {2, -3, 1});
  const Mat4 hang = Yawed(swing, 1.0, {0, -5, 0});
  const Mat4 b0 = Mul(fixed, model.bones.back());
  return {b0, Mul(hang, b0)};
}
}  // namespace

TEST_CASE("parts (D4b-3): placed on the bone they hang from as presented, swinging between the ticks", "[presentation]") {
  const PoseTick a = Chain(4, 10.0, 0.0, 0.0);
  const PoseTick b = Chain(4, 10.0, 0.4, 0.5);
  const std::vector<Mat4> pa = Part(a, 0.0), pb = Part(b, 0.6);
  const PartPair p = PreparePart(pa, pb, a.bones, b.bones);
  REQUIRE(p.valid);
  CHECK(p.anchor[0] == 3);  // fixed to the chain's tip
  CHECK(p.anchor[1] == 3);
  CHECK(p.held == 0);
  CHECK(p.turn_max == Approx(0.6).margin(1e-4));
  std::vector<Mat4> out;
  InterpolatePart(p, a.bones, 0.0, &out);  // the ends: the ticks' palettes
  for (size_t j = 0; j < out.size(); ++j) CHECK(MaxDiff(out[j], pa[j]) < 1e-4);
  InterpolatePart(p, b.bones, 1.0, &out);
  for (size_t j = 0; j < out.size(); ++j) CHECK(MaxDiff(out[j], pb[j]) < 1e-4);
  // Between: on the model's interpolated tip, the fixed bone exactly where it hangs, the swinging one half swung.
  const PosePair mp = PreparePose(a, b);
  std::vector<Mat4> model;
  InterpolatePose(a, b, mp, 0.5, &model);
  InterpolatePart(p, model, 0.5, &out);
  const std::vector<Mat4> expect = Part(Chain(4, 10.0, 0.2, 0.25), 0.3);
  for (size_t j = 0; j < out.size(); ++j) CHECK(MaxDiff(out[j], expect[j]) < 1e-3);
  for (const double t : {0.1, 0.25, 0.5, 0.9}) {  // the palettes are at the alpha asked for
    InterpolatePose(a, b, mp, t, &model);
    InterpolatePart(p, model, t, &out);
    CHECK(PartProgress(p, model, out) == Approx(t).margin(1e-3));
  }
  // Each bone on its own in model space would leave the part behind the bending chain.
  CHECK(Distance(Position(InterpolateRigid(pa[0], pb[0], 0.5)), Position(expect[0])) > 0.5);
}

TEST_CASE("parts (D4b-3): mismatched palettes are not interpolated; a bone turned at once is held", "[presentation]") {
  const PoseTick a = Chain(4, 10.0, 0.0, 0.0);
  const PoseTick b = Chain(4, 10.0, 0.1, 0.1);
  std::vector<Mat4> pa = Part(a, 0.0), pb = Part(b, 0.1);
  CHECK_FALSE(PreparePart(pa, {pb[0]}, a.bones, b.bones).valid);           // another bone count
  CHECK_FALSE(PreparePart(pa, pb, a.bones, Chain(3, 10.0, 0.1, 0.1).bones).valid);  // another model
  std::vector<Mat4> broken = pb;
  broken[1] = Mat4{};  // does not decompose
  CHECK_FALSE(PreparePart(pa, broken, a.bones, b.bones).valid);
  const PartPair p = PreparePart(pa, Part(b, 3.0), a.bones, b.bones);  // swung by 3 rad in one tick
  REQUIRE(p.valid);
  CHECK(p.held == 1);
  CHECK(p.hold[1]);
  std::vector<Mat4> out;
  InterpolatePart(p, b.bones, 0.5, &out);
  CHECK(MaxDiff(out[1], Part(b, 3.0)[1]) < 1e-3);  // the held bone at tick b's, on the anchor given
}

TEST_CASE("parts (D4b-3): a bone whose offset turns to the other side of its anchor is held", "[presentation]") {
  // The swinging bone 5 units below bone 0 at tick a, 5 units above it at tick b, its rotation unchanged: the line
  // between the offsets passes through the anchor (D5 validation: 9.8 units short in one fight).
  const PoseTick a = Chain(4, 10.0, 0.0, 0.0);
  const PoseTick b = Chain(4, 10.0, 0.4, 0.5);
  // Bone 0 also moves 30 units on its own, so bone 0 is the bone the swinging one moved with most rigidly.
  const Mat4 b0a = Mul(Yawed(0.0, 1.0, {2, -3, 1}), a.bones.back());
  const Mat4 b0b = Mul(Yawed(0.0, 1.0, {32, -3, 1}), b.bones.back());
  const std::vector<Mat4> pa = {b0a, Mul(Yawed(0.0, 1.0, {0, -5, 0}), b0a)};
  const std::vector<Mat4> pb = {b0b, Mul(Yawed(0.0, 1.0, {0, 5, 0}), b0b)};
  const PartPair p = PreparePart(pa, pb, a.bones, b.bones);
  REQUIRE(p.valid);
  REQUIRE(p.anchor[1] == 4 + 0);
  CHECK(p.hold[1]);
  CHECK_FALSE(p.hold[0]);
  const PosePair mp = PreparePose(a, b);
  std::vector<Mat4> model, out;
  for (const double t : {0.25, 0.5, 0.75}) {
    InterpolatePose(a, b, mp, t, &model);
    InterpolatePart(p, model, t, &out);
    const Mat4* anchor = PartAnchor(p, 1, model, out);
    REQUIRE(anchor);
    CHECK(Distance(Position(out[1]), Position(*anchor)) == Approx(5.0).margin(1e-3));  // its length kept
  }
}

TEST_CASE("parts (D4b-3): a chain swings link by link, each link keeping its length", "[presentation]") {
  // Three links of 8 units below the model's tip, each bent by `bend` more than the one before (a rope swinging).
  const auto rope = [](const PoseTick& model, double bend) {
    std::vector<Mat4> links;
    Mat4 at = Mul(Yawed(0.0, 1.0, {0, -2, 0}), model.bones.back());
    for (int i = 0; i < 3; ++i) {
      at = Mul(Yawed(bend, 1.0, {0, 0, 8}), at);
      links.push_back(at);
    }
    return links;
  };
  const PoseTick a = Chain(4, 10.0, 0.0, 0.0), b = Chain(4, 10.0, 0.2, 0.2);
  const PartPair p = PreparePart(rope(a, 0.0), rope(b, 0.8), a.bones, b.bones);
  REQUIRE(p.valid);
  CHECK(p.anchor[0] < 4);  // the first link on the model
  CHECK(p.anchor[1] == 4 + 0);  // each further link on the one before
  CHECK(p.anchor[2] == 4 + 1);
  std::vector<Mat4> model, out;
  InterpolatePose(a, b, PreparePose(a, b), 0.5, &model);
  InterpolatePart(p, model, 0.5, &out);
  const std::vector<Mat4> expect = rope(Chain(4, 10.0, 0.1, 0.1), 0.4);
  for (size_t j = 0; j < out.size(); ++j) CHECK(MaxDiff(out[j], expect[j]) < 1e-3);
  for (size_t j = 1; j < out.size(); ++j) CHECK(Distance(Position(out[j]), Position(out[j - 1])) == Approx(8.0).margin(1e-3));
}

TEST_CASE("parts (D4b-3): a bone swinging about its anchor moves on the arc, not the chord", "[presentation]") {
  const Vec3 a{10, 0, 0}, b{0, 0, 10};
  CHECK(Distance(ArcPosition(a, b, 0.5), {0, 0, 0}) == Approx(10.0).margin(1e-9));
  CHECK(Distance(ArcPosition(a, b, 0.5), {7.0710678, 0, 7.0710678}) < 1e-6);
  CHECK(Distance(ArcPosition(a, b, 0.0), a) < 1e-9);
  CHECK(Distance(ArcPosition(a, b, 1.0), b) < 1e-9);
  CHECK(Distance(ArcPosition(a, {0, 0, 20}, 0.5), {0, 0, 0}) == Approx(15.0).margin(1e-9));  // the distance linearly
  CHECK(Distance(ArcPosition(a, {-10, 0, 0}, 0.5), {0, 0, 0}) < 1e-9);  // opposite: no way round is known, linearly
}
