// Presentation interpolation (M6D D3b-3; presentation.h, docs/HIGH_FPS.md section 15).
//
// v1 addresses and offsets (the D3b-3 investigation: per-tick probes of the view, the objects and their bone palettes,
// access watchpoints on the view's slot matrices under gdb, and the disassembly).

#include "runtime/presentation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>

#include "runtime/frame_pacing.h"
#include "runtime/modern_camera.h"
#include "runtime/pc_settings.h"
#include "runtime/presentation_math.h"

REXCVAR_DEFINE_BOOL(kuf_frame_interpolation, true, "KUF",
                    "M6D: with --kuf_frame_rate, draw the camera, the player character and the world's objects between "
                    "the game's ticks (false: each frame between ticks repeats its tick)");
REXCVAR_DEFINE_BOOL(kuf_frame_interpolation_objects, true, "KUF",
                    "M6D D4a: with --kuf_frame_interpolation, also draw the world's objects (enemies, other characters, "
                    "items) between ticks (false: only the camera and the player)");
REXCVAR_DEFINE_BOOL(kuf_frame_interpolation_bones, true, "KUF",
                    "M6D D4b: with --kuf_frame_interpolation, also draw the characters' skeletal poses between ticks "
                    "(false: their limbs pose once per tick)");
REXCVAR_DEFINE_BOOL(kuf_frame_interpolation_effects, true, "KUF",
                    "M6D D4b-4: with --kuf_frame_interpolation, the effect shaders' clock (mist, fireflies, grass and "
                    "the other effects timed by the frame timer) shows each frame's own moment between ticks (false: "
                    "the tick's, as at 30 fps)");
REXCVAR_DEFINE_INT32(kuf_presentation_test_offset, 0, "KUF/Debug",
                     "M6D isolation test: every presented camera is raised by this many world units (the player and "
                     "every other world object by 3/4 of it) and its field of view widened by 0.1 rad, also at the "
                     "ticks, and every character's bones in render-only frames by 1/4 of it; the game must not notice");
REXCVAR_DEFINE_BOOL(kuf_trace_presentation, false, "KUF/Debug",
                    "M6D: log every presented frame's interpolation: alpha, camera, player, the world objects' counts "
                    "and every object cut (tests)");
REXCVAR_DEFINE_BOOL(kuf_trace_presentation_objects, false, "KUF/Debug",
                    "M6D D4a: with --kuf_trace_presentation, also every world object of every tick as written and of "
                    "every presented frame (large logs; tests)");
REXCVAR_DEFINE_BOOL(kuf_trace_presentation_bones, false, "KUF/Debug",
                    "M6D D4b: with --kuf_trace_presentation, also every pose cut with its layers and every pose written "
                    "into a render-only frame: its alpha, progress between the ticks and bone length error (tests)");
REXCVAR_DEFINE_BOOL(kuf_debug_physics_sees_presented, false, "KUF/Debug",
                    "M6D D4b-3 test, not for play: the physics step's palette readers are not given the game's own "
                    "palettes, so they read the poses written for the frame (the isolation fault the tests must catch)");

REX_EXTERN(sub_8219A9B8);  // a model's palette for the game slot: (mesh) -> mesh + 856 + slot * 4, from frame memory

namespace {

using kuf::presentation::Interval;
using kuf::presentation::Mat4;
using kuf::presentation::ObjectTick;
using kuf::presentation::PartPair;
using kuf::presentation::PosePair;
using kuf::presentation::PoseTick;
using kuf::presentation::Rigid;
using kuf::presentation::Snapshot;
using kuf::presentation::Vec3;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t bits = Load32(base, a);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(base + a, &bits, 4);
}
Mat4 LoadMat(const uint8_t* base, uint32_t a) {
  Mat4 m;
  for (int i = 0; i < 16; ++i) m[i] = LoadF(base, a + 4 * i);
  return m;
}
void StoreMat(uint8_t* base, uint32_t a, const Mat4& m) {
  for (int i = 0; i < 16; ++i) StoreF(base, a + 4 * i, m[i]);
}
bool Plausible(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000; }
bool Finite(const Mat4& m) {
  for (float v : m) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}
double Distance(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }

constexpr uint32_t kGameSlotIndex = 0x82AE760C;  // the slot the game thread builds (both indices in a build)
constexpr uint32_t kRendererObjects = 29272;     // renderer + this: the renderer object array (by object index)
constexpr uint32_t kRendererSlotList = 6782 * 4;  // + slot * 4: the slot's frame packet list (object indices)
constexpr uint32_t kViewIndex = 10, kViewVtable = 0x82102A90;  // the gameplay view (sub_825246D0 hands it the camera)
constexpr uint32_t kSlotMatrix = 28;              // object + this + slot * 64: its world matrix for the slot (vf52)
constexpr uint32_t kViewFov = 191 * 4;            // view + this + slot * 4: its vertical field of view (sub_821EA080)
constexpr uint32_t kCharacterVtable = 0x821010E0;  // a skinned model (its parts, class 0x82103828, follow it)
constexpr uint32_t kMeshBones = 696, kMeshPalette = 856;  // its bone count and model-space bone palette (+ slot * 4)
constexpr uint32_t kPartVtable = 0x82103828;    // a character's part: copies its parent's slot matrix in the scene build
constexpr uint32_t kMarkerVtable = 0x821034C8;  // listed, but no world matrix of its own (no vf52)
constexpr uint32_t kMaxObjectIndex = 0x8000;    // the object array holds 30,000 (renderer + 29268)
// Skinned models (D4b, docs/HIGH_FPS.md 17). The pose is evaluated in the scene build: the culling (sub_821A30D8)
// calls each visible or shadow-casting model's vf96 (sub_8219CCB8), which evaluates it (sub_8219C8A8, from 0x8219CD18)
// into a new palette, or one of the pose cache's shared with other models in the same clip and frame, at mesh + 856 +
// slot * 4, then places the models attached to its bones from that palette. The animation layers say what it played.
constexpr uint32_t kPoseFromVf96 = 0x8219CD18;  // sub_8219C8A8's return address in vf96
constexpr uint32_t kMeshSkeleton = 700;         // the skeleton: + 12 bone count, + 20 bone pointers (bone + 892:
constexpr uint32_t kBoneParent = 892;           // its parent as an index relative to its own, 0 for the root)
constexpr uint32_t kBaseLayers = 708, kBaseClip = 712, kBaseTime = 732;                     // up to 5 each
constexpr uint32_t kBlendLayers = 772, kBlendClip = 776, kBlendTime = 796, kBlendWeight = 816;  // up to 5 each
constexpr uint32_t kMsgBoneByName = 520, kMsgBoneByIndex = 526;  // vf172 messages reading a bone of the palette
// Physics-driven parts (D4b-3, docs/HIGH_FPS.md 19): a part's controller (class 0x821014C4, the physics manager's
// pre-simulate update sub_821C39E8 and post-simulate update sub_821C45C0) holds the part (+ 88, class 0x82103828) and
// the skinned model it hangs from (+ 92). sub_821C39E8 allocates the part's palette for the game slot each tick and
// sub_821C45C0 writes the simulated pose into it, in the model's space and without a hierarchy, after the scene
// build has posed the model; the model lists its parts' object indices at + 1028 (vector: + 4 begin, + 8 end).
constexpr uint32_t kPartControllerVtable = 0x821014C4;
constexpr uint32_t kControllerPart = 88, kControllerModel = 92, kMeshParts = 1028;

bool g_enabled = false, g_objects_enabled = false, g_bones_enabled = false;
bool g_trace = false, g_trace_objects = false, g_trace_bones = false;
std::atomic<bool> g_objects_only{false}, g_bones_only{false}, g_effects_only{false};  // Set...Only (sweeps)
bool g_effects_enabled = false;  // the effect clock at the frame's moment (D4b-4)
int g_test_offset = 0;
Snapshot g_prev, g_cur;  // game thread
Interval g_interval;
uint64_t g_last_view_serial = 0;

// What a slot's matrix held as the game wrote it and what this file stored over it. A slot still holding the stored
// value at its next tick was not written by the game in that tick (no view handed on, the player not moved): the
// game's own value is taken back, so what such a tick shows and hands on is what the game had there.
struct Written {
  uint32_t address = 0;
  Mat4 game{}, stored{};
  float game_fov = 0, stored_fov = 0;
  bool valid = false;
};
std::array<Written, 2> g_camera_written, g_player_written;  // by slot

// World objects (D4a). Object indices are reused: the game releases an object with sub_821A4F90 (on the game thread
// queued until its frame has been drawn, then freed on the render thread) and a creator takes the lowest free index.
// The hook below counts the releases per index, so an object is the same across ticks only with the same index,
// generation, address and class.
std::array<std::atomic<uint32_t>, kMaxObjectIndex> g_generation{};
std::atomic<uint64_t> g_releases{0};
// The player model D3b-3 last found (its own rules; not a world object here while it is the same object).
uint32_t g_player_object = 0, g_player_object_index = 0, g_player_object_generation = 0;
// Per index, the object last listed there (address, generation + 1; 0: never): another object at an index is counted.
std::array<std::pair<uint32_t, uint32_t>, kMaxObjectIndex> g_last_listed{};
// This tick's objects to write: interpolated, or (isolation test) written as the tick with the offset.
struct ObjectPlan {
  uint32_t index = 0, address = 0;  // address: its slot matrix
  bool interpolate = false;
  Mat4 a{}, b{};
  Rigid ra, rb;
};
std::vector<ObjectPlan> g_object_plans;
// What this file stored into a slot's object matrices and what the game had there, by slot: taken back at that slot's
// next tick where the game has not written it since (an object not listed, or listed without a new matrix).
struct ObjectWritten {
  uint32_t index = 0, object = 0, generation = 0;
  Mat4 game{}, stored{};
};
std::array<std::vector<ObjectWritten>, 2> g_objects_written;

// Skeletal poses (D4b): each skinned model's pose as the tick's scene build evaluated it (game thread; the tick's
// scene build is from OnTickFrame to OnTickSceneDone), the plan for the frames between, and the palettes written.
bool g_capturing = false;
std::unordered_map<uint32_t, PoseTick> g_poses_prev, g_poses_cur;  // by model address
std::unordered_map<uint32_t, uint32_t> g_pose_palette;  // the tick's palette pointer, by model
// The model's continuity from its root transform's rules (D4a's objects and D3b-3's player): the cut, nullptr when
// it is the same object in both ticks with no cut; models not listed have none.
std::unordered_map<uint32_t, const char*> g_model_cut;
struct ModelId {
  uint32_t index = 0, generation = 0;
  bool player = false;
};
std::unordered_map<uint32_t, ModelId> g_model_id;
struct PosePlan {
  const PoseTick* a = nullptr;
  const PoseTick* b = nullptr;
  PosePair pair;
  bool interpolate = false;
  ModelId id;
};
std::unordered_map<uint32_t, PosePlan> g_pose_plans;
double g_bone_alpha = 1;  // this frame's (Write)
double g_effect_lag = 0;  // this frame's: how far the effect clock is behind the tick's, in ticks (Write)
// The palettes written into a slot (its frame memory, sub_8219A9B8), by slot: the model's pointer is put back to the
// tick's when the next tick into the slot starts (OnTickStart), before any of the game's code runs in it.
struct PoseWritten {
  uint32_t object = 0, index = 0, generation = 0, tick_palette = 0, palette = 0;
};
std::array<std::vector<PoseWritten>, 2> g_poses_written;
// Physics-driven parts (D4b-3): each part's palette as the tick's physics step left it, by part address, and the plan
// for the frames between (placed on the model's presented bones).
struct PartTick {
  uint32_t model = 0;
  ModelId id;  // the part's object index and its generation (0 / 0: not in its model's list)
  std::vector<Mat4> bones;
};
std::unordered_map<uint32_t, PartTick> g_parts_prev, g_parts_cur;
struct PartPlan {
  uint32_t model = 0;
  bool listed = false;  // in its model's parts list: its palette pointer can be set back (SetPalettes)
  bool interpolate = false;
  PartPair pair;
  const PartTick* b = nullptr;
  ModelId id;
};
std::unordered_map<uint32_t, PartPlan> g_part_plans;
std::array<std::unordered_set<uint32_t>, 2> g_private_palettes;

bool Same(const Mat4& a, const Mat4& b) { return std::memcmp(a.data(), b.data(), sizeof(float) * 16) == 0; }

struct Stats {
  uint64_t frames = 0, tick_frames = 0, build_frames = 0;
  uint64_t camera_interpolated = 0, player_interpolated = 0, held = 0;  // held: a build at alpha 1 (late tick)
  uint64_t alpha_bins[10] = {};
  uint64_t ticks = 0, player_ticks = 0;
  uint64_t restored = 0;  // ticks whose slot still held a presented matrix: the game's own value taken back
  std::map<std::string, uint64_t> camera_cuts, player_cuts;  // per tick interval
  double attach_error_max = 0;
  // World objects: per tick interval (listed, interpolated, unchanged, cut by reason), identity changes the release
  // count does not explain (expected 0), matrices taken back, per frame the objects written and the time it took.
  uint64_t object_ticks = 0, object_interpolated = 0, object_still = 0, object_unexplained = 0;
  uint64_t object_restored = 0, object_writes = 0, object_frames = 0;
  uint64_t object_reused = 0;  // an index listed again with another object (released and created since)
  std::map<std::string, uint64_t> object_cuts;
  double object_step_max = 0, object_turn_max = 0;  // the largest interpolated step (units) and turn (radians)
  double object_write_us = 0, object_write_us_max = 0;
  uint32_t object_listed_max = 0;
  // Skeletal poses: per tick interval the models captured, interpolated (in parent space or, without a usable
  // hierarchy, in model space), cut by reason; per frame the poses written and the time; the palettes put back; the
  // game's own bone reads (vf172 520/526 outside builds) and those that found a written palette (expected 0).
  uint64_t pose_ticks = 0, pose_interpolated = 0, pose_model_space = 0, pose_unlisted = 0, pose_bones_held = 0;
  std::map<std::string, uint64_t> pose_cuts;
  uint64_t pose_writes = 0, pose_frames = 0, pose_bones_written = 0, pose_restored = 0, pose_alloc_failed = 0;
  uint64_t physics_reads = 0, physics_reads_written = 0;  // WithGamePalettes: the game's other palette readers
  uint64_t bone_reads = 0, bone_reads_written = 0;
  // Physics-driven parts (D4b-3): per tick the parts captured, interpolated, cut by reason, bones held; per frame the
  // palettes written, how far each is from its frame's alpha (PartProgress) and how much a bone's distance from its
  // anchor strays from the ticks' (stretching, units).
  uint64_t part_ticks = 0, part_interpolated = 0, part_held = 0, part_unanchored = 0, part_writes = 0;
  std::map<std::string, uint64_t> part_cuts;
  double part_turn_max = 0, part_progress_error_max = 0, part_stretch_max = 0;
  double pose_turn_max = 0, pose_length_error_max = 0;
  double pose_write_us = 0, pose_write_us_max = 0;
  uint32_t pose_models_max = 0, pose_bones_max = 0;
};
std::mutex g_stats_mutex;
Stats g_stats;

Snapshot Capture(const uint8_t* base, uint32_t renderer, uint64_t tick, bool world) {
  Snapshot s;
  s.valid = true;
  s.tick = tick;
  s.world = world;
  s.renderer = renderer;
  s.slot = Load32(base, kGameSlotIndex) & 1;
  const uint32_t objects = Plausible(renderer) ? Load32(base, renderer + kRendererObjects) : 0;
  if (!Plausible(objects)) return s;
  const uint32_t view = Load32(base, objects + kViewIndex * 4);
  if (Plausible(view) && Load32(base, view) == kViewVtable) {
    s.view = view;
    s.camera = LoadMat(base, view + kSlotMatrix + s.slot * 64);
    s.fov = LoadF(base, view + kViewFov + s.slot * 4);
    Written& w = g_camera_written[s.slot];
    if (w.valid && w.address == view && Same(s.camera, w.stored) && s.fov == w.stored_fov) {
      s.camera = w.game;
      s.fov = w.game_fov;
      s.restored = true;
    }
    w = {view, s.camera, s.camera, s.fov, s.fov, true};
    s.have_camera = Finite(s.camera) && std::isfinite(s.fov) && s.fov > 0;
  }
  s.view_info = kuf::camera::LatestTickView();
  s.fresh = s.view_info.serial != g_last_view_serial;
  g_last_view_serial = s.view_info.serial;
  s.fov_setting = kuf::pc::Current().camera.field_of_view;
  // The player character: the skinned model standing at the gameplay camera's attach point (the camera follows it).
  if (!s.fresh || s.view_info.event) return s;
  const Vec3 feet{s.view_info.attach.x, s.view_info.attach.y - s.view_info.target_height, s.view_info.attach.z};
  const uint32_t list = Load32(base, renderer + kRendererSlotList + s.slot * 4);
  if (!Plausible(list)) return s;
  const uint32_t b = Load32(base, list + 4), e = Load32(base, list + 8);
  // The previous tick's player is kept while it is still near the attach point (kAttachKeep; another character can
  // come closer to it in a fight); a new one must stand at it (kAttachMatch).
  const uint32_t previous = g_cur.player;
  double best = 1e30, nearest_unskinned = 1e30;
  bool kept = false;
  for (uint32_t a = b; Plausible(b) && a < e && a < b + 0x4000; a += 4) {
    const uint32_t index = Load32(base, a);
    const uint32_t object = index < 0x1000 ? Load32(base, objects + index * 4) : 0;
    if (!Plausible(object) || Load32(base, object) != kCharacterVtable) continue;
    const Mat4 m = LoadMat(base, object + kSlotMatrix + s.slot * 64);
    if (!Finite(m)) continue;
    const double d = Distance(kuf::presentation::Position(m), feet);
    const uint32_t bones = Load32(base, object + kMeshBones);
    if (bones == 0 || bones > 512 || !Plausible(Load32(base, object + kMeshPalette + s.slot * 4))) {
      nearest_unskinned = std::min(nearest_unskinned, d);
      continue;
    }
    const bool is_previous = object == previous && d <= kuf::presentation::kAttachKeep;
    if (kept && !is_previous) continue;
    if (is_previous || d < best) {
      kept = is_previous;
      best = d;
      s.player = object;
      s.player_index = index;
      s.player_matrix = m;
    }
  }
  s.attach_error = best;
  s.unskinned_error = nearest_unskinned;
  if (!kept && best > kuf::presentation::kAttachMatch) s.player = 0;
  if (s.player) {
    Written& w = g_player_written[s.slot];
    if (w.valid && w.address == s.player && Same(s.player_matrix, w.stored)) {
      s.player_matrix = w.game;
      s.restored = true;
    }
    w = {s.player, s.player_matrix, s.player_matrix, 0, 0, true};
  }
  return s;
}

// Before the tick's capture: object matrices this file stored into the slot two ticks ago that the game has not written
// since get the game's own value back (only the same object: index, address and generation unchanged).
void RestoreObjects(uint8_t* base, uint32_t renderer, uint32_t slot) {
  std::vector<ObjectWritten>& written = g_objects_written[slot];
  const uint32_t objects = Plausible(renderer) ? Load32(base, renderer + kRendererObjects) : 0;
  uint64_t restored = 0;
  for (const ObjectWritten& w : written) {
    if (!Plausible(objects) || Load32(base, objects + w.index * 4) != w.object ||
        g_generation[w.index].load(std::memory_order_acquire) != w.generation) {
      continue;
    }
    const uint32_t address = w.object + kSlotMatrix + slot * 64;
    if (Same(LoadMat(base, address), w.stored)) {
      StoreMat(base, address, w.game);
      ++restored;
    }
  }
  written.clear();
  if (restored) {
    std::lock_guard lock(g_stats_mutex);
    g_stats.object_restored += restored;
  }
}

// The tick's other world objects in its draw list, by index: everything listed with a world matrix of its own except
// the view, characters' parts (they follow their parent) and the player model (D3b-3's rules).
void CaptureObjects(const uint8_t* base, Snapshot& s) {
  if (s.player) {
    g_player_object = s.player;
    g_player_object_index = s.player_index;
    g_player_object_generation = g_generation[s.player_index % kMaxObjectIndex].load(std::memory_order_acquire);
  }
  const uint32_t objects = Plausible(s.renderer) ? Load32(base, s.renderer + kRendererObjects) : 0;
  const uint32_t list = Plausible(s.renderer) ? Load32(base, s.renderer + kRendererSlotList + s.slot * 4) : 0;
  if (!Plausible(objects) || !Plausible(list)) return;
  const uint32_t b = Load32(base, list + 4), e = Load32(base, list + 8);
  for (uint32_t a = b; Plausible(b) && a < e && a < b + 0x4000; a += 4) {
    const uint32_t index = Load32(base, a);
    if (index == kViewIndex || index >= kMaxObjectIndex) continue;
    const uint32_t object = Load32(base, objects + index * 4);
    if (!Plausible(object)) continue;
    const uint32_t vtable = Load32(base, object);
    if (vtable == kViewVtable || vtable == kPartVtable || vtable == kMarkerVtable) continue;
    const uint32_t generation = g_generation[index].load(std::memory_order_acquire);
    if (object == g_player_object && index == g_player_object_index && generation == g_player_object_generation) {
      continue;
    }
    s.objects.push_back({index, object, vtable, generation, LoadMat(base, object + kSlotMatrix + s.slot * 64)});
    auto& last = g_last_listed[index];
    if (last.second != 0 && (last.first != object || last.second != generation + 1)) {
      if (g_trace) {
        REXLOG_INFO("[kuf-present] tick {} object {}: another object (address {:08X} -> {:08X}, generation {} -> {})",
                    s.tick, index, last.first, object, last.second - 1, generation);
      }
      std::lock_guard lock(g_stats_mutex);
      ++g_stats.object_reused;
    }
    last = {object, generation + 1};
  }
  std::sort(s.objects.begin(), s.objects.end(), [](const ObjectTick& x, const ObjectTick& y) { return x.index < y.index; });
  s.objects.erase(std::unique(s.objects.begin(), s.objects.end(),
                              [](const ObjectTick& x, const ObjectTick& y) { return x.index == y.index; }),
                  s.objects.end());
}

std::string Fmt(const Vec3& v);

// Each object of tick B against tick A: interpolated (both ticks consecutive world frames, the same object, no cut
// rule), unchanged (not written), or cut (shown as the tick wrote it).
void DecideObjects() {
  g_object_plans.clear();
  g_model_cut.clear();
  g_model_id.clear();
  const Snapshot& a = g_prev;
  const Snapshot& b = g_cur;
  const char* all = !a.valid                       ? "first"
                    : b.tick != a.tick + 1         ? "gap"
                    : !a.world || !b.world         ? "not world"
                    : a.renderer != b.renderer     ? "view"
                                                   : nullptr;
  std::map<std::string, uint64_t> cuts;
  uint64_t interpolated = 0, still = 0, unexplained = 0;
  double step_max = 0, turn_max = 0;
  auto ia = a.objects.begin();
  for (const ObjectTick& o : b.objects) {
    while (ia != a.objects.end() && ia->index < o.index) ++ia;
    const ObjectTick* before = ia != a.objects.end() && ia->index == o.index ? &*ia : nullptr;
    const char* cut = all ? all : !before ? "new" : kuf::presentation::ObjectCut(*before, o);
    if (o.vtable == kCharacterVtable) {
      g_model_cut[o.object] = cut;
      g_model_id[o.object] = {o.index, o.generation, false};
    }
    if (before && before->generation == o.generation && (before->object != o.object || before->vtable != o.vtable)) {
      ++unexplained;  // another object at the index without a release seen: still a cut ("object")
      REXLOG_WARN("[kuf-present] tick {} object {}: {:08X} class {:08X} became {:08X} class {:08X} without a release",
                  b.tick, o.index, before->object, before->vtable, o.object, o.vtable);
    }
    ObjectPlan plan;
    plan.index = o.index;
    plan.address = o.object + kSlotMatrix + b.slot * 64;
    plan.b = o.matrix;
    if (cut) {
      ++cuts[cut];
      if (g_trace && std::strcmp(cut, "new") != 0 && !all) {
        REXLOG_INFO("[kuf-present] tick {} object {} class {:08X} cut {}: step {:.2f}", b.tick, o.index, o.vtable, cut,
                    Distance(kuf::presentation::Position(before->matrix), kuf::presentation::Position(o.matrix)));
      }
      if (g_test_offset == 0) continue;
    } else if (Same(before->matrix, o.matrix)) {
      ++still;
      if (g_test_offset == 0) continue;
    } else {
      ++interpolated;
      plan.interpolate = true;
      plan.a = before->matrix;
      plan.ra = kuf::presentation::Decompose(before->matrix);
      plan.rb = kuf::presentation::Decompose(o.matrix);
      step_max = std::max(step_max, Distance(plan.ra.position, plan.rb.position));
      turn_max = std::max(turn_max, kuf::presentation::AngleBetween(plan.ra.rotation, plan.rb.rotation));
    }
    g_object_plans.push_back(plan);
  }
  std::vector<ObjectWritten>& written = g_objects_written[b.slot];
  written.clear();
  for (const ObjectPlan& p : g_object_plans) {
    const auto& o = *std::lower_bound(b.objects.begin(), b.objects.end(), p.index,
                                      [](const ObjectTick& x, uint32_t i) { return x.index < i; });
    written.push_back({p.index, o.object, o.generation, p.b, p.b});
  }
  if (g_trace) {
    std::string c;
    for (const auto& [k, v] : cuts) c += fmt::format("{}{} {}", c.empty() ? "" : ", ", k, v);
    REXLOG_INFO("[kuf-present] tick {} objects: listed {}, interpolated {}, unchanged {}, cut {}{}{}", b.tick,
                b.objects.size(), interpolated, still, c.empty() ? "none" : c, all ? " (all: " : "",
                all ? std::string(all) + ")" : "");
    if (g_trace_objects) {
      std::string line;
      for (const ObjectTick& o : b.objects) {
        const Vec3 f = kuf::presentation::Forward(o.matrix);
        line += fmt::format(" {}:{}:{:.4f}:{:.4f}", o.index, Fmt(kuf::presentation::Position(o.matrix)),
                            kuf::presentation::Azimuth(f), kuf::presentation::Elevation(f));
      }
      REXLOG_INFO("[kuf-present] tick {} objects as written:{}", b.tick, line);
    }
  }
  std::lock_guard lock(g_stats_mutex);
  g_stats.object_ticks += b.objects.size();
  g_stats.object_interpolated += interpolated;
  g_stats.object_still += still;
  g_stats.object_unexplained += unexplained;
  for (const auto& [k, v] : cuts) g_stats.object_cuts[k] += v;
  g_stats.object_step_max = std::max(g_stats.object_step_max, step_max);
  g_stats.object_turn_max = std::max(g_stats.object_turn_max, turn_max);
  g_stats.object_listed_max = std::max<uint32_t>(g_stats.object_listed_max, uint32_t(b.objects.size()));
}

// The frame's world objects at alpha into the slot (interpolated objects; with the isolation test every object).
void WriteObjects(uint8_t* base, double alpha, uint32_t slot) {
  const auto began = std::chrono::steady_clock::now();
  std::vector<ObjectWritten>& written = g_objects_written[slot];
  const float offset = float(g_test_offset) * 0.75f;
  std::string line;
  for (size_t k = 0; k < g_object_plans.size(); ++k) {
    const ObjectPlan& p = g_object_plans[k];
    Mat4 m = !p.interpolate || alpha >= 1 ? p.b
             : alpha <= 0                 ? p.a
                                          : kuf::presentation::InterpolateRigid(p.ra, p.rb, alpha);
    m[13] += offset;
    StoreMat(base, p.address, m);
    written[k].stored = m;
    if (g_trace_objects && p.interpolate) {
      const Vec3 f = kuf::presentation::Forward(m);
      line += fmt::format(" {}:{}:{:.4f}:{:.4f}", p.index, Fmt(kuf::presentation::Position(m)),
                          kuf::presentation::Azimuth(f), kuf::presentation::Elevation(f));
    }
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
  if (g_trace_objects) REXLOG_INFO("[kuf-present] frame tick {} alpha {:.4f} objects:{}", g_cur.tick, alpha, line);
  std::lock_guard lock(g_stats_mutex);
  g_stats.object_writes += g_object_plans.size();
  ++g_stats.object_frames;
  g_stats.object_write_us += us;
  g_stats.object_write_us_max = std::max(g_stats.object_write_us_max, us);
}

// ---- skeletal poses (D4b) ----

std::string Layers(const PoseTick& t) {
  std::string s;
  for (const auto& l : t.base) s += fmt::format(" base {}@{:g}", l.clip, l.time);
  for (const auto& l : t.blend) s += fmt::format(" blend {}@{:g}x{:.3f}", l.clip, l.time, l.weight);
  return s.empty() ? " none" : s;
}

// The model's pose as the tick's scene build just evaluated it (its palette for the game slot).
const PoseTick* CapturePose(const uint8_t* base, uint32_t mesh) {
  const uint32_t slot = Load32(base, kGameSlotIndex) & 1;
  const uint32_t palette = Load32(base, mesh + kMeshPalette + slot * 4);
  const uint32_t n = Load32(base, mesh + kMeshBones);
  if (!Plausible(palette) || n == 0 || n > 512) return nullptr;
  PoseTick t;
  t.object = mesh;
  t.skeleton = Load32(base, mesh + kMeshSkeleton);
  if (Plausible(t.skeleton) && Load32(base, t.skeleton + 12) == n) {
    const uint32_t bones = Load32(base, t.skeleton + 20);
    for (uint32_t i = 0; Plausible(bones) && i < n; ++i) {
      const uint32_t bone = Load32(base, bones + i * 4);
      if (!Plausible(bone)) {
        t.parents.clear();
        break;
      }
      const int rel = int32_t(Load32(base, bone + kBoneParent));
      t.parents.push_back(rel == 0 ? -1 : int(i) + rel);
    }
  }
  t.bones.resize(n);
  for (uint32_t i = 0; i < n; ++i) t.bones[i] = LoadMat(base, palette + i * 64);
  for (uint32_t i = 0, k = std::min(Load32(base, mesh + kBaseLayers), 5u); i < k; ++i) {
    t.base.push_back({Load32(base, mesh + kBaseClip + i * 4), LoadF(base, mesh + kBaseTime + i * 4), 1});
  }
  for (uint32_t i = 0, k = std::min(Load32(base, mesh + kBlendLayers), 5u); i < k; ++i) {
    t.blend.push_back({Load32(base, mesh + kBlendClip + i * 4), LoadF(base, mesh + kBlendTime + i * 4),
                       LoadF(base, mesh + kBlendWeight + i * 4)});
  }
  g_pose_palette[mesh] = palette;
  PoseTick& stored = g_poses_cur[mesh] = std::move(t);
  return &stored;
}

// The frames between the model's poses at the last two ticks: interpolated when its root transform's rules found it
// the same object without a cut and PoseCut finds no reason; else they show this tick's pose.
void DecidePose(uint32_t mesh, const PoseTick* b) {
  PosePlan plan;
  plan.b = b;
  const char* cut = nullptr;
  const auto c = g_model_cut.find(mesh);
  const auto prev = g_poses_prev.find(mesh);
  if (c == g_model_cut.end()) {
    cut = "unlisted";
  } else if (c->second) {
    cut = c->second;
  } else if (prev == g_poses_prev.end()) {
    cut = "new";
  } else {
    plan.a = &prev->second;
    plan.pair = kuf::presentation::PreparePose(*plan.a, *b);
    cut = kuf::presentation::PoseCut(*plan.a, *b, plan.pair);
  }
  if (const auto id = g_model_id.find(mesh); id != g_model_id.end()) plan.id = id->second;
  plan.interpolate = cut == nullptr;
  if (g_trace_bones) {
    // Every cut, and every pose change with a change of layers or a large turn: how much the pose moved.
    const PoseTick* a = prev != g_poses_prev.end() ? &prev->second : nullptr;
    bool layers = false;
    double moved = 0;
    if (a) {
      layers = a->base.size() != b->base.size() || a->blend.size() != b->blend.size();
      for (size_t i = 0; !layers && i < b->base.size(); ++i) layers = a->base[i].clip != b->base[i].clip;
      for (size_t i = 0; i < b->bones.size() && i < a->bones.size(); ++i) {
        moved = std::max(moved, Distance(kuf::presentation::Position(a->bones[i]),
                                         kuf::presentation::Position(b->bones[i])));
      }
    }
    if (cut || layers || plan.pair.turn_max > 0.5) {
      REXLOG_INFO("[kuf-present] tick {} pose {:08X} {} turn {:.4f} bones over {} moved {:.2f}: layers{} ->{}",
                  g_cur.tick, mesh, cut ? fmt::format("cut {}", cut) : std::string("interpolated"), plan.pair.turn_max,
                  plan.pair.held, moved, a ? Layers(*a) : std::string(" (none)"), Layers(*b));
    }
  }
  g_pose_plans[mesh] = std::move(plan);
}

// A palette of the model's own for this frame (the game's allocator: the slot's frame memory, released with the
// frame) holding the pose at the frame's alpha; the model's palette pointer for the slot points to it until the next
// tick into the slot starts.
void WritePose(PPCContext& ctx, uint8_t* base, uint32_t mesh, const PosePlan& plan) {
  const double alpha = plan.interpolate ? g_bone_alpha : 1.0;
  if (alpha >= 1 && g_test_offset == 0) return;
  const auto began = std::chrono::steady_clock::now();
  std::vector<Mat4> pose;
  if (plan.interpolate) {
    kuf::presentation::InterpolatePose(*plan.a, *plan.b, plan.pair, alpha, &pose);
  } else {
    pose = plan.b->bones;
  }
  if (g_test_offset != 0) {
    for (Mat4& m : pose) m[13] += float(g_test_offset) * 0.25f;
  }
  const uint32_t slot = Load32(base, kGameSlotIndex) & 1;
  const uint32_t tick_palette = g_pose_palette.count(mesh) ? g_pose_palette[mesh] : 0;
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
  ctx.r3.u64 = mesh;
  sub_8219A9B8(ctx, base);  // the slot's palette for the model: bone count x 64 bytes of frame memory
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  const uint32_t palette = Load32(base, mesh + kMeshPalette + slot * 4);
  if (!Plausible(palette) || palette == tick_palette) {
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.pose_alloc_failed;
    return;
  }
  for (size_t i = 0; i < pose.size(); ++i) StoreMat(base, palette + uint32_t(i) * 64, pose[i]);
  g_poses_written[slot].push_back({mesh, plan.id.index, plan.id.generation, tick_palette, palette});
  {
    std::lock_guard lock(g_stats_mutex);
    g_private_palettes[slot].insert(palette);
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
  double length_error = 0, progress = NAN;
  int moved = 0;
  if (g_trace_bones && plan.interpolate && plan.a) {
    const auto& b = plan.b->bones;
    progress = kuf::presentation::PoseProgress(plan.pair, pose);
    for (size_t i = 0; i < pose.size(); ++i) {
      const Vec3 x = kuf::presentation::Position(pose[i]), xb = kuf::presentation::Position(b[i]);
      if (kuf::presentation::AngleBetween(plan.pair.a[i].rotation, plan.pair.b[i].rotation) >= 1e-3) ++moved;
      // A bone whose offset from its parent the animation keeps (in the parent's space, both ticks) keeps its length.
      const int parent = plan.pair.local ? plan.pair.parents[i] : -1;
      if (parent >= 0 && Distance(plan.pair.a[i].position, plan.pair.b[i].position) < 1e-4) {
        const double lb = Distance(xb, kuf::presentation::Position(b[parent]));
        const double l = Distance(x, kuf::presentation::Position(pose[parent]));
        length_error = std::max(length_error, std::abs(l - lb));
      }
    }
    if (!std::isfinite(progress)) progress = alpha;  // no bone turns
  }
  if (g_trace_bones && plan.interpolate) {
    REXLOG_INFO("[kuf-present] pose tick {} alpha {:.4f} model {:08X} bones {} {} progress {:.4f} turning {} length "
                "error {:.5f}", g_cur.tick, alpha, mesh, pose.size(), plan.pair.local ? "local" : "model", progress, moved,
                length_error);
  }
  std::lock_guard lock(g_stats_mutex);
  ++g_stats.pose_writes;
  g_stats.pose_bones_written += pose.size();
  g_stats.pose_write_us += us;
  g_stats.pose_write_us_max = std::max(g_stats.pose_write_us_max, us);
  g_stats.pose_length_error_max = std::max(g_stats.pose_length_error_max, length_error);
}

// The model's palette for the game slot as the frame draws it (the one written for the frame, else the tick's).
bool PresentedBones(const uint8_t* base, uint32_t model, size_t n, std::vector<Mat4>* out) {
  const uint32_t palette = Load32(base, model + kMeshPalette + (Load32(base, kGameSlotIndex) & 1) * 4);
  if (!Plausible(palette) || Load32(base, model + kMeshBones) != n) return false;
  out->resize(n);
  for (size_t i = 0; i < n; ++i) (*out)[i] = LoadMat(base, palette + uint32_t(i) * 64);
  return true;
}

// The part's palette for this frame (as WritePose): placed on the model's bones as the frame draws them.
void WritePart(PPCContext& ctx, uint8_t* base, uint32_t part, const PartPlan& plan) {
  const double alpha = plan.interpolate ? g_bone_alpha : 1.0;
  if (alpha >= 1 && g_test_offset == 0) return;
  std::vector<Mat4> pose, model;
  if (plan.interpolate && PresentedBones(base, plan.model, plan.pair.model_bones, &model)) {
    kuf::presentation::InterpolatePart(plan.pair, model, alpha, &pose);
  } else {
    pose = plan.b->bones;
    model.clear();
  }
  if (g_test_offset != 0) {
    for (Mat4& m : pose) m[13] += float(g_test_offset) * 0.25f;
  }
  const uint32_t slot = Load32(base, kGameSlotIndex) & 1;
  const uint32_t tick_palette = g_pose_palette.count(part) ? g_pose_palette[part] : 0;
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
  ctx.r3.u64 = part;
  sub_8219A9B8(ctx, base);  // the slot's palette for the part (its frame memory)
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  const uint32_t palette = Load32(base, part + kMeshPalette + slot * 4);
  if (!Plausible(palette) || palette == tick_palette) {
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.pose_alloc_failed;
    return;
  }
  for (size_t i = 0; i < pose.size(); ++i) StoreMat(base, palette + uint32_t(i) * 64, pose[i]);
  g_poses_written[slot].push_back({part, plan.id.index, plan.id.generation, tick_palette, palette});
  double progress_error = 0, stretch = 0;
  if (!model.empty() && g_test_offset == 0) {
    const double progress = kuf::presentation::PartProgress(plan.pair, model, pose);
    if (std::isfinite(progress)) progress_error = std::abs(progress - alpha);
    // Each bone's distance from its anchor between its distances at the two ticks (no stretching).
    for (size_t j = 0; j < pose.size(); ++j) {
      const Mat4* anchor = kuf::presentation::PartAnchor(plan.pair, j, model, pose);
      if (!anchor) continue;
      const double da = plan.pair.distance_a[j], db = plan.pair.distance_b[j];
      const double d = Distance(kuf::presentation::Position(pose[j]), kuf::presentation::Position(*anchor));
      stretch = std::max(stretch, std::max(0.0, std::max(std::min(da, db) - d, d - std::max(da, db))));
    }
  }
  if (g_trace_bones) {
    REXLOG_INFO("[kuf-present] part tick {} alpha {:.4f} part {:08X} model {:08X} bones {} {} progress error {:.5f} "
                "stretch {:.5f}{}", g_cur.tick, alpha, part, plan.model, pose.size(),
                plan.interpolate ? "interpolated" : "tick", progress_error, stretch,
                plan.model == g_cur.player ? " (the player's model)" : "");
  }
  std::lock_guard lock(g_stats_mutex);
  g_private_palettes[slot].insert(palette);
  ++g_stats.part_writes;
  g_stats.part_progress_error_max = std::max(g_stats.part_progress_error_max, progress_error);
  g_stats.part_stretch_max = std::max(g_stats.part_stretch_max, stretch);
}

// The part's object index (the model's parts list): what SetPalettes checks before setting its palette back.
bool PartId(const uint8_t* base, uint32_t model, uint32_t part, ModelId* id) {
  const uint32_t objects = Plausible(g_cur.renderer) ? Load32(base, g_cur.renderer + kRendererObjects) : 0;
  const uint32_t begin = Load32(base, model + kMeshParts + 4), end = Load32(base, model + kMeshParts + 8);
  for (uint32_t e = begin; Plausible(objects) && Plausible(begin) && e < end && e < begin + 4 * 64; e += 4) {
    const uint32_t index = Load32(base, e);
    if (index < kMaxObjectIndex && Load32(base, objects + index * 4) == part) {
      *id = {index, g_generation[index].load(std::memory_order_acquire), false};
      return true;
    }
  }
  return false;
}

// After the physics step's post-simulate update of a part (the tick, its scene build done posing the models): its
// palette captured, the interval's plan decided, the tick's own frame written. Cut: a new part or another model, the
// model not interpolated over the interval (its own cut), a part not in its model's list, a bone that does not
// decompose ("matrix").
void CapturePart(PPCContext& ctx, uint8_t* base, uint32_t controller) {
  if (!Plausible(controller) || Load32(base, controller) != kPartControllerVtable) return;
  const uint32_t part = Load32(base, controller + kControllerPart);
  const uint32_t model = Load32(base, controller + kControllerModel);
  if (!Plausible(part) || !Plausible(model) || Load32(base, part) != kPartVtable ||
      Load32(base, model) != kCharacterVtable) {
    return;
  }
  const uint32_t slot = Load32(base, kGameSlotIndex) & 1;
  const uint32_t palette = Load32(base, part + kMeshPalette + slot * 4);
  const uint32_t n = Load32(base, part + kMeshBones);
  if (!Plausible(palette) || n == 0 || n > 512) return;
  PartTick t;
  t.model = model;
  const bool listed = PartId(base, model, part, &t.id);
  t.bones.resize(n);
  for (uint32_t i = 0; i < n; ++i) t.bones[i] = LoadMat(base, palette + i * 64);
  g_pose_palette[part] = palette;
  const PartTick& b = g_parts_cur[part] = std::move(t);
  PartPlan plan;
  plan.model = model;
  plan.b = &b;
  plan.id = b.id;
  plan.listed = listed;
  const char* cut = nullptr;
  const auto prev = g_parts_prev.find(part);
  const auto model_plan = g_pose_plans.find(model);
  if (!listed) {
    cut = "unlisted";
  } else if (prev == g_parts_prev.end() || prev->second.model != model || prev->second.id.index != b.id.index ||
             prev->second.id.generation != b.id.generation) {
    cut = "new";  // or another part at the same address (released and created since), or on another model
  } else if (model_plan == g_pose_plans.end() || !model_plan->second.interpolate || !model_plan->second.a) {
    cut = "model";
  } else {
    plan.pair = kuf::presentation::PreparePart(prev->second.bones, b.bones, model_plan->second.a->bones,
                                               model_plan->second.b->bones);
    if (!plan.pair.valid) cut = "matrix";
  }
  plan.interpolate = cut == nullptr;
  if (g_trace_bones && cut) REXLOG_INFO("[kuf-present] tick {} part {:08X} cut {}", g_cur.tick, part, cut);
  {
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.part_ticks;
    if (cut) {
      ++g_stats.part_cuts[cut];
    } else {
      ++g_stats.part_interpolated;
      g_stats.part_held += plan.pair.held;
      g_stats.part_unanchored += plan.pair.unanchored;
      g_stats.part_turn_max = std::max(g_stats.part_turn_max, plan.pair.turn_max);
    }
  }
  PartPlan& stored = g_part_plans[part] = std::move(plan);
  if (stored.listed && (stored.interpolate || g_test_offset != 0)) WritePart(ctx, base, part, stored);  // tick frame
}

// The tick's poses: counted when its scene build is done.
void SummarisePoses() {
  std::map<std::string, uint64_t> cuts;
  uint64_t interpolated = 0, model_space = 0, unlisted = 0, held = 0;
  double turn_max = 0;
  uint32_t bones_max = 0;
  for (const auto& [mesh, plan] : g_pose_plans) {
    bones_max = std::max<uint32_t>(bones_max, uint32_t(plan.b->bones.size()));
    if (plan.interpolate) {
      ++interpolated;
      if (!plan.pair.local) ++model_space;
      held += plan.pair.held;
      turn_max = std::max(turn_max, plan.pair.turn_max);
      continue;
    }
    const auto c = g_model_cut.find(mesh);
    const char* cut = c == g_model_cut.end() ? "unlisted" : c->second ? c->second : nullptr;
    if (!cut) cut = !plan.a ? "new" : kuf::presentation::PoseCut(*plan.a, *plan.b, plan.pair);
    if (std::strcmp(cut, "unlisted") == 0) ++unlisted;
    ++cuts[cut];
  }
  if (g_trace) {
    std::string c;
    for (const auto& [k, v] : cuts) c += fmt::format("{}{} {}", c.empty() ? "" : ", ", k, v);
    REXLOG_INFO("[kuf-present] tick {} poses: models {}, interpolated {} ({} in model space), cut {}", g_cur.tick,
                g_pose_plans.size(), interpolated, model_space, c.empty() ? "none" : c);
  }
  std::lock_guard lock(g_stats_mutex);
  g_stats.pose_ticks += g_pose_plans.size();
  g_stats.pose_interpolated += interpolated;
  g_stats.pose_model_space += model_space;
  g_stats.pose_unlisted += unlisted;
  g_stats.pose_bones_held += held;
  for (const auto& [k, v] : cuts) g_stats.pose_cuts[k] += v;
  g_stats.pose_turn_max = std::max(g_stats.pose_turn_max, turn_max);
  g_stats.pose_models_max = std::max<uint32_t>(g_stats.pose_models_max, uint32_t(g_pose_plans.size()));
  g_stats.pose_bones_max = std::max(g_stats.pose_bones_max, bones_max);
}

void DecideInterval() {
  const Interval i = kuf::presentation::Decide(g_prev, g_cur);
  g_interval = i;
  std::lock_guard lock(g_stats_mutex);
  ++g_stats.ticks;
  if (g_cur.restored) ++g_stats.restored;
  if (g_cur.player) {
    ++g_stats.player_ticks;
    g_stats.attach_error_max = std::max(g_stats.attach_error_max, g_cur.attach_error);
  }
  if (i.camera_cut) ++g_stats.camera_cuts[i.camera_cut];
  if (i.player_cut) ++g_stats.player_cuts[i.player_cut];
}

std::string Fmt(const Vec3& v) { return fmt::format("({:.2f},{:.2f},{:.2f})", v.x, v.y, v.z); }

// Writes the frame's presentation; returns the game time it shows (ticks).
double Write(uint8_t* base, double frame_alpha, bool tick_frame) {
  // The sweep's parts (tests): the world objects alone, or the skeletal poses alone, between the ticks.
  const bool objects_only = g_objects_only.load(std::memory_order_relaxed);
  const bool bones_only = g_bones_only.load(std::memory_order_relaxed);
  const bool effects_only = g_effects_only.load(std::memory_order_relaxed);
  const double alpha = objects_only || bones_only || effects_only ? 1.0 : frame_alpha;  // camera and player
  g_bone_alpha = objects_only || effects_only ? 1.0 : frame_alpha;
  g_effect_lag = g_effects_enabled && !objects_only && !bones_only ? std::clamp(1.0 - frame_alpha, 0.0, 1.0) : 0.0;
  const Snapshot& a = g_prev;
  const Snapshot& b = g_cur;
  const Interval& i = g_interval;
  const bool camera = !i.camera_cut && alpha < 1;
  const bool player = !i.player_cut && alpha < 1;
  Mat4 view = b.camera, model = b.player_matrix;
  float fov = b.fov;
  const float offset = float(g_test_offset);
  if (b.have_camera) {
    if (camera) {
      kuf::presentation::CameraTick ta, tb;
      ta.matrix = a.camera;
      tb.matrix = b.camera;
      ta.have_pivot = a.view_info.modern;
      tb.have_pivot = b.view_info.modern;
      ta.pivot = a.view_info.pivot;
      tb.pivot = b.view_info.pivot;
      view = kuf::presentation::InterpolateCamera(ta, tb, alpha, i.turn, i.pulled_in);
      if (i.fov) fov = float(a.fov + (b.fov - a.fov) * alpha);
    }
    if (offset != 0) {
      view[13] += offset;
      fov += 0.1f;
    }
    StoreMat(base, b.view + kSlotMatrix + b.slot * 64, view);
    StoreF(base, b.view + kViewFov + b.slot * 4, fov);
    g_camera_written[b.slot].stored = view;
    g_camera_written[b.slot].stored_fov = fov;
  }
  if (b.player) {
    if (player) model = kuf::presentation::InterpolateRigid(a.player_matrix, b.player_matrix, alpha);
    model[13] += offset * 0.75f;
    StoreMat(base, b.player + kSlotMatrix + b.slot * 64, model);
    g_player_written[b.slot].stored = model;
  }
  WriteObjects(base, bones_only || effects_only ? 1.0 : frame_alpha, b.slot);
  if (g_bones_enabled) {
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.pose_frames;
  }
  {
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.frames;
    ++(tick_frame ? g_stats.tick_frames : g_stats.build_frames);
    if (camera) ++g_stats.camera_interpolated;
    if (player) ++g_stats.player_interpolated;
    if (!tick_frame && alpha >= 1) ++g_stats.held;
    ++g_stats.alpha_bins[std::clamp(int(alpha * 10), 0, 9)];
  }
  const double shown = camera || player ? double(b.tick) - 1 + alpha : double(b.tick);
  if (!g_trace) return shown;
  const Vec3 eye = kuf::presentation::Position(view), forward = kuf::presentation::Forward(view);
  std::string cam = fmt::format("camera {} eye {} az {:.4f} el {:.4f} fov {:.4f}",
                                camera ? "interpolated" : i.camera_cut ? i.camera_cut : "tick", Fmt(eye),
                                kuf::presentation::Azimuth(forward), kuf::presentation::Elevation(forward), fov);
  if (b.view_info.modern) {
    const Vec3 p = camera && a.view_info.modern
                       ? Vec3{a.view_info.pivot.x + (b.view_info.pivot.x - a.view_info.pivot.x) * alpha,
                              a.view_info.pivot.y + (b.view_info.pivot.y - a.view_info.pivot.y) * alpha,
                              a.view_info.pivot.z + (b.view_info.pivot.z - a.view_info.pivot.z) * alpha}
                       : b.view_info.pivot;
    const Vec3 d{p.x - eye.x, p.y - eye.y, p.z - eye.z};
    const double c = (d.x * forward.x + d.y * forward.y + d.z * forward.z) /
                     std::max(1e-12, std::hypot(d.x, d.y, d.z) * std::hypot(forward.x, forward.y, forward.z));
    cam += fmt::format(" pivot {} distance {:.2f} aim {:.3f}", Fmt(p), std::hypot(d.x, d.y, d.z),
                       std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / M_PI);
  }
  std::string pl = "player none";
  if (b.player) {
    const Vec3 pf = kuf::presentation::Forward(model);
    pl = fmt::format("player {} {} pos {} yaw {:.4f}", b.player_index,
                     player ? "interpolated" : i.player_cut ? i.player_cut : "tick",
                     Fmt(kuf::presentation::Position(model)), kuf::presentation::Azimuth(pf));
  }
  REXLOG_INFO("[kuf-present] {} tick {} alpha {:.4f} {}; {}{}", tick_frame ? "tick" : "build", b.tick, alpha, cam, pl,
              i.pulled_in && camera ? "; pulled in" : "");
  return shown;
}

}  // namespace

namespace kuf::presentation {

void Configure(bool render_only_frames, const Interpolation& what) {
  g_enabled = render_only_frames && what.camera_player;
  g_objects_enabled = g_enabled && what.objects;
  g_bones_enabled = g_enabled && what.bones;
  g_effects_enabled = g_enabled && what.effects;
  g_trace = REXCVAR_GET(kuf_trace_presentation);
  g_trace_objects = g_trace && REXCVAR_GET(kuf_trace_presentation_objects);
  g_trace_bones = g_trace && REXCVAR_GET(kuf_trace_presentation_bones);
  g_test_offset = g_enabled ? REXCVAR_GET(kuf_presentation_test_offset) : 0;
  if (g_test_offset != 0) {
    REXLOG_WARN("[kuf-present] isolation test: presented cameras {} units up, the player {}, field of view + 0.1 rad",
                g_test_offset, g_test_offset * 3 / 4);
  }
  if (render_only_frames) {
    REXLOG_INFO("[kuf-present] camera and player interpolation between ticks: {}, world objects: {}, skeletal poses: {}, "
                "effect clock: {}", g_enabled ? "on" : "off", g_objects_enabled ? "on" : "off",
                g_bones_enabled ? "on" : "off", g_effects_enabled ? "on" : "off");
  }
}

bool Enabled() { return g_enabled; }


void SetObjectsOnly(bool on) { g_objects_only.store(on, std::memory_order_relaxed); }
void SetBonesOnly(bool on) { g_bones_only.store(on, std::memory_order_relaxed); }
void SetEffectsOnly(bool on) { g_effects_only.store(on, std::memory_order_relaxed); }
double EffectLag() { return g_effect_lag; }

// The palettes written into the slot (since its tick started) still drawn by the same objects: the model's pointer
// set back to the tick's own palette (to_tick) or to the one written. Returns how many were set.
uint64_t SetPalettes(uint8_t* base, uint32_t slot, bool to_tick) {
  const uint32_t objects = Plausible(g_cur.renderer) ? Load32(base, g_cur.renderer + kRendererObjects) : 0;
  uint64_t set = 0;
  auto one = [&](const PoseWritten& w) {
    if (!Plausible(objects) || w.index >= kMaxObjectIndex || Load32(base, objects + w.index * 4) != w.object ||
        g_generation[w.index].load(std::memory_order_acquire) != w.generation) {
      return;  // released since: not the same object
    }
    const uint32_t at = w.object + kMeshPalette + slot * 4;
    if (Load32(base, at) == (to_tick ? w.palette : w.tick_palette)) {
      const uint32_t v = __builtin_bswap32(to_tick ? w.tick_palette : w.palette);
      std::memcpy(base + at, &v, 4);
      ++set;
    }
  };
  // A model written more than once points to its last palette: back to the tick's from it, forward to it.
  if (to_tick) {
    for (const PoseWritten& w : g_poses_written[slot]) one(w);
  } else {
    for (auto w = g_poses_written[slot].rbegin(); w != g_poses_written[slot].rend(); ++w) one(*w);
  }
  return set;
}

void Reset(uint8_t* base) {
  // A slot still holding exactly what was stored there for a frame was not written by the game since: the game's own
  // value back (as the next tick's capture would take it). Palettes and objects with their own identity checks.
  auto restore = [&](Written& w, uint32_t slot, bool camera) {
    if (!w.valid || !Plausible(w.address)) return;
    const uint32_t at = w.address + kSlotMatrix + slot * 64;
    if (Same(LoadMat(base, at), w.stored)) StoreMat(base, at, w.game);
    if (camera && LoadF(base, w.address + kViewFov + slot * 4) == w.stored_fov) {
      StoreF(base, w.address + kViewFov + slot * 4, w.game_fov);
    }
  };
  uint64_t palettes = 0;
  for (uint32_t slot = 0; slot < 2; ++slot) {
    if (g_bones_enabled) palettes += SetPalettes(base, slot, true);
    if (g_objects_enabled) RestoreObjects(base, g_cur.renderer, slot);
    if (g_enabled) {
      restore(g_camera_written[slot], slot, true);
      restore(g_player_written[slot], slot, false);
    }
    g_poses_written[slot].clear();
    g_objects_written[slot].clear();
    g_camera_written[slot] = {};
    g_player_written[slot] = {};
  }
  g_prev = {};
  g_cur = {};
  g_interval = {};
  g_object_plans.clear();
  g_capturing = false;
  g_poses_prev.clear();
  g_poses_cur.clear();
  g_pose_palette.clear();
  g_pose_plans.clear();
  g_parts_prev.clear();
  g_parts_cur.clear();
  g_part_plans.clear();
  g_model_cut.clear();
  g_model_id.clear();
  g_effect_lag = 0;
  std::lock_guard lock(g_stats_mutex);
  g_private_palettes[0].clear();
  g_private_palettes[1].clear();
  g_stats.pose_restored += palettes;
}

void OnTickStart(uint8_t* base, uint32_t slot) {
  if (!g_bones_enabled) return;
  slot &= 1;
  const uint64_t restored = SetPalettes(base, slot, true);
  g_poses_written[slot].clear();
  std::lock_guard lock(g_stats_mutex);
  g_private_palettes[slot].clear();
  g_stats.pose_restored += restored;
}

void OnTickSceneDone() {
  if (!g_capturing) return;
  g_capturing = false;
  SummarisePoses();
}

void OnTickSlotBuild(uint8_t* base, uint32_t renderer) {
  if (g_objects_enabled) RestoreObjects(base, renderer, Load32(base, kGameSlotIndex) & 1);
}

double OnTickFrame(uint8_t* base, uint32_t renderer, uint64_t tick, bool world, double alpha) {
  if (!g_enabled) return NAN;
  g_prev = std::move(g_cur);
  g_cur = Capture(base, renderer, tick, world);
  if (g_objects_enabled) CaptureObjects(base, g_cur);
  if (g_bones_enabled) {
    g_poses_prev = std::move(g_poses_cur);
    g_poses_cur.clear();
    g_pose_palette.clear();
    g_pose_plans.clear();
    g_parts_prev = std::move(g_parts_cur);
    g_parts_cur.clear();
    g_part_plans.clear();
    g_capturing = true;  // the scene build evaluates the poses (until OnTickSceneDone)
  }
  if (g_prev.valid && g_prev.have_camera && g_cur.have_camera && g_cur.tick == g_prev.tick + 1) {
    const Vec3 a = kuf::presentation::Position(g_prev.camera), b = kuf::presentation::Position(g_cur.camera);
    g_cur.eye_step = {b.x - a.x, b.y - a.y, b.z - a.z};
    g_cur.have_step = true;
  }
  DecideInterval();
  DecideObjects();
  if (g_cur.player) {
    const bool same = g_prev.player == g_cur.player && g_prev.player_index == g_cur.player_index;
    g_model_cut[g_cur.player] = !same ? "new" : g_interval.player_cut;
    g_model_id[g_cur.player] = {g_cur.player_index,
                                g_generation[g_cur.player_index % kMaxObjectIndex].load(std::memory_order_acquire), true};
  }
  if (g_trace) {
    const Snapshot& b = g_cur;
    REXLOG_INFO("[kuf-present] tick {} as written: camera {} eye {} az {:.4f} el {:.4f}{}; player {}; camera cut {}, "
                "player cut {}{}{}{}{}", b.tick, b.have_camera ? "yes" : "no", Fmt(kuf::presentation::Position(b.camera)),
                kuf::presentation::Azimuth(kuf::presentation::Forward(b.camera)),
                kuf::presentation::Elevation(kuf::presentation::Forward(b.camera)),
                b.view_info.modern ? fmt::format(" pivot {}", Fmt(b.view_info.pivot)) : std::string(),
                b.player ? Fmt(kuf::presentation::Position(b.player_matrix)) : std::string("none"),
                g_interval.camera_cut ? g_interval.camera_cut : "-",
                g_interval.player_cut ? g_interval.player_cut : "-",
                std::isfinite(g_interval.turn) ? fmt::format(", turn {:.4f}", g_interval.turn) : std::string(),
                b.view_info.event ? ", event camera" : "", b.restored ? ", game's matrix taken back" : "",
                b.player || !b.fresh || b.view_info.event
                    ? std::string()
                    : fmt::format(", nearest character {:.2f}, without bones {:.2f}", b.attach_error,
                                  b.unskinned_error));
  }
  return Write(base, alpha, true);
}

double OnBuildFrame(uint8_t* base, uint32_t renderer, double alpha) {
  if (!g_enabled || !g_cur.valid || renderer != g_cur.renderer) return NAN;
  // Unlike their models, the parts are not posed by the build: a part's pointer may still hold the palette written for
  // an earlier frame, in frame memory this build takes again. Back to the tick's own until the build writes one (a
  // part left unwritten, at alpha 1 or with its model not drawn, would draw from memory reused for something else).
  const uint32_t slot = Load32(base, kGameSlotIndex) & 1;
  for (const auto& [part, plan] : g_part_plans) {
    const auto tick = g_pose_palette.find(part);
    if (tick == g_pose_palette.end() || !Plausible(tick->second)) continue;
    const uint32_t v = __builtin_bswap32(tick->second);
    std::memcpy(base + part + kMeshPalette + slot * 4, &v, 4);
  }
  return Write(base, alpha, false);
}

void WriteJson(std::ostream& out) {
  std::lock_guard lock(g_stats_mutex);
  const Stats& s = g_stats;
  const auto map_json = [](const std::map<std::string, uint64_t>& m) {
    std::string j = "{";
    for (const auto& [k, v] : m) j += fmt::format("{}\"{}\": {}", j.size() > 1 ? ", " : "", k, v);
    return j + "}";
  };
  std::string bins;
  for (int k = 0; k < 10; ++k) bins += fmt::format("{}{}", k ? ", " : "", s.alpha_bins[k]);
  out << fmt::format(
      "  \"presentation\": {{\"interpolation\": {}, \"frames\": {}, \"tick_frames\": {}, \"build_frames\": {}, "
      "\"camera_interpolated\": {}, \"player_interpolated\": {}, \"held\": {}, \"alpha_bins\": [{}], \"ticks\": {}, "
      "\"player_ticks\": {}, \"restored\": {}, \"attach_error_max\": {:.3f}, \"camera_cuts\": {}, "
      "\"player_cuts\": {}, \"objects\": {{\"ticks\": {}, \"interpolated\": {}, \"unchanged\": {}, \"cuts\": {}, "
      "\"unexplained\": {}, \"releases\": {}, \"reused\": {}, \"restored\": {}, \"listed_max\": {}, \"step_max\": {:.2f}, "
      "\"turn_max\": {:.4f}, \"writes\": {}, \"write_us_mean\": {:.2f}, \"write_us_max\": {:.1f}}}, "
      "\"bones\": {{\"enabled\": {}, \"ticks\": {}, \"interpolated\": {}, \"model_space\": {}, \"unlisted\": {}, "
      "\"bones_held\": {}, \"cuts\": {}, \"models_max\": {}, \"bones_max\": {}, \"turn_max\": {:.4f}, \"writes\": {}, "
      "\"bones_written\": {}, \"frames\": {}, \"write_us_mean\": {:.2f}, \"write_us_max\": {:.1f}, "
      "\"length_error_max\": {:.5f}, \"restored\": {}, \"alloc_failed\": {}, \"bone_reads\": {}, "
      "\"bone_reads_written\": {}, \"physics_reads\": {}, \"physics_reads_written\": {}, \"parts\": {{\"ticks\": {}, "
      "\"interpolated\": {}, \"cuts\": {}, \"held\": {}, \"unanchored\": {}, \"turn_max\": {:.4f}, \"writes\": {}, "
      "\"progress_error_max\": {:.5f}, \"stretch_max\": {:.5f}}}}}}},\n",
      g_enabled ? "true" : "false", s.frames, s.tick_frames, s.build_frames, s.camera_interpolated,
      s.player_interpolated, s.held, bins, s.ticks, s.player_ticks, s.restored, s.attach_error_max,
      map_json(s.camera_cuts), map_json(s.player_cuts), s.object_ticks, s.object_interpolated, s.object_still,
      map_json(s.object_cuts), s.object_unexplained, g_releases.load(), s.object_reused, s.object_restored, s.object_listed_max,
      s.object_step_max, s.object_turn_max, s.object_writes,
      s.object_frames ? s.object_write_us / double(s.object_frames) : 0.0, s.object_write_us_max,
      g_bones_enabled ? "true" : "false", s.pose_ticks, s.pose_interpolated, s.pose_model_space, s.pose_unlisted,
      s.pose_bones_held, map_json(s.pose_cuts), s.pose_models_max, s.pose_bones_max, s.pose_turn_max, s.pose_writes,
      s.pose_bones_written,
      s.pose_frames, s.pose_frames ? s.pose_write_us / double(s.pose_frames) : 0.0, s.pose_write_us_max,
      s.pose_length_error_max, s.pose_restored, s.pose_alloc_failed, s.bone_reads, s.bone_reads_written,
      s.physics_reads, s.physics_reads_written, s.part_ticks, s.part_interpolated, map_json(s.part_cuts), s.part_held,
      s.part_unanchored, s.part_turn_max, s.part_writes, s.part_progress_error_max, s.part_stretch_max);
}

}  // namespace kuf::presentation

REX_EXTERN(__imp__sub_821A4F90);
REX_HOOK_RAW(sub_821A4F90) {  // a renderer object released: (renderer, index); its index can be reused after this
  const uint32_t index = ctx.r4.u32;
  if (index < kMaxObjectIndex) g_generation[index].fetch_add(1, std::memory_order_release);
  g_releases.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_821A4F90(ctx, base);
}

REX_EXTERN(__imp__sub_8219C8A8);
REX_HOOK_RAW(sub_8219C8A8) {  // a skinned model's pose evaluated into its palette for the game slot: (mesh, flag)
  const uint32_t mesh = ctx.r3.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8219C8A8(ctx, base);
  if (!g_bones_enabled || lr != kPoseFromVf96 || Load32(base, mesh) != kCharacterVtable) return;
  // vf96 next places the models attached to the bones from this palette: they follow the pose written here.
  const uint64_t r3 = ctx.r3.u64;
  if (kuf::pacing::InRenderOnlyBuild()) {
    const auto plan = g_pose_plans.find(mesh);
    if (plan != g_pose_plans.end()) WritePose(ctx, base, mesh, plan->second);
    for (const auto& [part, p] : g_part_plans) {  // the parts hanging from it, on its pose as written
      if (p.model == mesh && p.listed && (p.interpolate || g_test_offset != 0)) WritePart(ctx, base, part, p);
    }
  } else if (g_capturing) {
    if (const PoseTick* b = CapturePose(base, mesh)) {
      DecidePose(mesh, b);
      WritePose(ctx, base, mesh, g_pose_plans[mesh]);  // the tick's own frame, drawn at its alpha
    }
  }
  ctx.r3.u64 = r3;
}

namespace kuf::presentation {

// The game's readers of a skinned model's palette for the game slot outside the scene build's posing (vf96): the
// physics step's parts hanging from the model's bones (class 0x82103828, the physics manager's pre-simulate updates,
// sub_821C39E8 / sub_821C45C0 / sub_821CA680), and three not seen running in the tested routes (sub_821B5470,
// sub_821B9C98, sub_82567788; docs/HIGH_FPS.md 17.6). The tick's scene build may have pointed the palette at one
// written here: for the call they get the game's own.
void WithGamePalettes(PPCContext& ctx, uint8_t* base, PPCFunc* original, bool part_posed) {
  const uint32_t controller = ctx.r3.u32;
  if (!g_bones_enabled || kuf::pacing::InRenderOnlyBuild()) {
    original(ctx, base);
    return;
  }
  const uint32_t slot = Load32(base, kGameSlotIndex) & 1;
  const uint64_t set = REXCVAR_GET(kuf_debug_physics_sees_presented) ? 0 : SetPalettes(base, slot, true);
  bool written = false;
  for (const PoseWritten& w : g_poses_written[slot]) {
    written = written || Load32(base, w.object + kMeshPalette + slot * 4) == w.palette;
  }
  original(ctx, base);
  if (set) SetPalettes(base, slot, false);
  {
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.physics_reads;
    if (written) ++g_stats.physics_reads_written;
  }
  if (part_posed) {
    const uint64_t r3 = ctx.r3.u64;
    CapturePart(ctx, base, controller);
    ctx.r3.u64 = r3;
  }
}

}  // namespace kuf::presentation

REX_EXTERN(__imp__sub_821C39E8);
REX_HOOK_RAW(sub_821C39E8) { kuf::presentation::WithGamePalettes(ctx, base, __imp__sub_821C39E8, false); }
REX_EXTERN(__imp__sub_821C45C0);
REX_HOOK_RAW(sub_821C45C0) { kuf::presentation::WithGamePalettes(ctx, base, __imp__sub_821C45C0, true); }
REX_EXTERN(__imp__sub_821CA680);
REX_HOOK_RAW(sub_821CA680) { kuf::presentation::WithGamePalettes(ctx, base, __imp__sub_821CA680, false); }
REX_EXTERN(__imp__sub_821B5470);
REX_HOOK_RAW(sub_821B5470) { kuf::presentation::WithGamePalettes(ctx, base, __imp__sub_821B5470, false); }
REX_EXTERN(__imp__sub_821B9C98);
REX_HOOK_RAW(sub_821B9C98) { kuf::presentation::WithGamePalettes(ctx, base, __imp__sub_821B9C98, false); }
REX_EXTERN(__imp__sub_82567788);
REX_HOOK_RAW(sub_82567788) { kuf::presentation::WithGamePalettes(ctx, base, __imp__sub_82567788, false); }

REX_EXTERN(__imp__sub_8219D648);
REX_HOOK_RAW(sub_8219D648) {  // a skinned model's message handler (vf172): (mesh, message, ...)
  const uint32_t mesh = ctx.r3.u32, message = ctx.r4.u32;
  if (g_bones_enabled && (message == kMsgBoneByName || message == kMsgBoneByIndex) &&
      !kuf::pacing::InRenderOnlyBuild()) {
    // The game reading a bone of the palette: it must find its own, never one written here.
    const uint32_t slot = Load32(base, kGameSlotIndex) & 1;
    const uint32_t palette = Load32(base, mesh + kMeshPalette + slot * 4);
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.bone_reads;
    if (g_private_palettes[slot].count(palette)) ++g_stats.bone_reads_written;
  }
  __imp__sub_8219D648(ctx, base);
}
