// Render-only frames (M6D D3b-1; render_frames.h, docs/HIGH_FPS.md section 14).
//
// v1 addresses and offsets, found with the D3b-1 investigation's tools (freeze memory diffs per build phase, GPU
// traces of a tick's frame and its build's frame, hardware watchpoints under gdb) and the disassembly; each is checked
// where it is used (vtables, plausible values) so a wrong guess passes the game's own behaviour through.

#include "runtime/render_frames.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "runtime/frame_pacing.h"

REXCVAR_DEFINE_BOOL(kuf_render_build_hud, true, "KUF/Debug",
                    "M6D: render-only frames draw the tick's HUD (false: its entries are released after the tick's "
                    "own frame, as without render-only frames, so the other frames have no HUD: the K34 test case)");
REXCVAR_DEFINE_BOOL(kuf_debug_restore_released, false, "KUF/Debug",
                    "M6D K46 fault test: render-only builds restore the tick's object indices even when the object "
                    "an index named was released or replaced since (the crash K46 corrected)");
REXCVAR_DEFINE_BOOL(kuf_trace_effect_clock, false, "KUF/Debug",
                    "M6D D4b-4: log each drawn frame's effect clock (the frame timer's count, its lag behind the tick, "
                    "the time the effect shaders get; effect_check.py)");
REXCVAR_DEFINE_BOOL(kuf_debug_effect_rate_kept, false, "KUF/Debug",
                    "M6D D4b-4 fault test: the frame timer's rate changed for a frame's effect clock is not put back "
                    "(the run summary's effect_clock.leaks must count it)");
REXCVAR_DEFINE_INT32(kuf_trace_hud_from, -1, "KUF/Debug",
                     "M6D D4b-4A: from this tick, for --kuf_trace_hud_ticks ticks, every HUD entry of the tick's frame "
                     "logged once (its type and screen rectangle and the words after them; hud_check.py); -1: off");
REXCVAR_DEFINE_INT32(kuf_trace_hud_ticks, 600, "KUF/Debug", "M6D D4b-4A: see --kuf_trace_hud_from");
REXCVAR_DEFINE_INT32(kuf_debug_restore_stale_every, 0, "KUF/Debug",
                     "M6D K46 test: every N ticks the tick's saved scene setup list gains two records the builds must "
                     "leave out: an index whose renderer object entry is empty (an object listed, then released after "
                     "the lists were saved) and an index now holding another object (the index reused); 0: off");

REX_EXTERN(__imp__sub_8222B2C8);  // the HUD's entries deleted after its draw: (lists object, slot)
REX_EXTERN(__imp__sub_821B07E0);  // frame memory freed at the frame end: (arena)
REX_EXTERN(__imp__sub_821B03D8);
REX_EXTERN(__imp__sub_821B0648);
REX_EXTERN(__imp__sub_82811A78);  // the heap's free: (block)
REX_EXTERN(__imp__sub_8219E278);  // a mesh's per-frame reset for the game slot (vf200): (mesh)
REX_EXTERN(__imp__sub_821C60B0);  // the physics manager's step: (manager, dt)
REX_EXTERN(__imp__sub_821C5EA8);  // the physics manager's callbacks: (manager)
REX_EXTERN(__imp__sub_82235EC0);  // a mesh's world and previous-frame matrices for its draw: (mesh, ...)
REX_EXTERN(__imp__sub_821F5608);  // the scene draw: (renderer)
REX_EXTERN(__imp__sub_82224C60);  // the two-pass object draw: (its object, device)
REX_EXTERN(__imp__sub_821D24F0);  // the effect clock's readers (render thread): draw functions
REX_EXTERN(__imp__sub_821F0350);
REX_EXTERN(__imp__sub_822244B0);
REX_EXTERN(__imp__sub_825BE6B0);  // the UI's update and submission: (ui)
REX_EXTERN(__imp__sub_821A2E70);  // the slot marked as a world frame: (renderer)
REX_EXTERN(__imp__sub_827FE1A8);  // the CRT's per-thread data (_getptd): the rand() seed at + 20

namespace {

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

bool Plausible(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000; }

constexpr uint32_t kSyncEnabled = 0x82AC6658 + 28;  // the game/render sync object's "synchronised" (0: shutdown)
constexpr uint32_t kGameSlotIndex = 0x82AE760C;     // the slot the game thread builds
constexpr uint32_t kRenderSlotIndex = 0x82AE7610;   // the slot the render thread draws
constexpr uint32_t kRendererPointer = 0x82AE89DC;
constexpr uint32_t kWorldFrameCounter = 0x82AE7620;  // world frames, +1 per tick (sub_821A2E70)

// The renderer's parts (renderer + offset).
constexpr uint32_t kRendererSlotList = 6782 * 4;     // + slot * 4: the slot's frame packet object list (a pointer)
constexpr uint32_t kRendererOtherList = 27136;       // sub_821A2D48's object indices (not per slot)
constexpr uint32_t kRendererPost = 412;              // the post-processing (sub_821E5DD0), vtable 0x82102A6C
constexpr uint32_t kRendererEffects = 444;           // an effect system (sub_8221EEB0, sub_8221F828), vtable 0x821032F0
constexpr uint32_t kRendererObjectPass = 14520;      // the two-pass object draw (sub_82224C60), vtable 0x8210348C
constexpr uint32_t kRendererFrameTimer = 28864;      // the frame timer (sub_82214560 counts presents)
constexpr uint32_t kRendererDrawCount = 29640;       // +1 per scene draw (sub_821F5608)

constexpr uint32_t kPostVtable = 0x82102A6C, kPostBufferSelector = 84;  // which bloom buffer the frame writes (0/1)
constexpr uint32_t kEffectsVtable = 0x821032F0, kEffectsBuildCount = 80;  // +1 per scene build (sub_8221EEB0)
constexpr uint32_t kObjectPassVtable = 0x8210348C;
constexpr uint32_t kObjectPassList = 156;            // + slot * 16: 68-byte records (sub_821A6A68: index, matrix)
constexpr uint32_t kObjectPassAccumulation = 1600;   // its two-buffer accumulation, vtable 0x82103490
constexpr uint32_t kAccumulationVtable = 0x82103490;
constexpr uint32_t kFrameTimerCount = 12;            // frames presented (the effect shaders' time: count / rate)
constexpr uint32_t kFrameTimerRate = 20;             // their rate (30): the effect shaders' time is count / rate
constexpr uint32_t kUiSlotLists[] = {104, 136, 168};  // ui + this + slot * 16 (sub_825BE6B0 fills them)
constexpr uint32_t kUiList = 88;                      // ui + this: elements flagged at + 6
constexpr uint32_t kMeshPalette = 856, kMeshPalette2 = 864;  // a mesh's bone palettes, + slot * 4 (sub_8219A9B8)

bool RenderThreadFrame() { return kuf::pacing::RenderOnlyFrames() && kuf::pacing::OnRenderThread(); }

bool Synchronised(const uint8_t* base) { return Load32(base, kSyncEnabled) != 0; }

// ---- what the tick's frame consumes, restored before each build ----
// A frame consumes a vector by setting its end (+ 8) back to its begin (+ 4); the storage and the capacity (+ 12)
// stay, so restoring needs no guest allocation. Only vectors of values: object indices, records of an index and a
// matrix, pointers to persistent UI elements. Not the 2D list (renderer + 28976 + slot * 16): its items are heap blocks
// of animation deltas that the frame's state->vf36 (sub_821ABF50) applies once and frees.
// The renderer's slot list and sub_821A2D48's list hold indices into the renderer's object array (renderer + 29272,
// count at + 29268; the object pass's records index something else). The tick can release an object after its
// frame's lists were saved (saving at an Idol did: sub_821A2FD8 then read a null object in a build); a restored index
// whose object is no longer the one it named is left out.
struct SavedVector {
  uint32_t vec = 0;
  bool indexed = false;           // object indices
  std::vector<uint32_t> words;
  std::vector<uint32_t> objects;  // indexed: per word, the object it named when saved
  const char* name = "";
};
std::vector<SavedVector> g_saved;  // game thread
uint32_t g_saved_renderer = 0;     // game thread
std::atomic<uint32_t> g_ui{0};     // the UI manager (sub_825BE6B0's argument, from the game step)
std::atomic<uint64_t> g_restored{0}, g_restore_failures{0}, g_restore_dropped{0}, g_restore_injected{0};

constexpr uint32_t kRendererObjectCount = 29268, kRendererObjects = 29272;

uint32_t RendererObject(const uint8_t* base, uint32_t renderer, uint32_t index) {
  if (!Plausible(renderer) || index >= Load32(base, renderer + kRendererObjectCount)) return 0;
  const uint32_t objects = Load32(base, renderer + kRendererObjects);
  return Plausible(objects) ? Load32(base, objects + index * 4) : 0;
}

// renderer: the vector holds indices into its object array
void SaveVector(const uint8_t* base, uint32_t vec, uint32_t renderer = 0, const char* name = "") {
  if (!Plausible(vec)) return;
  const uint32_t b = Load32(base, vec + 4), e = Load32(base, vec + 8);
  SavedVector v;
  v.vec = vec;
  v.indexed = renderer != 0;
  v.name = name;
  for (uint32_t a = b; Plausible(b) && a < e && a < b + 0x10000; a += 4) {
    v.words.push_back(Load32(base, a));
    if (v.indexed) v.objects.push_back(RendererObject(base, renderer, v.words.back()));
  }
  g_saved.push_back(std::move(v));
}

void RestoreVectors(uint8_t* base) {
  for (const SavedVector& v : g_saved) {
    const uint32_t b = Load32(base, v.vec + 4), cap = Load32(base, v.vec + 12);
    if (!Plausible(b) || b + uint32_t(v.words.size()) * 4 > cap) {
      if (!v.words.empty()) g_restore_failures.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    uint32_t end = b;
    for (size_t i = 0; i < v.words.size(); ++i) {
      if (v.indexed) {
        const uint32_t now = RendererObject(base, g_saved_renderer, v.words[i]);
        if (!now || now != v.objects[i]) {
          if (g_restore_dropped.fetch_add(1, std::memory_order_relaxed) < 200) {
            REXLOG_INFO("[kuf-render] tick {}: restored {} {:08X} left out index {} (object {:08X}, now {:08X}){}",
                        kuf::pacing::TicksCompleted(), v.name, v.vec, v.words[i], v.objects[i], now,
                        REXCVAR_GET(kuf_debug_restore_released) ? " - kept (fault test)" : "");
          }
          if (!REXCVAR_GET(kuf_debug_restore_released)) continue;
        }
      }
      Store32(base, end, v.words[i]);
      end += 4;
    }
    Store32(base, v.vec + 8, end);
    g_restored.fetch_add(1, std::memory_order_relaxed);
  }
}

// The calling thread's CRT rand() seed (sub_827FACD0: seed = seed * 214013 + 2531011 at _getptd() + 20).
uint32_t RandSeedAddress(PPCContext& ctx, uint8_t* base) {
  const uint64_t r3 = ctx.r3.u64, lr = ctx.lr;
  __imp__sub_827FE1A8(ctx, base);
  const uint32_t ptd = ctx.r3.u32;
  ctx.r3.u64 = r3;
  ctx.lr = lr;
  return Plausible(ptd) ? ptd + 20 : 0;
}

// --kuf_trace_hud_from (D4b-4A, docs/HIGH_FPS.md 20): the HUD's entries as the UI submitted them for the tick (the 2D
// draw-list object's lists at + 88 / + 120 / + 152 + slot * 16: heap entries, each its type, then its screen rectangle
// x0, y0, x1, y1 in the 1280x720 layout), logged at the tick frame's HUD release. An element placed from a world
// position moves its rectangle with the camera or the object. Labelled with the frame's own tick: a frame drawn after
// the game completed the next tick was logged as the next tick's, and that one then skipped (D5 validation, 4K).
void TraceHud(const uint8_t* base, uint32_t lists, uint32_t slot) {
  static int64_t last = -1;  // render thread
  const int64_t from = REXCVAR_GET(kuf_trace_hud_from), tick = int64_t(kuf::pacing::RenderFrameTicks());
  if (from < 0 || tick < from || tick >= from + REXCVAR_GET(kuf_trace_hud_ticks) || tick == last) return;
  last = tick;
  for (const uint32_t off : {88u, 120u, 152u}) {
    const uint32_t vec = lists + off + slot * 16, b = Load32(base, vec + 4), e = Load32(base, vec + 8);
    for (uint32_t a = b; Plausible(b) && a < e && a < b + 4096; a += 4) {
      const uint32_t entry = Load32(base, a);
      if (!Plausible(entry)) continue;
      std::string words;
      for (uint32_t w = 5; w < 12; ++w) words += fmt::format(" {:08X}", Load32(base, entry + w * 4));
      REXLOG_INFO("[kuf-hud] tick {} list {} type {} rect {},{},{},{}{}", tick, off, Load32(base, entry),
                  int32_t(Load32(base, entry + 4)), int32_t(Load32(base, entry + 8)),
                  int32_t(Load32(base, entry + 12)), int32_t(Load32(base, entry + 16)), words);
    }
  }
}

// ---- the effect clock at the frame's moment (D4b-4, docs/HIGH_FPS.md 20) ----
// The effect shaders' time is the frame timer's count / its rate (renderer + 28864: + 12 frames presented, + 20 their
// rate, 30), read by four draw functions on the render thread only (sub_821D24F0, sub_821F0350, sub_822244B0 and the
// two-pass object draw sub_82224C60; no other reader of the rate in the image). The count advances once per tick's
// frame (a build's frame is set back, OnBuildFrameStart), so every frame of a tick showed the tick's moment. While one
// of those functions draws a frame whose world is shown `lag` ticks before its tick (1 - its alpha), the rate is set so
// that count / rate = (count - lag) / 30, and put back when it returns: the count, which the game keeps, is never
// changed, and nothing outside those draws sees the changed rate.
std::atomic<int> g_effect_depth{0};  // > 0 while an effect-time reader runs with the changed rate
std::atomic<uint64_t> g_effect_frames{0}, g_effect_sets{0}, g_effect_leaks{0}, g_effect_last_frame{0};
thread_local int t_effect_depth = 0;

struct EffectTime {
  uint32_t timer = 0, rate_bits = 0;  // timer 0: nothing changed
};

EffectTime BeginEffectTime(uint8_t* base) {
  EffectTime e;
  if (t_effect_depth++ > 0 || !RenderThreadFrame()) return e;  // the outermost reader sets it for the nested ones
  const double lag = kuf::pacing::RenderEffectLag();
  const uint32_t renderer = Load32(base, kRendererPointer);
  const uint32_t timer = Plausible(renderer) ? Load32(base, renderer + kRendererFrameTimer) : 0;
  const int32_t count = int32_t(Load32(base, timer + kFrameTimerCount));
  const uint32_t bits = Load32(base, timer + kFrameTimerRate);
  float rate;
  std::memcpy(&rate, &bits, 4);
  if (count <= 1 || !(rate > 0) || !std::isfinite(rate)) return e;
  const float shown_rate = float(double(rate) * count / (count - std::min(lag, 1.0)));
  uint32_t shown_bits;
  std::memcpy(&shown_bits, &shown_rate, 4);
  g_effect_depth.fetch_add(1, std::memory_order_acq_rel);
  Store32(base, timer + kFrameTimerRate, shown_bits);
  g_effect_sets.fetch_add(1, std::memory_order_relaxed);
  // One count of the frame (its first reader): the frame's number is the presents counted so far.
  const uint64_t frame = kuf::pacing::FramesDrawn();
  if (g_effect_last_frame.exchange(frame, std::memory_order_relaxed) != frame) {
    g_effect_frames.fetch_add(1, std::memory_order_relaxed);
    if (REXCVAR_GET(kuf_trace_effect_clock)) {
      REXLOG_INFO("[kuf-effects] frame {} tick {} count {} lag {:.4f} rate {:.4f} time {:.5f} (tick's {:.5f})", frame,
                  kuf::pacing::RenderFrameTicks(), count, lag, shown_rate, count / double(shown_rate),
                  count / double(rate));
    }
  }
  e.timer = timer;
  e.rate_bits = bits;
  return e;
}

void EndEffectTime(uint8_t* base, const EffectTime& e) {
  --t_effect_depth;
  if (!e.timer) return;
  if (!REXCVAR_GET(kuf_debug_effect_rate_kept)) Store32(base, e.timer + kFrameTimerRate, e.rate_bits);
  g_effect_depth.fetch_sub(1, std::memory_order_acq_rel);
}

std::atomic<uint32_t> g_effect_rate_bits{0};
// Game thread, when a tick starts: the rate must be the game's own whenever no effect-time reader runs (the original
// rate is the one the first frame of the run drew with, kept in g_effect_rate_bits).
// A reader that starts and ends between this check's two looks at the depth (the render thread sets the rate tens of
// thousands of times a second; the game thread can be preempted longer than one draw) leaves the depth 0 both times
// with its rate read in between: the set counter, counted while the depth is raised, tells (two such false leaks
// seen in 33,000 frames under memory pressure, D5 validation).
void CheckEffectRate(const uint8_t* base) {
  const uint32_t renderer = Load32(base, kRendererPointer);
  const uint32_t timer = Plausible(renderer) ? Load32(base, renderer + kRendererFrameTimer) : 0;
  const uint64_t sets = g_effect_sets.load(std::memory_order_acquire);
  if (!Plausible(timer) || g_effect_depth.load(std::memory_order_acquire) > 0) return;
  const uint32_t bits = Load32(base, timer + kFrameTimerRate);
  if (g_effect_depth.load(std::memory_order_acquire) > 0) return;  // a reader started meanwhile
  if (g_effect_sets.load(std::memory_order_acquire) != sets) return;  // a reader started and ended meanwhile
  uint32_t expected = 0;
  if (g_effect_rate_bits.compare_exchange_strong(expected, bits)) return;
  if (bits != expected && g_effect_depth.load(std::memory_order_acquire) == 0) {
    if (g_effect_leaks.fetch_add(1, std::memory_order_relaxed) < 20) {
      REXLOG_WARN("[kuf-effects] tick {}: the frame timer's rate is {:08X} outside the effect draws (the game's {:08X})",
                  kuf::pacing::TicksCompleted(), bits, expected);
    }
  }
}

// ---- kept until the tick's last frame is drawn, released once ----
// The render thread does not run the slot's consume-once releases at the end of the tick's frame; the game thread runs
// them when the next tick into the slot starts (OnTickStart): the slot's frames are drawn by then (the game thread
// only starts a tick into a slot the render thread has given back) and nothing has written into it yet.
//  - The HUD's draw lists (the 2D draw-list object, renderer + 30156: lists at + 88 / + 120 / + 152 + slot * 16) hold
//    heap entries the UI submits each tick; the HUD's draw (sub_8222B9F8) ends with sub_8222B2C8(lists, slot), which
//    deletes them and empties the slot's lists.
//  - The slot's frame memory: three arenas (0x82AE81FC + slot * 48, renderer + 8 + slot * 48 and renderer + 29308 +
//    slot * 48; each: its block at + 36, + 40 units used, + 44 its size, overflow blocks listed at + 4 with their sizes
//    at + 20) hold what the tick allocated for its frame, the skinned meshes' bone palettes among it (written by the
//    tick's physics step); the frame end frees them (sub_821B07E0, sub_821B03D8, sub_821B0648). A build's frame end
//    frees only what the build allocated: back to the mark the tick's frame left.
constexpr int kArenaKinds = 3;
constexpr uint32_t kFrameEndArenaCallers[kArenaKinds] = {0x821A3C34, 0x821A3C50, 0x821A3C58};  // in sub_821A3BA0
struct ArenaMark {
  uint32_t arena = 0;  // 0: none kept
  uint32_t used = 0, blocks = 0;
};
struct SlotRelease {
  std::vector<uint32_t> hud_lists;
  ArenaMark arenas[kArenaKinds];
};
std::mutex g_release_mutex;
SlotRelease g_release[2];  // per slot

void ResetArena(PPCContext& ctx, uint8_t* base, int kind, uint32_t arena) {
  ctx.r3.u64 = arena;
  if (kind == 0) {
    __imp__sub_821B07E0(ctx, base);
  } else if (kind == 1) {
    __imp__sub_821B03D8(ctx, base);
  } else {
    __imp__sub_821B0648(ctx, base);
  }
}

void ArenaAtFrameEnd(PPCContext& ctx, uint8_t* base, int kind) {
  const uint32_t arena = ctx.r3.u32;
  if (!RenderThreadFrame() || uint32_t(ctx.lr) != kFrameEndArenaCallers[kind] || !Synchronised(base)) {
    ResetArena(ctx, base, kind, arena);
    return;
  }
  const uint32_t slot = Load32(base, kRenderSlotIndex) & 1;
  const uint32_t blocks_begin = Load32(base, arena + 8), blocks_end = Load32(base, arena + 12);
  const uint32_t blocks = Plausible(blocks_begin) && blocks_end >= blocks_begin ? (blocks_end - blocks_begin) / 4 : 0;
  std::unique_lock lock(g_release_mutex);
  ArenaMark& mark = g_release[slot].arenas[kind];
  if (!kuf::pacing::DrawingRenderOnlyFrame() || mark.arena != arena) {
    if (mark.arena && mark.arena != arena) {  // not expected: another arena for the slot
      lock.unlock();
      ResetArena(ctx, base, kind, arena);
      return;
    }
    mark = {arena, Load32(base, arena + 40), blocks};  // the tick's frame: keep it all
    return;
  }
  const ArenaMark tick = mark;  // a build's frame: back to the tick frame's mark
  lock.unlock();
  for (uint32_t i = tick.blocks; i < blocks; ++i) {
    ctx.r3.u64 = Load32(base, blocks_begin + i * 4);
    __imp__sub_82811A78(ctx, base);
  }
  if (blocks > tick.blocks) {
    Store32(base, arena + 12, blocks_begin + tick.blocks * 4);
    const uint32_t sizes_begin = Load32(base, arena + 24);
    if (Plausible(sizes_begin)) Store32(base, arena + 28, sizes_begin + tick.blocks * 4);
  }
  Store32(base, arena + 40, tick.used);
}

// ---- the meshes' previous-frame matrices ----
// sub_82235EC0 gives the shaders a drawn mesh's matrix of the frame before (mesh + 228, with the frame it was last
// drawn in at + 292, against the renderer's draw count): its motion since the frame before. A build's frame repeats
// the tick's moment, so it repeats the tick frame's motion: each mesh starts from the state the tick's frame started
// from and is left as the tick's frame left it.
constexpr uint32_t kMotionState = 228, kMotionStateSize = 296 - 228;
struct MotionState {
  std::array<uint8_t, kMotionStateSize> before{}, after{};
};
std::map<uint32_t, MotionState> g_motion;  // render thread: the meshes the current tick's frame drew
bool g_motion_fresh = false;               // the next tick frame's first mesh starts a new map

}  // namespace

namespace kuf::render_frames {

void OnTickStart(PPCContext& ctx, uint8_t* base, uint32_t slot) {
  CheckEffectRate(base);
  SlotRelease release;
  {
    std::lock_guard lock(g_release_mutex);
    std::swap(release, g_release[slot & 1]);
  }
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
  for (const uint32_t lists : release.hud_lists) {
    ctx.r3.u64 = lists;
    ctx.r4.u64 = slot & 1;
    __imp__sub_8222B2C8(ctx, base);
  }
  for (int kind = 0; kind < kArenaKinds; ++kind) {
    if (release.arenas[kind].arena) ResetArena(ctx, base, kind, release.arenas[kind].arena);
  }
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
}

// K46 test (kuf_debug_restore_stale_every): the saved scene setup list (the one whose stale index crashed in
// sub_821A2FD8) as if the tick had listed two objects and then released one and replaced the other: an index whose
// entry is empty, then one whose entry holds another object than recorded. Only where the list's storage holds them.
void InjectStale(const uint8_t* base, uint32_t renderer) {
  const auto list = std::find_if(g_saved.begin(), g_saved.end(),
                                 [](const SavedVector& v) { return std::strcmp(v.name, "scene setup list") == 0; });
  const uint32_t count = Load32(base, renderer + kRendererObjectCount);
  const uint32_t objects = Load32(base, renderer + kRendererObjects);
  if (list == g_saved.end() || !Plausible(objects) || count > 0x10000) return;
  const uint32_t b = Load32(base, list->vec + 4), cap = Load32(base, list->vec + 12);
  if (!Plausible(b) || b + uint32_t(list->words.size() + 2) * 4 > cap) return;
  uint32_t released = count, live = count;
  for (uint32_t i = 0; i < count && (released == count || live == count); ++i) {
    const bool empty = Load32(base, objects + i * 4) == 0;
    if (empty && released == count) released = i;
    if (!empty && live == count) live = i;
  }
  if (released == count || live == count) return;
  list->words.push_back(released);
  list->objects.push_back(0x40000000);  // the object it named, released since
  list->words.push_back(live);
  list->objects.push_back(Load32(base, objects + live * 4) ^ 0x100);  // another object than the one there now
  g_restore_injected.fetch_add(2, std::memory_order_relaxed);
  REXLOG_INFO("[kuf-render] tick {}: test records in the saved scene setup list: index {} (entry empty), index {} "
              "(another object)", kuf::pacing::TicksCompleted(), released, live);
}

void SaveTickOutputs(const uint8_t* base, uint32_t renderer) {
  g_saved.clear();
  g_saved_renderer = renderer;
  const uint32_t slot = Load32(base, kGameSlotIndex) & 1;
  SaveVector(base, Load32(base, renderer + kRendererSlotList + slot * 4), renderer, "slot list");
  SaveVector(base, renderer + kRendererOtherList, renderer, "scene setup list");
  const uint32_t pass = Load32(base, renderer + kRendererObjectPass);
  if (Plausible(pass) && Load32(base, pass) == kObjectPassVtable) SaveVector(base, pass + kObjectPassList + slot * 16);
  const uint32_t ui = g_ui.load();
  if (Plausible(ui)) {
    for (const uint32_t off : kUiSlotLists) SaveVector(base, ui + off + slot * 16);
    SaveVector(base, ui + kUiList);
  }
  const int32_t every = REXCVAR_GET(kuf_debug_restore_stale_every);
  if (every > 0 && kuf::pacing::TicksCompleted() % uint64_t(every) == 0) InjectStale(base, renderer);
}

BuildState BeginBuild(PPCContext& ctx, uint8_t* base, uint32_t renderer, bool world) {
  BuildState state;
  // The game thread's CRT rand() seed: gameplay draws from it (drops, AI, combat); a build runs from it as the tick
  // left it and puts it back.
  state.seed_at = RandSeedAddress(ctx, base);
  state.seed = state.seed_at ? Load32(base, state.seed_at) : 0;
  if (world) {
    // sub_821A2E70 marks the slot as a world frame (renderer + 29656 + slot) and counts world frames (0x82AE7620),
    // the clock units animate by (a unit's frame: its start + that count - its start count) and effect shaders take
    // their time from. A build draws the tick's world frame again: marked, not counted.
    const uint32_t world_frames = Load32(base, kWorldFrameCounter);
    ctx.r3.u64 = renderer;
    __imp__sub_821A2E70(ctx, base);
    Store32(base, kWorldFrameCounter, world_frames);
    RestoreVectors(base);
  }
  // An effect system counts the scene builds (+ 80, sub_8221EEB0): a build is the tick's scene build again.
  state.effects = Load32(base, renderer + kRendererEffects);
  if (Plausible(state.effects) && Load32(base, state.effects) == kEffectsVtable) {
    state.effects_built = Load32(base, state.effects + kEffectsBuildCount);
  } else {
    state.effects = 0;
  }
  return state;
}

void EndBuild(uint8_t* base, const BuildState& state) {
  if (state.effects) Store32(base, state.effects + kEffectsBuildCount, state.effects_built);
  if (state.seed_at) Store32(base, state.seed_at, state.seed);
}

void OnBuildFrameStart(uint8_t* base) {
  // The frame timer counts presents (sub_82214560, in the draw): + 12 frames presented, + 20 their rate (30); the
  // effect shaders' time is frames / rate (sub_821D24F0). A build's frame shows the tick's moment: it starts from the
  // count the tick's frame was drawn with, and its own count brings it back.
  const uint32_t renderer = Load32(base, kRendererPointer);
  const uint32_t timer = renderer ? Load32(base, renderer + kRendererFrameTimer) : 0;
  if (Plausible(timer)) Store32(base, timer + kFrameTimerCount, Load32(base, timer + kFrameTimerCount) - 1);
}

uint64_t RestoredVectors() { return g_restored.load(); }
uint64_t RestoreFailures() { return g_restore_failures.load(); }
uint64_t RestoreDropped() { return g_restore_dropped.load(); }
uint64_t RestoreInjected() { return g_restore_injected.load(); }
uint64_t EffectClockFrames() { return g_effect_frames.load(); }
uint64_t EffectClockSets() { return g_effect_sets.load(); }
uint64_t EffectClockLeaks() { return g_effect_leaks.load(); }

}  // namespace kuf::render_frames

// ---- hooks ----

REX_HOOK_RAW(sub_825BE6B0) {  // the UI's update and submission: (ui)
  if (!kuf::pacing::InRenderOnlyBuild()) g_ui = ctx.r3.u32;
  __imp__sub_825BE6B0(ctx, base);
}

REX_HOOK_RAW(sub_8222B2C8) {  // the HUD's entries deleted after its draw: (lists object, slot)
  TraceHud(base, ctx.r3.u32, ctx.r4.u32 & 1);
  if (RenderThreadFrame() && Synchronised(base) && REXCVAR_GET(kuf_render_build_hud)) {
    std::lock_guard lock(g_release_mutex);
    auto& lists = g_release[ctx.r4.u32 & 1].hud_lists;
    if (std::find(lists.begin(), lists.end(), ctx.r3.u32) == lists.end()) lists.push_back(ctx.r3.u32);
    return;
  }
  __imp__sub_8222B2C8(ctx, base);
}

REX_HOOK_RAW(sub_821B07E0) { ArenaAtFrameEnd(ctx, base, 0); }  // frame memory 0x82AE81FC + slot * 48: (arena)
REX_HOOK_RAW(sub_821B03D8) { ArenaAtFrameEnd(ctx, base, 1); }  // frame memory renderer + 8 + slot * 48
REX_HOOK_RAW(sub_821B0648) { ArenaAtFrameEnd(ctx, base, 2); }  // frame memory renderer + 29308 + slot * 48

// A skinned mesh's bone palettes for the game slot are blocks of the slot's frame memory: a character's evaluated by
// the scene build (vf96, docs/HIGH_FPS.md 17.2), a physics-driven part's written by the tick's physics step (the
// physics manager's pre-simulate updates, sub_821C39E8 -> sub_8219A9B8 / sub_821C3340); the scene build's first part
// clears the pointers first (the mesh's vf200, sub_8219E278, also for its children). A build runs no physics step: it
// keeps the tick's palettes, valid as the slot's frame memory is kept.
REX_HOOK_RAW(sub_8219E278) {  // a mesh's per-frame reset for the game slot: (mesh)
  if (!kuf::pacing::InRenderOnlyBuild()) {
    __imp__sub_8219E278(ctx, base);
    return;
  }
  const uint32_t mesh = ctx.r3.u32, slot = Load32(base, kGameSlotIndex) & 1;
  const uint32_t palette = Load32(base, mesh + kMeshPalette + slot * 4);
  const uint32_t palette2 = Load32(base, mesh + kMeshPalette2 + slot * 4);
  __imp__sub_8219E278(ctx, base);
  Store32(base, mesh + kMeshPalette + slot * 4, palette);
  Store32(base, mesh + kMeshPalette2 + slot * 4, palette2);
}

// The scene build's second part runs the tick's physics: the physics manager (renderer + 30160, vtable 0x82103790)
// runs its callbacks (vf28) at the start and steps the PhysX scene (vf24: its pre-simulate updates, simulate and
// fetchResults over 1/30 s) in the middle. A build draws the tick's results again: it runs neither.
REX_HOOK_RAW(sub_821C60B0) {  // the physics manager's step: (manager, dt)
  if (kuf::pacing::InRenderOnlyBuild()) return;
  __imp__sub_821C60B0(ctx, base);
}

REX_HOOK_RAW(sub_821C5EA8) {  // the physics manager's callbacks: (manager)
  if (kuf::pacing::InRenderOnlyBuild()) return;
  __imp__sub_821C5EA8(ctx, base);
}

REX_HOOK_RAW(sub_82235EC0) {  // a mesh's world and previous-frame matrices for its draw: (mesh, ...)
  if (!RenderThreadFrame()) {
    __imp__sub_82235EC0(ctx, base);
    return;
  }
  const uint32_t mesh = ctx.r3.u32;
  uint8_t* state = base + mesh + kMotionState;
  if (!kuf::pacing::DrawingRenderOnlyFrame()) {
    if (g_motion_fresh) {
      g_motion.clear();
      g_motion_fresh = false;
    }
    MotionState& m = g_motion[mesh];
    std::memcpy(m.before.data(), state, kMotionStateSize);
    __imp__sub_82235EC0(ctx, base);
    std::memcpy(m.after.data(), state, kMotionStateSize);
    return;
  }
  auto it = g_motion.find(mesh);
  if (it != g_motion.end()) std::memcpy(state, it->second.before.data(), kMotionStateSize);
  __imp__sub_82235EC0(ctx, base);
  if (it != g_motion.end()) std::memcpy(state, it->second.after.data(), kMotionStateSize);
}

REX_HOOK_RAW(sub_821F5608) {  // the scene draw: (renderer)
  if (!RenderThreadFrame()) {
    __imp__sub_821F5608(ctx, base);
    return;
  }
  if (!kuf::pacing::DrawingRenderOnlyFrame()) {
    g_motion_fresh = true;  // a tick's frame: its meshes' motion states are recorded anew
    __imp__sub_821F5608(ctx, base);
    return;
  }
  const uint32_t renderer = ctx.r3.u32;
  // The draw counts the frames it draws (renderer + 29640); objects stamp their previous-frame matrices with it (one
  // frame on: motion; the same frame: another view of it). A build's frame counts as the tick's frame.
  Store32(base, renderer + kRendererDrawCount, Load32(base, renderer + kRendererDrawCount) - 1);
  // The post-processing blends with the previous frame's bloom: its two buffers alternate each frame by the selector
  // at + 84. A build's frame uses the tick frame's.
  const uint32_t post = Load32(base, renderer + kRendererPost);
  if (Plausible(post) && Load32(base, post) == kPostVtable && Load32(base, post + kPostBufferSelector) <= 1) {
    Store32(base, post + kPostBufferSelector, Load32(base, post + kPostBufferSelector) ^ 1);
  }
  __imp__sub_821F5608(ctx, base);
}

REX_HOOK_RAW(sub_82224C60) {  // the two-pass object draw: (its object, device)
  // The pass keeps a two-buffer accumulation (its sub-object at + 1600: + 160 the buffer this frame writes, + 164 the
  // one it read; sub_82223A70 swaps them each frame). A build's frame starts from the tick frame's choice.
  const uint32_t pass = ctx.r3.u32;
  if (RenderThreadFrame() && kuf::pacing::DrawingRenderOnlyFrame() &&
      Load32(base, pass + kObjectPassAccumulation) == kAccumulationVtable) {
    Store32(base, pass + kObjectPassAccumulation + 160, Load32(base, pass + kObjectPassAccumulation + 164));
  }
  const EffectTime e = BeginEffectTime(base);
  __imp__sub_82224C60(ctx, base);
  EndEffectTime(base, e);
}

// The other readers of the effect clock (above): drawn at the frame's moment.
REX_HOOK_RAW(sub_821D24F0) {
  const EffectTime e = BeginEffectTime(base);
  __imp__sub_821D24F0(ctx, base);
  EndEffectTime(base, e);
}
REX_HOOK_RAW(sub_821F0350) {
  const EffectTime e = BeginEffectTime(base);
  __imp__sub_821F0350(ctx, base);
  EndEffectTime(base, e);
}
REX_HOOK_RAW(sub_822244B0) {
  const EffectTime e = BeginEffectTime(base);
  __imp__sub_822244B0(ctx, base);
  EndEffectTime(base, e);
}

