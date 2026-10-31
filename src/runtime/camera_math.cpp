#include "runtime/camera_math.h"

#include <algorithm>
#include <cmath>

namespace kuf::camera {

double Radians(double degrees) { return degrees * (M_PI / 180.0); }

Stick ShapeStick(int16_t raw_x, int16_t raw_y, double deadzone) {
  const double x = raw_x / 32767.0, y = raw_y / 32767.0;
  const double magnitude = std::min(std::hypot(x, y), 1.0);
  deadzone = std::clamp(deadzone, 0.0, 0.95);
  if (magnitude <= deadzone || magnitude == 0) {
    return {};
  }
  const double scaled = std::pow((magnitude - deadzone) / (1.0 - deadzone), kStickExponent);
  const double length = std::hypot(x, y);
  return {x / length * scaled, y / length * scaled};
}

namespace {
double LevelSpeed(int level, double step, double max) {
  level = std::clamp(level, pc::kLevelMin, pc::kLevelMax);
  const double linear_top = step * pc::kLevelLinearMax;
  if (level <= pc::kLevelLinearMax) {
    return step * level;
  }
  const double factor = std::pow(max / linear_top, 1.0 / (pc::kLevelMax - pc::kLevelLinearMax));
  return linear_top * std::pow(factor, level - pc::kLevelLinearMax);
}
}  // namespace

double MouseRadiansPerCount(int level) { return Radians(LevelSpeed(level, kMouseStepDegrees, kMouseMaxDegrees)); }

double StickRadiansPerSecond(int level) { return Radians(LevelSpeed(level, kStickStepDegrees, kStickMaxDegrees)); }

double VerticalFovFor(double key_vertical, int horizontal_degrees) {
  if (horizontal_degrees == pc::kFovOriginal || !(key_vertical > 0 && key_vertical < M_PI)) {
    return key_vertical;
  }
  // The reference camera's vertical angle for this horizontal angle at 16:9, as a factor on every key's.
  const double wanted = std::tan(Radians(horizontal_degrees) / 2) * 9.0 / 16.0;
  const double factor = wanted / std::tan(kReferenceVerticalFov / 2);
  return 2 * std::atan(std::tan(key_vertical / 2) * factor);
}

Vec3 SmoothAround(const Vec3& from, const Vec3& to, double fraction) {
  const double from_r = std::hypot(from.x, from.z), to_r = std::hypot(to.x, to.z);
  const double y = from.y + (to.y - from.y) * fraction;
  if (from_r < 1e-6 || to_r < 1e-6) {  // no direction to turn: the straight line
    return {from.x + (to.x - from.x) * fraction, y, from.z + (to.z - from.z) * fraction};
  }
  const double from_a = std::atan2(from.x, from.z);
  const double turn = std::remainder(std::atan2(to.x, to.z) - from_a, 2 * M_PI);
  if (std::abs(turn) < 1e-9) {  // the same direction: the straight line (exactly)
    return {from.x + (to.x - from.x) * fraction, y, from.z + (to.z - from.z) * fraction};
  }
  const double a = from_a + turn * fraction, r = from_r + (to_r - from_r) * fraction;
  return {r * std::sin(a), y, r * std::cos(a)};
}

Vec3 RotateWith(const Vec3& v, const Vec3& from, const Vec3& to) {
  const double fl = std::hypot(from.x, from.y, from.z), tl = std::hypot(to.x, to.y, to.z);
  if (fl < 1e-9 || tl < 1e-9) {
    return v;
  }
  const Vec3 a{from.x / fl, from.y / fl, from.z / fl}, b{to.x / tl, to.y / tl, to.z / tl};
  const Vec3 axis{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};  // sin(angle) * unit axis
  const double c = a.x * b.x + a.y * b.y + a.z * b.z;                                   // cos(angle)
  if (c < -1.0 + 1e-9) {
    return v;
  }
  // Rodrigues' formula: v c + (k x v) + k (k . v) / (1 + c), with k = a x b.
  const Vec3 kxv{axis.y * v.z - axis.z * v.y, axis.z * v.x - axis.x * v.z, axis.x * v.y - axis.y * v.x};
  const double kv = (axis.x * v.x + axis.y * v.y + axis.z * v.z) / (1.0 + c);
  return {v.x * c + kxv.x + axis.x * kv, v.y * c + kxv.y + axis.y * kv, v.z * c + kxv.z + axis.z * kv};
}

double HorizontalFov(double vertical, double aspect) { return 2 * std::atan(std::tan(vertical / 2) * aspect); }

double ViewPitch(const Vec3& eye, const Vec3& target) {
  const double dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
  return std::atan2(dy, std::hypot(dx, dz));
}

double SoftDepth(double depth, double max_depth) {
  // Identity up to (max_depth - blend), then an exponential approach to max_depth: the value and its slope are
  // continuous where the limit takes over.
  const double knee = max_depth - kGroundBlend;
  if (depth <= knee) {
    return depth;
  }
  return max_depth - kGroundBlend * std::exp(-(depth - knee) / kGroundBlend);
}

double LimitUpward(double input, bool looking_up, double final_distance, double dt) {
  if (!looking_up) {
    return input;
  }
  if (input > 0) {
    const double t = std::clamp((final_distance - kNearDistance) / (kComfortDistance - kNearDistance), 0.0, 1.0);
    input *= t * t * (3 - 2 * t);  // smoothstep: no sudden stop
  }
  if (final_distance < kNearDistance) {
    input -= kBackOff * dt * (kNearDistance - final_distance) / kNearDistance;
  }
  return input;
}

View ApplyOrbit(const Vec3& eye, const Vec3& pivot, double offset, double max_angle, double max_depth,
                double min_distance) {
  const double dx = eye.x - pivot.x, dy = eye.y - pivot.y, dz = eye.z - pivot.z;
  const double horizontal = std::hypot(dx, dz);
  const double radius = std::sqrt(horizontal * horizontal + dy * dy);
  if (radius < 1e-3 || horizontal < 1e-6) {
    return {eye, pivot, 0};  // degenerate (straight above or below): leave the game's camera
  }
  const double base = -std::atan2(dy, horizontal);  // the game's view angle above the horizon
  max_angle = std::clamp(max_angle, 0.0, Radians(89.0));
  const double angle = std::clamp(base + offset, -max_angle, max_angle);
  const double ux = dx / horizontal, uz = dz / horizontal;  // horizontally from the pivot towards the eye

  double distance = radius;
  if (angle > 0) {  // looking up: the eye below the pivot
    max_depth = std::max(max_depth, kGroundBlend + 1.0);
    const double depth = radius * std::sin(angle);
    const double allowed = SoftDepth(depth, max_depth);
    if (allowed < depth) {
      distance = std::max(allowed / std::sin(angle), std::min(min_distance, radius));
    }
  }
  View view;
  view.offset = angle - base;
  view.target = pivot;
  view.eye = {pivot.x + distance * std::cos(angle) * ux, pivot.y - distance * std::sin(angle),
              pivot.z + distance * std::cos(angle) * uz};
  return view;
}

Vec3 RotateAbout(const Vec3& v, const Vec3& k, double angle) {
  const double c = std::cos(angle), s = std::sin(angle);
  const Vec3 cross{k.y * v.z - k.z * v.y, k.z * v.x - k.x * v.z, k.x * v.y - k.y * v.x};
  const double dot = k.x * v.x + k.y * v.y + k.z * v.z;
  return {v.x * c + cross.x * s + k.x * dot * (1 - c), v.y * c + cross.y * s + k.y * dot * (1 - c),
          v.z * c + cross.z * s + k.z * dot * (1 - c)};
}

Basis UnpitchBasis(const Basis& pitched, double delta) {
  // About the right axis, a positive angle (right-hand rule) turns forward downwards (right = up x forward).
  Basis b = pitched;
  b.forward = RotateAbout(pitched.forward, pitched.right, delta);
  b.up = RotateAbout(pitched.up, pitched.right, delta);
  return b;
}

double Recenter(double offset, double idle_seconds, double dt) {
  if (idle_seconds < kRecenterDelay || dt <= 0) {
    return offset;
  }
  return offset * std::exp(-dt / kRecenterTime);
}

}  // namespace kuf::camera
