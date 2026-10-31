#include "runtime/presentation_math.h"

#include <algorithm>
#include <cmath>

namespace kuf::presentation {
namespace {

Vec3 Row(const Mat4& m, int i) { return {m[i * 4], m[i * 4 + 1], m[i * 4 + 2]}; }
void SetRow(Mat4& m, int i, const Vec3& v, float w) {
  m[i * 4] = float(v.x);
  m[i * 4 + 1] = float(v.y);
  m[i * 4 + 2] = float(v.z);
  m[i * 4 + 3] = w;
}
double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 Scale(const Vec3& v, double s) { return {v.x * s, v.y * s, v.z * s}; }
Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
Vec3 Lerp(const Vec3& a, const Vec3& b, double t) { return Add(a, Scale(Sub(b, a), t)); }
double LerpD(double a, double b, double t) { return a + (b - a) * t; }

// From spherical coordinates in the game's convention (azimuth atan2(x, z), elevation above the xz plane).
Vec3 Direction(double azimuth, double elevation) {
  return {std::sin(azimuth) * std::cos(elevation), std::sin(elevation), std::cos(azimuth) * std::cos(elevation)};
}

// The azimuth change from a to b: the shorter way, or the way of `turn` (a known signed change, NaN if unknown).
double AzimuthChange(double a, double b, double turn) {
  const double shorter = std::remainder(b - a, 2 * M_PI);
  if (!std::isfinite(turn)) return shorter;
  return shorter + 2 * M_PI * std::round((turn - shorter) / (2 * M_PI));
}

}  // namespace

Vec3 Forward(const Mat4& m) { return Row(m, 2); }
Vec3 Position(const Mat4& m) { return Row(m, 3); }
double Azimuth(const Vec3& v) { return std::atan2(v.x, v.z); }
double Elevation(const Vec3& v) { return std::atan2(v.y, std::hypot(v.x, v.z)); }

Rigid Decompose(const Mat4& m) {
  Rigid r;
  const Vec3 r0 = Row(m, 0), r1 = Row(m, 1), r2 = Row(m, 2);
  r.scale = {Length(r0), Length(r1), Length(r2)};
  r.position = Row(m, 3);
  if (!(r.scale.x > 1e-9 && r.scale.y > 1e-9 && r.scale.z > 1e-9) || !std::isfinite(Dot(r.position, r.position))) {
    return r;
  }
  // Orthonormal rows (right, up, forward), forward kept: forward x right = up.
  const Vec3 f = Scale(r2, 1 / r.scale.z);
  Vec3 x = Sub(r0, Scale(f, Dot(r0, f)));
  const double lx = Length(x);
  if (lx < 1e-9) return r;
  x = Scale(x, 1 / lx);
  const Vec3 y = Cross(f, x);
  if (Dot(y, r1) <= 0) return r;  // a reflection
  // The rotation matrix with these rows, as a quaternion (m[i][j] = row i, component j).
  const double m00 = x.x, m01 = x.y, m02 = x.z, m10 = y.x, m11 = y.y, m12 = y.z, m20 = f.x, m21 = f.y, m22 = f.z;
  const double trace = m00 + m11 + m22;
  Quat q;
  if (trace > 0) {
    const double s = 0.5 / std::sqrt(trace + 1);
    q = {0.25 / s, (m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s};
  } else if (m00 > m11 && m00 > m22) {
    const double s = 2 * std::sqrt(1 + m00 - m11 - m22);
    q = {(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s};
  } else if (m11 > m22) {
    const double s = 2 * std::sqrt(1 + m11 - m00 - m22);
    q = {(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s};
  } else {
    const double s = 2 * std::sqrt(1 + m22 - m00 - m11);
    q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s};
  }
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  r.rotation = {q.w / n, q.x / n, q.y / n, q.z / n};
  r.valid = true;
  return r;
}

Mat4 Compose(const Rigid& r) {
  const Quat& q = r.rotation;
  const double w = q.w, x = q.x, y = q.y, z = q.z;
  const Vec3 r0{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)};
  const Vec3 r1{2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)};
  const Vec3 r2{2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
  Mat4 m{};
  SetRow(m, 0, Scale(r0, r.scale.x), 0);
  SetRow(m, 1, Scale(r1, r.scale.y), 0);
  SetRow(m, 2, Scale(r2, r.scale.z), 0);
  SetRow(m, 3, r.position, 1);
  return m;
}

Quat Slerp(Quat a, Quat b, double t) {
  double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
  if (d < 0) {
    b = {-b.w, -b.x, -b.y, -b.z};
    d = -d;
  }
  double ka = 1 - t, kb = t;
  if (d < 0.9995) {
    const double angle = std::acos(std::clamp(d, -1.0, 1.0)), s = std::sin(angle);
    ka = std::sin((1 - t) * angle) / s;
    kb = std::sin(t * angle) / s;
  }
  Quat q{a.w * ka + b.w * kb, a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb};
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  return {q.w / n, q.x / n, q.y / n, q.z / n};
}

double AngleBetween(const Quat& a, const Quat& b) {
  const double d = std::abs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
  return 2 * std::acos(std::clamp(d, 0.0, 1.0));
}

Mat4 InterpolateRigid(const Mat4& a, const Mat4& b, double t) {
  if (t <= 0) return a;
  if (t >= 1) return b;
  const Rigid ra = Decompose(a), rb = Decompose(b);
  if (!ra.valid || !rb.valid) return b;
  return InterpolateRigid(ra, rb, t);
}

Mat4 InterpolateRigid(const Rigid& a, const Rigid& b, double t) {
  Rigid r;
  r.valid = true;
  r.rotation = Slerp(a.rotation, b.rotation, t);
  r.scale = Lerp(a.scale, b.scale, t);
  r.position = Lerp(a.position, b.position, t);
  return Compose(r);
}

Mat4 LookAt(const Vec3& forward, const Vec3& position) {
  const double lf = Length(forward);
  const Vec3 f = lf > 1e-12 ? Scale(forward, 1 / lf) : Vec3{0, 0, 1};
  Vec3 right{f.z, 0, -f.x};  // the world's up x forward
  const double lr = Length(right);
  right = lr > 1e-12 ? Scale(right, 1 / lr) : Vec3{1, 0, 0};
  const Vec3 up = Cross(f, right);
  Mat4 m{};
  SetRow(m, 0, right, 0);
  SetRow(m, 1, up, 0);
  SetRow(m, 2, f, 0);
  SetRow(m, 3, position, 1);
  return m;
}

double LookAtError(const Mat4& m) {
  const Mat4 l = LookAt(Forward(m), Position(m));
  double e = 0;
  for (int i = 0; i < 12; ++i) e = std::max(e, double(std::abs(l[i] - m[i])));
  for (int i = 12; i < 15; ++i) {
    if (!std::isfinite(m[i])) return 1e9;
  }
  return std::abs(m[15] - 1) > 1e-6 ? 1e9 : e;
}

Mat4 InterpolateCamera(const CameraTick& a, const CameraTick& b, double t, double turn, bool pulled_in) {
  if (t <= 0) return a.matrix;
  if (t >= 1) return b.matrix;
  constexpr double kLookAt = 1e-3;  // float rounding of the game's own look-at is about 1e-7
  if (LookAtError(a.matrix) > kLookAt || LookAtError(b.matrix) > kLookAt) return InterpolateRigid(a.matrix, b.matrix, t);
  const Vec3 ea = Position(a.matrix), eb = Position(b.matrix), fa = Forward(a.matrix), fb = Forward(b.matrix);
  if (a.have_pivot && b.have_pivot) {
    // The pivot as the point on each view's line of sight nearest the camera's pivot: the view is aimed at it
    // exactly, so the end points are the ticks' cameras.
    const Vec3 pa = Add(ea, Scale(fa, Dot(Sub(a.pivot, ea), fa) / Dot(fa, fa)));
    const Vec3 pb = Add(eb, Scale(fb, Dot(Sub(b.pivot, eb), fb) / Dot(fb, fb)));
    const Vec3 oa = Sub(ea, pa), ob = Sub(eb, pb);
    const double da = Length(oa), db = Length(ob);
    if (da > 1e-3 && db > 1e-3 && Dot(oa, fa) < 0 && Dot(ob, fb) < 0) {
      const double az = Azimuth(oa) + AzimuthChange(Azimuth(oa), Azimuth(ob), turn) * t;
      const double el = LerpD(Elevation(oa), Elevation(ob), t);
      const double d = pulled_in ? db : LerpD(da, db, t);
      const Vec3 pivot = Lerp(pa, pb, t);
      const Vec3 offset = Scale(Direction(az, el), d);
      return LookAt(Scale(offset, -1), Add(pivot, offset));
    }
  }
  const double az = Azimuth(fa) + AzimuthChange(Azimuth(fa), Azimuth(fb), turn) * t;
  const double el = LerpD(Elevation(fa), Elevation(fb), t);
  return LookAt(Direction(az, el), Lerp(ea, eb, t));
}

double Alpha(double since_tick, double tick_seconds) {
  if (!(tick_seconds > 0) || !std::isfinite(since_tick)) return 1;
  return std::clamp(since_tick / tick_seconds, 0.0, 1.0);
}

namespace {

double Distance(const Vec3& a, const Vec3& b) { return Length(Sub(a, b)); }

// The angle between two views' directions (radians).
double ViewTurn(const Mat4& a, const Mat4& b) {
  const Vec3 fa = Forward(a), fb = Forward(b);
  return std::acos(std::clamp(Dot(fa, fb) / std::max(1e-12, Length(fa) * Length(fb)), -1.0, 1.0));
}

const char* CameraCut(const Snapshot& a, const Snapshot& b) {
  if (!a.valid) return "first";
  if (b.tick != a.tick + 1) return "gap";
  if (!a.world || !b.world) return "not world";
  if (a.renderer != b.renderer || a.view != b.view || !a.have_camera || !b.have_camera) return "view";
  if (!a.fresh || !b.fresh) return "no view";
  if (a.view_info.event != b.view_info.event) return "event";
  if (a.view_info.camera != b.view_info.camera) return "camera";
  if (a.view_info.modern != b.view_info.modern) return "handover";
  if (b.view_info.snapped) return "snapped";
  const bool around = a.view_info.modern && b.view_info.modern;
  const Vec3 ea = Position(a.camera), eb = Position(b.camera);
  if (b.view_info.pulled_in && (!around || ViewTurn(a.camera, b.camera) > kPulledTurn)) return "pulled in";
  if (Distance(ea, eb) > kCameraJump) return "jump";
  if (!around && a.have_step && b.have_step && Length(Sub(b.eye_step, a.eye_step)) > kShotStepChange) return "shot";
  if (b.view_info.event && (Distance(ea, eb) > kEventMove || ViewTurn(a.camera, b.camera) > kEventTurn)) return "shot";
  return nullptr;
}

const char* PlayerCut(const Snapshot& a, const Snapshot& b) {
  if (!b.player) return "none";
  if (!a.valid) return "first";
  if (b.tick != a.tick + 1) return "gap";
  if (!a.world || !b.world) return "not world";
  if (a.view_info.event || b.view_info.event) return "event";
  if (a.player != b.player || a.player_index != b.player_index) return "object";
  if (Distance(Position(a.player_matrix), Position(b.player_matrix)) > kPlayerJump) return "teleport";
  const auto ra = Decompose(a.player_matrix), rb = Decompose(b.player_matrix);
  if (!ra.valid || !rb.valid) return "matrix";
  if (AngleBetween(ra.rotation, rb.rotation) > kPlayerTurn) return "turn";
  return nullptr;
}

}  // namespace

Interval Decide(const Snapshot& a, const Snapshot& b) {
  Interval i;
  i.camera_cut = CameraCut(a, b);
  i.player_cut = PlayerCut(a, b);
  if (!i.camera_cut) {
    i.fov = a.fov_setting == b.fov_setting;
    i.pulled_in = b.view_info.pulled_in;
    if (a.view_info.modern && b.view_info.modern) i.turn = b.view_info.turn;
  }
  return i;
}

bool IsRigid(const Mat4& m) {
  const Rigid r = Decompose(m);
  if (!r.valid || std::abs(m[3]) + std::abs(m[7]) + std::abs(m[11]) + std::abs(m[15] - 1) > 1e-6) return false;
  const Mat4 c = Compose(r);
  const double size = std::max({r.scale.x, r.scale.y, r.scale.z, 1.0});
  for (int i = 0; i < 12; ++i) {
    if (std::abs(c[i] - m[i]) > kRigidError * size) return false;
  }
  return true;
}

const char* ObjectCut(const ObjectTick& a, const ObjectTick& b) {
  if (a.index != b.index || a.object != b.object || a.vtable != b.vtable || a.generation != b.generation) {
    return "object";
  }
  if (Distance(Position(a.matrix), Position(b.matrix)) > kObjectJump) return "teleport";
  if (!IsRigid(a.matrix) || !IsRigid(b.matrix)) return "matrix";
  if (AngleBetween(Decompose(a.matrix).rotation, Decompose(b.matrix).rotation) > kObjectTurn) return "turn";
  return nullptr;
}

namespace {

// Row-vector affine transforms in double: p * M, rows right, up, forward, position.
using Mat4d = std::array<double, 16>;
Mat4d ToD(const Mat4& m) {
  Mat4d d;
  for (int i = 0; i < 16; ++i) d[i] = m[i];
  return d;
}
Mat4 ToF(const Mat4d& d) {
  Mat4 m;
  for (int i = 0; i < 16; ++i) m[i] = float(d[i]);
  return m;
}
Mat4d Multiply(const Mat4d& a, const Mat4d& b) {
  Mat4d r{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = v;
    }
  }
  return r;
}
// The inverse of an affine matrix (last column 0, 0, 0, 1); false if singular.
bool InverseAffine(const Mat4d& m, Mat4d* out) {
  const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
  const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
  if (!(std::abs(det) > 1e-12) || !std::isfinite(det)) return false;
  const double s = 1 / det;
  Mat4d r{};
  r[0] = (e * k - f * h) * s, r[1] = (c * h - b * k) * s, r[2] = (b * f - c * e) * s;
  r[4] = (f * g - d * k) * s, r[5] = (a * k - c * g) * s, r[6] = (c * d - a * f) * s;
  r[8] = (d * h - e * g) * s, r[9] = (b * g - a * h) * s, r[10] = (a * e - b * d) * s;
  for (int j = 0; j < 3; ++j) r[12 + j] = -(m[12] * r[j] + m[13] * r[4 + j] + m[14] * r[8 + j]);
  r[15] = 1;
  *out = r;
  return true;
}
// Decompose of a double matrix (via float: the palettes are floats) that must give the matrix back (IsRigid's test;
// a product of affine matrices keeps the last column 0, 0, 0, 1 exactly).
bool RigidOf(const Mat4d& m, Rigid* out) {
  const Mat4 f = ToF(m);
  if (!IsRigid(f)) return false;
  *out = Decompose(f);
  return true;
}
bool ValidParents(const std::vector<int>& parents, size_t n) {
  if (parents.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (parents[i] >= int(i) || parents[i] < -1) return false;  // parents come first
  }
  return true;
}

}  // namespace

PosePair PreparePose(const PoseTick& a, const PoseTick& b) {
  PosePair p;
  const size_t n = b.bones.size();
  if (n == 0 || a.bones.size() != n) return p;
  p.a.resize(n);
  p.b.resize(n);
  // In each bone's parent space: local = model x inverse(parent's model), so bone lengths stay as the game's.
  if (ValidParents(a.parents, n) && a.parents == b.parents) {
    p.local = true;
    for (size_t i = 0; i < n && p.local; ++i) {
      const int parent = a.parents[i];
      for (int k = 0; k < 2 && p.local; ++k) {
        const PoseTick& t = k ? b : a;
        Mat4d m = ToD(t.bones[i]);
        if (parent >= 0) {
          Mat4d inv;
          if (!InverseAffine(ToD(t.bones[parent]), &inv)) {
            p.local = false;
            break;
          }
          m = Multiply(m, inv);
        }
        if (!RigidOf(m, k ? &p.b[i] : &p.a[i])) p.local = false;
      }
    }
    if (p.local) p.parents = a.parents;
  }
  if (!p.local) {  // each bone in model space
    for (size_t i = 0; i < n; ++i) {
      if (!RigidOf(ToD(a.bones[i]), &p.a[i]) || !RigidOf(ToD(b.bones[i]), &p.b[i])) return p;
    }
  }
  p.hold.assign(n, false);
  for (size_t i = 0; i < n; ++i) {
    const double turn = AngleBetween(p.a[i].rotation, p.b[i].rotation);
    if (turn > kBoneTurn) {
      p.hold[i] = true;
      ++p.held;
      continue;
    }
    p.turn_max = std::max(p.turn_max, turn);
  }
  p.valid = true;
  return p;
}

void InterpolatePose(const PoseTick& a, const PoseTick& b, const PosePair& p, double t, std::vector<Mat4>* out) {
  if (t <= 0 || !p.valid) {
    *out = t <= 0 ? a.bones : b.bones;
    return;
  }
  if (t >= 1) {
    *out = b.bones;
    return;
  }
  const size_t n = p.a.size();
  out->resize(n);
  std::vector<Mat4d> model(p.local ? n : 0);
  for (size_t i = 0; i < n; ++i) {
    const Mat4d m = ToD(p.hold[i] ? Compose(p.b[i]) : InterpolateRigid(p.a[i], p.b[i], t));
    if (!p.local) {
      (*out)[i] = ToF(m);
      continue;
    }
    const int parent = p.parents[i];
    model[i] = parent >= 0 ? Multiply(m, model[parent]) : m;
    (*out)[i] = ToF(model[i]);
  }
}

double PoseProgress(const PosePair& p, const std::vector<Mat4>& pose) {
  if (!p.valid || pose.size() != p.a.size()) return NAN;
  double sum = 0;
  int n = 0;
  for (size_t i = 0; i < pose.size(); ++i) {
    const double total = AngleBetween(p.a[i].rotation, p.b[i].rotation);
    if (p.hold[i] || total < 1e-3) continue;
    Mat4d m = ToD(pose[i]);
    const int parent = p.local ? p.parents[i] : -1;
    if (parent >= 0) {
      Mat4d inv;
      if (!InverseAffine(ToD(pose[parent]), &inv)) continue;
      m = Multiply(m, inv);
    }
    const Rigid r = Decompose(ToF(m));
    if (!r.valid) continue;
    sum += AngleBetween(p.a[i].rotation, r.rotation) / total;
    ++n;
  }
  return n ? sum / n : NAN;
}

const char* PoseCut(const PoseTick& a, const PoseTick& b, const PosePair& p) {
  if (a.object != b.object || a.skeleton != b.skeleton || a.bones.size() != b.bones.size() || a.parents != b.parents) {
    return "skeleton";
  }
  // The game crossfades a new animation: the old clip becomes a blend layer it fades out over about seven ticks
  // (weights 6/7 ... 2/7), the base layer changing to the new clip during the fade (docs/HIGH_FPS.md 17). A change of
  // the base clips with no fade in either tick is a switch the game shows at once.
  bool changed = a.base.size() != b.base.size();
  for (size_t i = 0; !changed && i < a.base.size(); ++i) changed = a.base[i].clip != b.base[i].clip;
  if (changed && a.blend.empty() && b.blend.empty()) return "snap";
  if (!p.valid) return "matrix";
  return nullptr;
}

PartPair PreparePart(const std::vector<Mat4>& part_a, const std::vector<Mat4>& part_b,
                     const std::vector<Mat4>& model_a, const std::vector<Mat4>& model_b) {
  PartPair p;
  const size_t n = part_b.size(), m = model_b.size();
  if (n == 0 || part_a.size() != n || m == 0 || model_a.size() != m) return p;
  // Candidate anchors: the model's bones (0 .. m - 1), then the part's own bones before the one placed (m + i).
  std::vector<Mat4d> inv_a(m + n), inv_b(m + n);
  std::vector<bool> usable(m + n, true);
  for (size_t k = 0; k < m + n; ++k) {
    const Mat4& a = k < m ? model_a[k] : part_a[k - m];
    const Mat4& b = k < m ? model_b[k] : part_b[k - m];
    usable[k] = InverseAffine(ToD(a), &inv_a[k]) && InverseAffine(ToD(b), &inv_b[k]);
  }
  p.anchor.assign(n, -1);
  p.model_bones = m;
  p.a.resize(n);
  p.b.resize(n);
  p.hold.assign(n, false);
  p.distance_a.assign(n, 0);
  p.distance_b.assign(n, 0);
  for (size_t j = 0; j < n; ++j) {
    const Mat4d ja = ToD(part_a[j]), jb = ToD(part_b[j]);
    // The anchor: the bone the relation to which changed least over the tick (largest element) and stays a rigid
    // transform at both ticks (a bone scaled unevenly would shear it): the model's bone a part bone hangs from, or
    // for a link of a chain the link before it, if that relation changed less than the bone itself did in the
    // model's space. None (a body moving on its own, e.g. thrown free): the part bone in the model's space, where
    // its position moves on a straight line between the ticks.
    double best = 0;
    for (int e = 0; e < 16; ++e) best = std::max(best, std::abs(ja[e] - jb[e]));
    for (size_t k = 0; k < m + j; ++k) {
      if (!usable[k]) continue;
      const Mat4d ra = Multiply(ja, inv_a[k]), rb = Multiply(jb, inv_b[k]);
      double change = 0;
      for (int e = 0; e < 16; ++e) change = std::max(change, std::abs(ra[e] - rb[e]));
      Rigid a, b;
      if (change < best && RigidOf(ra, &a) && RigidOf(rb, &b)) {
        best = change;
        p.a[j] = a;
        p.b[j] = b;
        p.anchor[j] = int(k);
      }
    }
    const int k = p.anchor[j];
    if (k < 0) {
      if (!RigidOf(ja, &p.a[j]) || !RigidOf(jb, &p.b[j])) return p;
      ++p.unanchored;
    } else {
      p.distance_a[j] = Distance(Position(part_a[j]), Position(size_t(k) < m ? model_a[k] : part_a[k - m]));
      p.distance_b[j] = Distance(Position(part_b[j]), Position(size_t(k) < m ? model_b[k] : part_b[k - m]));
    }
    const double turn = AngleBetween(p.a[j].rotation, p.b[j].rotation);
    // Its offset from the anchor turned by more than kBoneTurn as well (to the other side of the anchor, the bone's
    // own rotation little changed): no arc between the ticks is known (ArcPosition would take the line through the
    // anchor, 9.8 units short of the bone's length in one fight, D5 validation); held like a turned bone.
    const double la = Length(p.a[j].position), lb = Length(p.b[j].position);
    const double swing = k >= 0 && la > 1e-6 && lb > 1e-6
                             ? std::acos(std::clamp(Dot(p.a[j].position, p.b[j].position) / (la * lb), -1.0, 1.0))
                             : 0.0;
    if (turn > kBoneTurn || swing > kBoneTurn) {
      p.hold[j] = true;
      ++p.held;
    } else {
      p.turn_max = std::max(p.turn_max, turn);
    }
  }
  p.valid = true;
  return p;
}

// A part bone's position relative to its anchor between the ticks: its direction the shorter way round and its
// distance linearly (a bone swinging about its anchor moves on the arc, not the chord); directions nearly opposite
// or a zero offset: linearly.
Vec3 ArcPosition(const Vec3& a, const Vec3& b, double t) {
  const double la = Length(a), lb = Length(b);
  const Vec3 line{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
  if (la < 1e-6 || lb < 1e-6) return line;
  const Vec3 ua = Scale(a, 1 / la), ub = Scale(b, 1 / lb);
  const double c = std::clamp(Dot(ua, ub), -1.0, 1.0), angle = std::acos(c);
  if (angle < 1e-6 || angle > M_PI - 1e-3) return line;
  const double sa = std::sin((1 - t) * angle) / std::sin(angle), sb = std::sin(t * angle) / std::sin(angle);
  const double l = la + (lb - la) * t;
  return {(ua.x * sa + ub.x * sb) * l, (ua.y * sa + ub.y * sb) * l, (ua.z * sa + ub.z * sb) * l};
}

const Mat4* PartAnchor(const PartPair& p, size_t j, const std::vector<Mat4>& model, const std::vector<Mat4>& part) {
  const int k = p.anchor[j];
  if (k < 0) return nullptr;
  if (size_t(k) < p.model_bones) return size_t(k) < model.size() ? &model[k] : nullptr;
  return &part[k - p.model_bones];
}

void InterpolatePart(const PartPair& p, const std::vector<Mat4>& model, double t, std::vector<Mat4>* out) {
  const size_t n = p.anchor.size();
  out->resize(n);
  for (size_t j = 0; j < n; ++j) {  // anchors on the part come before the bones placed on them
    Mat4 m = p.hold[j] || t >= 1 ? Compose(p.b[j]) : t <= 0 ? Compose(p.a[j]) : InterpolateRigid(p.a[j], p.b[j], t);
    const Mat4* anchor = PartAnchor(p, j, model, *out);
    if (anchor && !p.hold[j] && t > 0 && t < 1) {
      const Vec3 v = ArcPosition(p.a[j].position, p.b[j].position, t);
      m[12] = float(v.x);
      m[13] = float(v.y);
      m[14] = float(v.z);
    }
    const Mat4d rel = ToD(m);
    (*out)[j] = anchor ? ToF(Multiply(rel, ToD(*anchor))) : ToF(rel);
  }
}

double PartProgress(const PartPair& p, const std::vector<Mat4>& model, const std::vector<Mat4>& part) {
  if (!p.valid || part.size() != p.anchor.size()) return NAN;
  double sum = 0;
  int n = 0;
  for (size_t j = 0; j < part.size(); ++j) {
    const double total = AngleBetween(p.a[j].rotation, p.b[j].rotation);
    if (p.hold[j] || total < 1e-3) continue;
    Mat4d inv = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const Mat4* anchor = PartAnchor(p, j, model, part);
    if ((p.anchor[j] >= 0 && !anchor) || (anchor && !InverseAffine(ToD(*anchor), &inv))) continue;
    const Rigid r = Decompose(ToF(Multiply(ToD(part[j]), inv)));
    if (!r.valid) continue;
    sum += AngleBetween(p.a[j].rotation, r.rotation) / total;
    ++n;
  }
  return n ? sum / n : NAN;
}

}  // namespace kuf::presentation
