// Presentation interpolation math and its discontinuity rules (M6D D3b-3), pure and unit-tested (tests/presentation).
// The game-facing part is presentation.cpp; docs/HIGH_FPS.md section 15 explains both.
//
// Matrices are the game's: 16 floats, rows right, up, forward (x, y, z, 0 each) and position (x, y, z, 1), a point
// p in object space at p * M (row vectors). The gameplay camera's rows are a look-at with the world's up (y): right =
// normalize(up x forward), up = forward x right, no roll (camera_math.h Basis).
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "runtime/camera_math.h"
#include "runtime/modern_camera.h"

namespace kuf::presentation {

using camera::Vec3;
using Mat4 = std::array<float, 16>;

struct Quat {
  double w = 1, x = 0, y = 0, z = 0;
};

// A matrix as rotation, per-row scale and position (rows orthonormalised, forward kept). `valid` is false for a
// degenerate matrix (a zero row, NaN) or one with a reflection.
struct Rigid {
  bool valid = false;
  Quat rotation;
  Vec3 scale{1, 1, 1};
  Vec3 position;
};
Rigid Decompose(const Mat4& m);
Mat4 Compose(const Rigid& r);

// The shorter way between two rotations (q and -q are the same rotation).
Quat Slerp(Quat a, Quat b, double t);
double AngleBetween(const Quat& a, const Quat& b);  // radians, 0..pi

// An object's transform between two ticks: position and scale linearly, rotation the shorter way (t 0..1; t 0 and
// 1 give the matrices themselves).
Mat4 InterpolateRigid(const Mat4& a, const Mat4& b, double t);
// The same from decomposed matrices (both valid; the per-object path decomposes once per tick).
Mat4 InterpolateRigid(const Rigid& a, const Rigid& b, double t);

// The camera's rows for a forward direction: the game's look-at with the world's up (forward kept).
Mat4 LookAt(const Vec3& forward, const Vec3& position);
// How far a camera matrix is from the look-at of its own forward (largest element difference): 0 for the
// gameplay camera, larger for a rolled camera (some event cameras).
double LookAtError(const Mat4& m);

// The camera between two ticks. With a pivot at both ticks (the modern camera: the point the view is aimed at) the
// eye moves around the pivot: the pivot linearly, the eye's offset from it by azimuth, elevation and distance, and
// the view is aimed at the pivot, as the camera itself places it every tick (a fast orbit or turn does not cut a
// chord past the character). `turn` is the azimuth change of the view over the tick when known (radians, signed,
// not wrapped: a turn of more than half a circle in one tick goes the way it was made); NaN: the shorter way.
// `pulled_in`: the game's collision pulled the camera in at once at the second tick, so the frames between use
// its distance (the camera does not pass through the scenery it was pulled out of). Without a pivot the eye moves
// linearly and the view direction by azimuth and elevation. A camera that is not a look-at (rolled) is
// interpolated as a rigid transform.
struct CameraTick {
  Mat4 matrix{};
  bool have_pivot = false;
  Vec3 pivot;
};
Mat4 InterpolateCamera(const CameraTick& a, const CameraTick& b, double t, double turn, bool pulled_in);

Vec3 Forward(const Mat4& m);
Vec3 Position(const Mat4& m);
double Azimuth(const Vec3& v);    // atan2(x, z): the game's heading convention
double Elevation(const Vec3& v);  // radians above the horizontal plane

// The interpolation factor of a frame built `since_tick` seconds after its tick started: the fraction of a tick
// since then, 0..1 (never beyond the newest tick).
double Alpha(double since_tick, double tick_seconds);

// ---- discontinuities ----

// One renderer object as a tick listed it: its identity and its world matrix for the slot as the game wrote it.
// `generation` counts the releases of its object index (presentation.cpp): an index is reused only after a release,
// so index, generation, address and class together name one object across ticks.
struct ObjectTick {
  uint32_t index = 0, object = 0, vtable = 0, generation = 0;
  Mat4 matrix{};
};

// What one tick handed to the renderer (presentation.cpp reads it at the tick's scene build).
struct Snapshot {
  bool valid = false;
  uint64_t tick = 0;
  bool world = false;                       // the tick marked its slot as a world frame (not loading, menus, pause)
  uint32_t renderer = 0, view = 0, slot = 0;
  bool have_camera = false, fresh = false;  // fresh: the game handed the renderer a view in this tick
  Mat4 camera{};
  bool have_step = false;  // eye_step known: the eye's move from the tick before (consecutive, both with a camera)
  Vec3 eye_step;
  float fov = 0;
  int fov_setting = 0;                      // PC Settings > Field of View
  camera::TickView view_info;
  uint32_t player = 0, player_index = 0;    // the player model (renderer object, its index); 0: none found
  Mat4 player_matrix{};
  double attach_error = 0;                  // its distance from the camera's attach point
  double unskinned_error = 0;               // the nearest model of its class without a bone palette (trace)
  bool restored = false;                    // a slot still held the presented matrix: the game's own taken back
  std::vector<ObjectTick> objects;          // the other listed world objects (D4a; presentation.cpp says which)
};

// Backstops for discontinuities the game does not signal (the explicit ones come first in Decide).
constexpr double kAttachMatch = 2.0;    // world units: the player model stands at the camera's attach point
constexpr double kAttachKeep = 60.0;    // ... the previous tick's player is kept within this: stopping from a run,
                                        // the model's root is up to 24 units from the attach point (measured)
// Measured in the scripted fight at 120 (gameplay.combat_loot, about 17,400 ticks with the player): the model moves
// at most 77 units in a tick (a drop, p99 38) and turns at most 63 degrees (p99 15).
constexpr double kPlayerJump = 400.0;   // world units in one tick: a teleport
constexpr double kPlayerTurn = 2.6;     // radians in one tick: the model turned at once (about 150 degrees)
constexpr double kCameraJump = 2500.0;  // world units in one tick: the camera was put elsewhere
constexpr double kEventMove = 250.0;    // event cameras: a cut between shots (units, radians in one tick)
constexpr double kEventTurn = 0.45;
// The game's own and event cameras: a cut between shots when the eye's step changes by more than this from one tick
// to the next (world units). The new game's opening changes shot by 0.7, 485, then 205 units back; its title flight
// slows by 15 units per tick; the original camera following the player steps at most 28 units.
constexpr double kShotStepChange = 150.0;
// The modern camera pulled in by its collision shows the new distance at once and moves on in direction (15.2),
// unless the direction jumped too (radians in one tick): then the camera was put elsewhere (a pull-in from 880 to
// 211 units that tilted the view 55 degrees down in the fight). Turning is at most 6 degrees a tick with the mouse,
// 22.5 with a flick (M6B smoothing).
constexpr double kPulledTurn = 0.45;

// What the frames between ticks a and b (consecutive snapshots) may do. A cut names its reason (nullptr:
// interpolated); across one the frames show tick b as the game wrote it.
struct Interval {
  const char* camera_cut = "first";
  const char* player_cut = "first";
  bool fov = false;        // the field of view interpolated too (the setting unchanged)
  bool pulled_in = false;  // InterpolateCamera's pulled_in
  double turn = NAN;       // InterpolateCamera's turn
};
Interval Decide(const Snapshot& a, const Snapshot& b);

// ---- world objects (D4a) ----

// Backstops for world objects, measured in the scripted fight (gameplay.combat_loot, about 17,800 ticks, 81
// characters, 59 other objects): characters move at most 101 units in a tick (a knock-back), dropped items 79.
constexpr double kObjectJump = 400.0;  // world units in one tick: a teleport (as kPlayerJump)
constexpr double kObjectTurn = 2.6;    // radians in one tick: turned at once (as kPlayerTurn)
// A matrix the rigid interpolation would change (shear, a reflection): largest element difference after Decompose
// and Compose, relative to its largest row length.
constexpr double kRigidError = 1e-3;

// Whether a matrix is a rotation with per-row scale and a position (Compose(Decompose(m)) gives it back).
bool IsRigid(const Mat4& m);

// Why an object's frames between two consecutive ticks a and b show tick b as written (nullptr: interpolated).
// "object": another object at the index (released, another address or class), "teleport", "turn", "matrix".
const char* ObjectCut(const ObjectTick& a, const ObjectTick& b);

// ---- skeletal poses (D4b) ----

// A skinned model's pose as a tick's scene build evaluated it (docs/HIGH_FPS.md 17): its bone palette in model space
// (each bone's matrix = its local matrix x its parent's, row vectors), the skeleton's hierarchy and the animation
// layers the game had set for it.
struct PoseLayer {
  uint32_t clip = 0;
  float time = 0;    // in frames: one per tick
  float weight = 1;  // blend layers: the weight of the clip being faded out
};
struct PoseTick {
  uint32_t object = 0, skeleton = 0;
  std::vector<int> parents;  // per bone: its parent's index, -1 for a root; empty: not known (model space only)
  std::vector<Mat4> bones;
  std::vector<PoseLayer> base, blend;
};

// Backstop for a bone the interpolation cannot follow: turning by more than this in its parent's space in one tick
// (radians; the way round is not known) it is shown at the new tick's angle, the rest of the pose interpolated.
// Measured in the scripted fight: docs/HIGH_FPS.md 17.
constexpr double kBoneTurn = 2.6;

// Two consecutive ticks' poses decomposed for interpolation: each bone's local transform (or its model-space one when
// the hierarchy is not known or does not decompose).
struct PosePair {
  bool valid = false;
  bool local = false;          // interpolated in each bone's parent space
  std::vector<int> parents;
  std::vector<Rigid> a, b;
  double turn_max = 0;         // the largest bone turn over the tick (radians, in the space interpolated)
  size_t held = 0;             // bones turned by more than kBoneTurn: shown at tick b's (in their parent's space)
  std::vector<bool> hold;
};
PosePair PreparePose(const PoseTick& a, const PoseTick& b);
// The pose between the two ticks (t 0..1; t 0 and 1 give the ticks' palettes themselves).
void InterpolatePose(const PoseTick& a, const PoseTick& b, const PosePair& p, double t, std::vector<Mat4>* out);
// How far a pose is from tick a's to tick b's (0..1), recovered from its palette: per bone that turns, the angle from
// a's rotation to the pose's over the angle from a's to b's, in the space interpolated (the mean over those bones; NaN
// when none turns). Tests: the palettes the game draws are at the frame's alpha.
double PoseProgress(const PosePair& p, const std::vector<Mat4>& pose);
// Why the frames between two consecutive ticks show tick b's pose (nullptr: interpolated): "skeleton" (another skeleton
// or bone count), "snap" (the game switched the animation without a crossfade: the base layers' clips changed and
// neither tick has a blend layer fading one out), "matrix" (a bone that does not decompose).
const char* PoseCut(const PoseTick& a, const PoseTick& b, const PosePair& p);

// A physics-driven part hanging from a skinned model (D4b-3, docs/HIGH_FPS.md 19): its palette is made by the physics
// step each tick in the model's space, without a hierarchy. Each part bone is taken relative to its anchor, the
// bone it moved with most rigidly over the tick (the model's bone it hangs from, or the link before it in a chain);
// that relation is interpolated (rotation the shorter way, the offset from the anchor by direction and distance,
// scale linearly) and placed on the anchor as presented, so the part follows the model's interpolated bones and
// swings between its two ticks' poses. The ends are the ticks' palettes when the model's bones are its ticks'
// palettes. A relation is an anchor only if it stays a rigid transform (a model bone scaled unevenly shears it) and
// changed less over the tick than the part bone did in the model's space; a part bone with none (a body moving on its
// own) is interpolated in the model's space.
struct PartPair {
  bool valid = false;
  std::vector<int> anchor;     // per part bone: the bone it is placed on: the model's (0 .. model_bones - 1) or an
                               // earlier part bone (model_bones + i); -1: none, in the model's space
  std::vector<double> distance_a, distance_b;  // per part bone: its distance from its anchor at each tick (units)
  size_t model_bones = 0;      // the model's bone count
  std::vector<Rigid> a, b;     // per part bone: its transform relative to the anchor at each tick
  std::vector<bool> hold;      // turned by more than kBoneTurn relative to its anchor: shown at tick b's
  size_t held = 0;
  double turn_max = 0;         // the largest turn relative to an anchor (radians, held bones left out)
  size_t unanchored = 0;       // part bones with no anchor
};
PartPair PreparePart(const std::vector<Mat4>& part_a, const std::vector<Mat4>& part_b,
                     const std::vector<Mat4>& model_a, const std::vector<Mat4>& model_b);
// The part's palette between the ticks (t 0..1) placed on `model`, the model's bones as presented in the frame.
void InterpolatePart(const PartPair& p, const std::vector<Mat4>& model, double t, std::vector<Mat4>* out);
// Part bone j's anchor matrix in a frame: the model's bone or the part's own (`part`), nullptr for none.
// The offset between the ticks by direction (the shorter way) and distance (linearly); nearly opposite: linearly.
Vec3 ArcPosition(const Vec3& a, const Vec3& b, double t);
const Mat4* PartAnchor(const PartPair& p, size_t j, const std::vector<Mat4>& model, const std::vector<Mat4>& part);
// How far a part's palette is from tick a's to tick b's relative to its anchors (0..1, the mean over the bones that
// turn; NaN when none turns), as PoseProgress: tests.
double PartProgress(const PartPair& p, const std::vector<Mat4>& model, const std::vector<Mat4>& part);

}  // namespace kuf::presentation
