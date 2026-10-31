// The modern camera's math (M6B), pure and unit-tested (tests/camera). The game-facing part is
// modern_camera.cpp; docs/CAMERA.md explains both.
#pragma once

#include <cstdint>

#include "runtime/pc_settings.h"

namespace kuf::camera {

struct Vec3 {
  double x = 0, y = 0, z = 0;
};

// A thumbstick's camera response: a radial dead zone (fraction of full travel), then a power curve so small
// deflections give fine control and full deflection gives full speed. The direction is kept. Result per axis in
// [-1, 1] (right / up positive, as the raw values).
struct Stick {
  double x = 0, y = 0;
};
constexpr double kStickExponent = 1.5;
Stick ShapeStick(int16_t raw_x, int16_t raw_y, double deadzone);

// Speeds from the settings levels: evenly spaced up to pc::kLevelLinearMax (fine steps where small changes matter;
// these are the levels of the first M6B build, so saved settings keep their speed), then each level a fixed
// factor faster, up to kMouseMaxDegrees / kStickMaxDegrees at pc::kLevelMax.
constexpr double kMouseStepDegrees = 0.025, kMouseMaxDegrees = 1.5;   // degrees per mouse count
constexpr double kStickStepDegrees = 15.0, kStickMaxDegrees = 600.0;  // degrees per second at full deflection
double MouseRadiansPerCount(int level);
double StickRadiansPerSecond(int level);  // yaw at full deflection
constexpr double kStickPitchRatio = 0.65; // vertical speed relative to horizontal (stick)

// The vertical view angle as an orbit around the pivot, applied after the game has placed the camera (and turned
// it horizontally). The pivot is the game's own camera target on the character (its upper body).
//
// `offset` (radians, positive = look up) changes the view's angle above the horizon from the game's default; it is
// limited to +/- max_angle and the returned offset is the one actually used (the caller keeps it, so the limit has
// no dead zone when reversing). The eye moves on a circle around the pivot and always looks at it: looking down
// raises the eye over the character (to a top-down view), looking up lowers it below the pivot so the character is
// seen from below. The orbit distance stays the game's, except near the ground: the eye may go `max_depth` below
// the pivot (the pivot's height above the character's feet less a clearance); as an orbit would go deeper, the
// distance shrinks smoothly towards the character (a soft limit, continuous in position and in its rate, never an
// abrupt stop), but not below `min_distance`. At offset 0 the result is exactly the game's camera.
struct View {
  Vec3 eye, target;
  double offset = 0;
};
constexpr double kGroundBlend = 60.0;  // world units over which the soft ground limit takes over
View ApplyOrbit(const Vec3& eye, const Vec3& pivot, double offset, double max_angle, double max_depth,
                double min_distance);

// Upward look near obstacles: when the game's collision has pulled a low (looking-up) camera in to
// `final_distance` from the pivot, further upward input is scaled down smoothly from kComfortDistance to zero at
// kNearDistance (the orbit eases to a stop instead of reaching into the character), and below kNearDistance
// the view eases back down at up to kBackOff radians per second. Returns the vertical input to apply.
constexpr double kComfortDistance = 140.0, kNearDistance = 80.0, kBackOff = 0.6;
double LimitUpward(double input, bool looking_up, double final_distance, double dt);

// The game's camera position smoothing (a fraction of the way per frame, in a straight line) done around a centre:
// `from` and `to` are offsets from it; the direction (azimuth, the short way round), the horizontal distance and
// the height each move `fraction` of the way. Without a change of direction this is the straight line exactly; a
// turn travels around the centre instead of across it.
Vec3 SmoothAround(const Vec3& from, const Vec3& to, double fraction);

// `v` turned by the rotation that takes the direction of `from` to the direction of `to` (the shortest one; `v`
// unchanged if either is zero or they point in opposite directions).
Vec3 RotateWith(const Vec3& v, const Vec3& from, const Vec3& to);

// The depth below the pivot that `ApplyOrbit` lets an orbit of this depth reach (the soft ground limit).
double SoftDepth(double depth, double max_depth);

// Field of view. The game's projection takes a vertical angle from the camera key (radians; Celine's gameplay
// camera 0.6, 57.6 degrees horizontally at 16:9) and the viewport's aspect. The setting is a horizontal angle at
// 16:9 for that camera; every key's own angle is scaled by the same factor (tan of the half angles), so cameras
// that the game made narrower stay narrower. Wider screens see more at the sides (the vertical angle is kept).
constexpr double kReferenceVerticalFov = 0.6;  // radians: the gameplay camera's own
double VerticalFovFor(double key_vertical, int horizontal_degrees);  // 0 (pc::kFovOriginal): key_vertical
double HorizontalFov(double vertical, double aspect);                 // radians

// The view's angle above the horizon (radians) for eye -> target.
double ViewPitch(const Vec3& eye, const Vec3& target);

// `v` rotated by `angle` (radians, right-hand rule) about the unit vector `axis`.
Vec3 RotateAbout(const Vec3& v, const Vec3& axis, double angle);

// The game's camera basis (rows right, up, forward; forward = eye -> target, right = up x forward, up = forward
// x right) and position. UnpitchBasis turns a pitched camera back by `delta` (radians, the view angle that was
// added, + = up) about its own right axis: the orientation the game itself would have had.
struct Basis {
  Vec3 right, up, forward, position;
};
Basis UnpitchBasis(const Basis& pitched, double delta);

// Automatic recentring: after `delay` seconds without vertical input the offset eases back to 0 with this time
// constant (seconds).
constexpr double kRecenterDelay = 1.0, kRecenterTime = 0.35;
double Recenter(double offset, double idle_seconds, double dt);

double Radians(double degrees);

}  // namespace kuf::camera
