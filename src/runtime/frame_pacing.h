// Frame pacing (M6D, docs/HIGH_FPS.md).
//
// --kuf_fixed_tick (default on): the game thread runs its 30 Hz ticks on a host clock, so the guest's vblank rate,
// VSync, the display and the rendering speed can no longer make the game faster than 30 ticks per real second. The
// render thread still draws each tick exactly once (the renderer consumes a tick's data as it draws it). With
// --kuf_fixed_tick=false the original pacing: one tick per presented frame.
//
// --kuf_render_repeat (D1 diagnostic only): the render thread draws the latest tick again up to that rate. Repeated
// frames are black (the renderer has consumed the tick); used by the input script's freeze to measure that.
//
// --kuf_frame_rate (D3b-1, default 0 = off): render-only frames between the ticks up to that rate (-1: as many as
// fit). After each tick the game thread builds the tick's render slot again without a game step (the tick's own slot
// build, sub_821F49E8 / sub_821F4D00) and the unchanged renderer draws it once; render_frames.cpp keeps the frame the
// tick's and the game out of it. A frame that would not be finished before the next tick is due is dropped, never a
// tick. --kuf_render_builds=n (diagnostic) builds n such frames per tick; with it the input script's freeze holds the
// ticks and keeps building. --kuf_trace_render_lists logs the slot containers and who fills the frame packet;
// --kuf_trace_slot_writes writes the guest words each phase of 4 ticks changes; --kuf_trace_game_state logs the game
// state per tick (the isolation tests). docs/HIGH_FPS.md sections 12 and 14.
#pragma once

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>

#include "runtime/pc_settings.h"

namespace kuf::pacing {

// Reads --kuf_fixed_tick and --kuf_render_repeat (and sets up the SDK's vblank mode for the latter). Before the
// graphics system starts.
void Configure(const std::filesystem::path& user_data_root);

// The frame-rate settings changed (the PC Settings menu, D5): they take effect when the next tick has been handed to
// the render thread (frame_pacing.cpp ApplyFrameRequest), within one tick, without a restart.
void RequestFrameRate(const kuf::pc::FrameRateSettings& frame);

bool FixedTick();
bool Repeating();
int RepeatTarget();  // --kuf_render_repeat: 0 = off, -1 = unlimited

// The present hook, once per presented frame: true when the frame shows a game tick for the first time (always,
// unless --kuf_render_repeat draws ticks again). Scripts and captures count these frames.
bool OnPresent();

// Game ticks completed so far, and the game's own tick counter (0x82ABB740; 0 before the first tick).
uint64_t TicksCompleted();
// Frames presented so far (render thread's count; the frame being drawn is the next).
uint64_t FramesDrawn();
uint32_t GameTickCounter();

// Render-only frames (render_frames.cpp): configured (--kuf_frame_rate or --kuf_render_builds); the game thread inside
// one's scene build; the calling thread is the render thread; the render thread is drawing one (not a tick's frame).
bool RenderOnlyFrames();
bool InRenderOnlyBuild();
bool OnRenderThread();
// Render thread: how far behind its tick's own the effect clock of the frame being drawn is (ticks, 0 to 1; D4b-4).
double RenderEffectLag();
// Render thread: the tick number of the frame being drawn (TicksCompleted when it was handed over; the game may have
// completed another tick by the time a slow frame is drawn). Elsewhere, and without render-only frames: TicksCompleted.
uint64_t RenderFrameTicks();
bool DrawingRenderOnlyFrame();

// "pacing": {...} for run-summary.json.
void WriteJson(std::ostream& out);

// D1/D3a diagnostic (the input script's `freeze <seconds> <label>`, with --kuf_render_repeat or --kuf_render_builds):
// holds the game for that long while the render thread keeps drawing the same tick (repeat) or the game thread keeps
// building render slots from it (builds); captures three of those frames (with builds also the last tick's frame,
// <label>_tick) and writes <user data>/freeze-<label>.txt, the guest memory that changed against a control period of
// the same length with nothing drawn.
bool RequestFreeze(int seconds, const std::string& label, uint64_t frame);

// The input script's `trace` (K35): GPU-trace the next frame that shows a game tick for the first time, with SDK patch
// 0009's trace (--gpu_trace_frame, which must be on, e.g. --gpu_trace_frame=2000000000 to wait for this). When the
// render thread starts that frame, its swap number in the SDK's command processor is the number of frames presented so
// far: the game's only VdSwap call site (telemetry.cpp) and the command processor count the same swaps, one PM4 swap
// packet per VdSwap, in order. So the trace is anchored to the game's tick frames, as the scripts are, not to a swap
// number, which since the fixed tick depends on loading time. Logs "[kuf-trace] GPU trace of tick frame ...".
void RequestGpuTrace(uint64_t frame);

}  // namespace kuf::pacing
