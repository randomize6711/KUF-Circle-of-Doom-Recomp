// Render-only frames (M6D D3b-1, docs/HIGH_FPS.md section 14): drawing a game tick again, between ticks, from the
// render data its game step left, without running any of the game.
//
// frame_pacing.cpp schedules the frames (--kuf_frame_rate, or the D3a diagnostic --kuf_render_builds) and runs each
// one's scene build on the game thread between ticks (BuildOnce). This file makes such a frame the tick's frame again
// and keeps the game out of it:
//   - what the tick's frame consumes is kept or restored: the frame packet's object list and two other lists of
//     object indices, the two-pass object draw's records and the UI's lists are restored before each build; the
//     HUD's entries, the slot's three frame-memory arenas (the bone palettes among them) and the meshes' palette
//     pointers are kept until the tick's last frame is drawn, then released once;
//   - nothing simulates: no physics step, no physics callbacks, the game thread's rand() seed put back;
//   - nothing advances per drawn frame: the world frame count (animation and effect time), the renderer's draw count,
//     the frame timer (effect shaders' time), an effect system's counters, the bloom's and the two-pass draw's
//     buffer alternation and the meshes' previous-frame matrices repeat the tick frame's.
// With render-only frames off (the default) none of this runs: every hook passes straight through.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace kuf::render_frames {

// Game thread, when a game tick starts, before it: the deferred releases of the slot the tick is about to write (the
// game slot index), whose frames have all been drawn.
void OnTickStart(PPCContext& ctx, uint8_t* base, uint32_t slot);

// Game thread, at the tick's own slot build (sub_821F49E8, before it): the lists its frame consumes.
void SaveTickOutputs(const uint8_t* base, uint32_t renderer);

// Game thread, around a render-only build's scene build (both slot indices on the tick's slot). `world`: the tick
// marked its slot as a world frame (sub_821A2E70; not in menus and while paused).
struct BuildState {
  uint32_t seed_at = 0, seed = 0;     // the game thread's CRT rand() seed
  uint32_t effects = 0, effects_built = 0;
};
BuildState BeginBuild(PPCContext& ctx, uint8_t* base, uint32_t renderer, bool world);
void EndBuild(uint8_t* base, const BuildState& state);

// Render thread, when it takes a render-only build's slot (before the frame's first draw).
void OnBuildFrameStart(uint8_t* base);

// For run-summary.json and the logs.
uint64_t RestoredVectors();
uint64_t RestoreFailures();
uint64_t RestoreDropped();  // restored records left out: their object was released after the tick saved them
uint64_t RestoreInjected();  // K46 test records added to the saved lists (kuf_debug_restore_stale_every)
// The effect clock at the frame's moment (D4b-4): frames drawn with it, rate changes made (one per reader call), and
// tick starts that found the timer's rate changed outside the effect draws (must be 0).
uint64_t EffectClockFrames();
uint64_t EffectClockSets();
uint64_t EffectClockLeaks();

}  // namespace kuf::render_frames
