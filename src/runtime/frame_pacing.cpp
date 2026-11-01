// Frame pacing (M6D; frame_pacing.h, docs/HIGH_FPS.md).
//
// The game runs two threads linked by a sync object (0x82AC6658: +0 "free" and +4 "filled" semaphores, both
// starting at 2 / 0 with a maximum of 2, +20/+24 their counts, +28 = synchronised, cleared at shutdown):
//   game thread sub_826F0FB8:   sub_826F2DB8 wait "free"   -> sub_826F0D58 one tick  -> sub_826F2E10 release "filled"
//   render thread sub_826EB458: sub_826F2EA0 wait "filled" -> sub_826E9FC8 one frame -> sub_826F2E40 release "free"
// so the original game draws exactly one frame per tick, and the console's present interval (two vblanks) paces both.
//
// Fixed tick (--kuf_fixed_tick, default on; D2): after its own wait for "free" the game thread sleeps until the next
// tick time on a host clock (1/30 s steps from an absolute schedule; a backlog of more than two ticks is dropped, so a
// stall slows the game as the original's missed vblanks did instead of speeding it up afterwards). Nothing else
// changes: the render thread still draws each tick once and the console's present interval still aligns the frames
// with the guest's vblanks, so with the default settings the game looks and plays as before; but the guest's vblank
// rate (--video_mode_refresh_rate, --vsync=false), the display and the rendering speed can no longer make the game
// run faster than 30 ticks per real second (M6D: they did, up to 5.9 times, docs/HIGH_FPS.md section 3).
//
// Drawing a tick more than once (--kuf_render_repeat, a D1 diagnostic only): the renderer consumes a tick's render
// data as it draws it. A tick writes into one of two slots (index 0x82AE760C, flipped at the tick's end); a frame
// draws the slot at 0x82AE7610 (sub_821F5608 and the game state's vf36/vf40). The draw clears each view's per-slot
// lists (sub_821EA3A0), and the end of the frame (sub_821A3BA0), after the device's end-of-frame step (sub_821E3F48),
// clears the slot's draw list (sub_8220FB00, an erase of renderer[(slot + 6782) * 4]), runs the slot's deferred
// resource commands (sub_821AA7A0, renderer + 29444 + slot * 16: deletes), frees and rebuilds the slot's three
// frame-memory pools (sub_821B07E0, sub_821B03D8, sub_821B0648), clears the slot's byte at renderer + 29656 + slot,
// releases the objects the slot kept alive (sub_821F0BB8), flips 0x82AE7610 and collects released GPU resources
// (sub_821C8710). The repeat mode defers all of that to the moment it moves on to the next tick, but the draw also
// empties further lists (the game's render command queue renderer + 29412 + slot * 16, lists of other objects, among
// them a physics callback object; freeze reports), so repeated frames are still black: not for play. With it the
// render thread draws the latest tick again up to the given rate and the guest gets 1000 vblanks per second.
//
// Also here: the render-only frames' scheduling and builds (--kuf_frame_rate, the D3a diagnostic --kuf_render_builds;
// what makes them the tick's frames is in render_frames.cpp), the input script's `trace` (K35), which points the SDK's
// GPU trace at the next frame showing a new tick, and its items' delivery at each tick start (K38).

#include "coop/coop_runtime.h"
#include "runtime/lobby_screen.h"
#include "runtime/frame_pacing.h"
#include "runtime/controls_screen.h"
#include "runtime/menu_pointer.h"
#include "runtime/pc_settings_menu.h"
#include "runtime/title_menu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <sys/uio.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>

#include "input/input_script.h"
#include "runtime/frame_capture.h"
#include "runtime/pc_settings.h"
#include "runtime/presentation.h"
#include "runtime/presentation_math.h"
#include "runtime/render_frames.h"
#include "runtime/telemetry.h"
#include "runtime/tick_schedule.h"
#include "runtime/adaptive_rate.h"

REXCVAR_DEFINE_BOOL(kuf_fixed_tick, true, "KUF",
                    "Run the game's 30 Hz ticks on a host clock, so the guest's vblank rate, VSync and the display "
                    "cannot change the game's speed (M6D). false = the original pacing: one tick per presented frame");
REXCVAR_DEFINE_INT32(kuf_render_repeat, 0, "KUF/Debug",
                     "M6D D1 diagnostic, not for play (repeated frames are black): draw the latest tick again up to "
                     "this many frames per second (-1 = unlimited); needed by the input script's freeze");

REX_EXTERN(__imp__sub_821A3BA0);  // the end of a frame (FinishSlot)
REX_EXTERN(__imp__sub_821E3F48);  // ... its device step
REX_EXTERN(__imp__sub_821C8710);  // ... its GPU resource collection
REX_EXTERN(__imp__sub_821EA3A0);  // a view's per-slot list clear (FinishSlot)

REXCVAR_DEFINE_INT32(kuf_trace_render_lists, 0, "KUF/Debug",
                     "M6D diagnostic: from this game tick on, log the sizes of the render slots' draw lists before and "
                     "after each of the next 48 frames");
REXCVAR_DEFINE_INT32(kuf_frame_rate, 0, "KUF",
                     "Frames drawn per second (M6D): 0 = one per game tick, 30 (the original); 31-480 = render-only "
                     "frames between the ticks up to that rate, drawn from the tick's render data (the camera and "
                     "the player interpolated between ticks: --kuf_frame_interpolation); -1 = as many as fit. The "
                     "game runs 30 ticks per second");
REXCVAR_DEFINE_INT32(kuf_render_builds, 0, "KUF/Debug",
                     "M6D D3a diagnostic, not for play: after each game tick, build this many further render slots "
                     "from the same game state without a game step (render-only builds), each drawn once; the input "
                     "script's freeze then holds the game and keeps building");
REXCVAR_DEFINE_INT32(kuf_trace_slot_writes, 0, "KUF/Debug",
                     "M6D D3a diagnostic: from this game tick on, for 4 ticks with the render thread idle, write which "
                     "guest words the game step, the render slot build and the frame between change "
                     "(slot-writes-<tick>.txt)");
REXCVAR_DEFINE_BOOL(kuf_freeze_alpha_sweep, false, "KUF/Debug",
                    "M6D: with frame interpolation, the input script's freeze first draws the held tick interval at "
                    "alpha 0, 0.25, 0.5, 0.75 and 1 and captures each (<label>_a000 ... _a100), then the same with "
                    "only the world objects moving, the camera and the player at alpha 1 (<label>_o000 ... _o100), then "
                    "with only the skeletal poses moving (<label>_b000 ... _b100), then with only the effect clock "
                    "moving (<label>_e000 ... _e100; tests)");
REXCVAR_DEFINE_INT32(kuf_trace_frame_clock, -1, "KUF/Debug",
                     "M6D K44: from this game tick on, for 900 ticks, log the presentation clock at each tick "
                     "([kuf-clock]): the tick's start against its schedule, the clock's phase, each frame step and what "
                     "became of it (built, late, render busy)");
REXCVAR_DEFINE_INT32(kuf_debug_tick_stall_at, -1, "KUF/Debug",
                     "M6D K44 test: hold the game thread before this game tick starts for --kuf_debug_tick_stall_us "
                     "(a late tick, as the host's load makes one)");
REXCVAR_DEFINE_INT32(kuf_debug_tick_stall_us, 0, "KUF/Debug", "M6D K44 test: see --kuf_debug_tick_stall_at");
REXCVAR_DEFINE_BOOL(kuf_present_clock_align, true, "KUF/Frame Rate",
                    "D6 (K49): at frame rates that are a multiple of 30, the frames between ticks on the tick's "
                    "schedule (evenly spaced); false: the clock keeps the phase it restarted with (D5)");
REXCVAR_DEFINE_BOOL(kuf_present_clock_from_handoff, true, "KUF/Frame Rate",
                    "M8: with the clock on the tick's schedule (kuf_present_clock_align), the frames between ticks step "
                    "from when the tick's own frame was handed to the render thread, not from the tick's scheduled "
                    "start (that frame reaches the renderer after the tick's work: 60 fps alternated 14 / 19.5 ms)");
REXCVAR_DEFINE_BOOL(kuf_frame_rate_adaptive, false, "KUF/Frame Rate",
                    "M8: at a frame-rate limit that is a multiple of 30, when the GPU's time per frame shows it cannot "
                    "draw that many frames, draw the largest multiple of 30 it can (evenly spaced on the tick's "
                    "schedule) instead of dropping frames unevenly; back up when it can again (needs "
                    "--gpu_timestamp_stats for the GPU's time)");
REXCVAR_DEFINE_DOUBLE(kuf_frame_rate_adaptive_load, 0.93, "KUF/Frame Rate",
                      "M8: --kuf_frame_rate_adaptive draws n frames per tick while n x the GPU's time per frame stays "
                      "within this share of a tick");
REXCVAR_DEFINE_BOOL(kuf_present_clock_keep_late_phase, false, "KUF/Debug",
                    "M6D K44 test, not for play: the presentation clock as before K44 (a frame step too late to build "
                    "before the next tick is skipped and the clock keeps its phase, which can lock the frame rate "
                    "at 30 or 90)");
REXCVAR_DEFINE_BOOL(kuf_debug_frame_rate_ignored, false, "KUF/Debug",
                    "M6D D5 fault test: frame-rate changes made in the PC Settings menu are dropped before they reach the "
                    "frame scheduler (the menu and the file still show them; the tests must notice)");
REXCVAR_DEFINE_BOOL(kuf_trace_game_state, false, "KUF/Debug",
                    "M6D: log the game state each tick at its slot build ([kuf-state]): the game's tick counter, the "
                    "game thread's CRT rand() seed and each submitted object's index, class and position (6 decimals: "
                    "runs of the same route compare exactly)");

REX_EXTERN(__imp__sub_821A2E70);  // the tick's mark of its slot as a world frame (render-only builds)
REX_EXTERN(__imp__sub_821F49E8);  // the tick's render slot build: queued work and draw lists
REX_EXTERN(__imp__sub_821F4D00);  // ... the views and the frame's setup
REX_EXTERN(__imp__sub_821F4CD8);  // ... the flip of the game's slot index
REX_EXTERN(__imp__sub_826F2DB8);  // the game thread's wait for "free"
REX_EXTERN(__imp__sub_826F2E10);  // the game thread's release of "filled"
REX_EXTERN(__imp__sub_826F2E40);  // the render thread's release of "free"

namespace {

using Clock = std::chrono::steady_clock;

// v1 addresses (docs/HIGH_FPS.md).
constexpr uint32_t kSyncEnabled = 28;              // sync object +28: 0 = not synchronised (shutdown)
constexpr uint32_t kGameTickCounter = 0x82ABB740;  // ++ once per tick (sub_826F0D58)
constexpr uint32_t kRendererDevice = 28860;        // renderer + 28860: the D3D device (sub_821E3F48's argument)
constexpr uint32_t kDrawViewClearCaller = 0x821F5B44;  // return address of sub_821F5608's sub_821EA3A0 (per view)
constexpr uint32_t kRenderSlotIndex = 0x82AE7610;  // the slot the render thread draws
constexpr uint32_t kGameSlotIndex = 0x82AE760C;    // the slot the game thread builds
constexpr uint32_t kRendererPointer = 0x82AE89DC;  // the renderer
constexpr uint32_t kRendererSlotList = 6782 * 4;   // renderer + this + slot * 4: the slot's draw list (a pointer)
constexpr uint32_t kRendererViews = 26548;         // renderer + this: vector of view indices into the object array
constexpr uint32_t kRendererObjects = 29272;       // renderer + this: the object array (views and other objects)
constexpr uint32_t kViewSlotList = 1156;           // view + this + slot * 16: what the view draws

constexpr auto kTickWaitLimit = std::chrono::milliseconds(100);  // 30 fps mode: repeat a frame after this long

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

bool g_fixed = false;  // --kuf_fixed_tick
int g_target = 0;      // --kuf_render_repeat (0 = draw each tick once)
std::filesystem::path g_user_root;
std::atomic<uint8_t*> g_base{nullptr};

// Game thread.
std::atomic<uint64_t> g_ticks{0};     // ticks completed (sub_826F0D58 returned)
std::atomic<uint64_t> g_produced{0};  // ticks handed to the render thread (sub_826F2E10 returned)
std::mutex g_tick_times_mutex;
std::vector<int64_t> g_tick_start_ns;  // tick start times (steady clock, ns), up to kMaxTickTimes
std::vector<int64_t> g_present_ns;     // present times (steady clock, ns), up to kMaxTickTimes * 8
std::vector<int64_t> g_tick_latency_ns;  // per tick: its start to its frame's present (ns)
// With render-only frames, each frame's game time (ticks: N - 1 + alpha between ticks N - 1 and N, N for a frame of
// tick N): its state's age when presented, and per tick from its start to the first frame showing all of it.
std::vector<int64_t> g_state_age_ns, g_full_response_ns;
uint64_t g_next_full_tick = 1;  // render thread: the first tick no presented frame has shown completely
constexpr size_t kMaxTickTimes = 1 << 20;  // about 9.7 hours of ticks
kuf::pacing::TickSchedule g_schedule;  // game thread
Clock::time_point g_tick_began;        // game thread: when the current tick was allowed to start
std::atomic<uint64_t> g_backlog_drops{0};
std::atomic<bool> g_logged_game_thread{false};

// Render thread.
bool g_render_started = false;
uint64_t g_consumed = 0;
bool g_new_tick = true;       // the frame being drawn shows a tick for the first time
bool g_finish_pending = false;
uint32_t g_renderer = 0;
std::vector<uint32_t> g_deferred_view_clears;  // views whose per-slot lists the draw would have cleared
Clock::time_point g_next_frame;
bool g_have_frame_time = false;
std::atomic<uint64_t> g_presents{0}, g_presents_new{0};
// At each frame showing a new tick: how many ticks the game's own counter is ahead of the frames shown (the game
// thread runs up to two ticks ahead; input scripts apply their events at presents, the game reads input at the start
// of a tick, so this lead decides which tick sees a scripted event).
std::array<std::atomic<uint64_t>, 5> g_lead_counts{};
std::atomic<std::thread::id> g_render_thread{};
std::atomic<bool> g_logged_foreign_present{false};
thread_local bool t_finishing = false;  // inside FinishSlot: skip the per-frame calls already made

// The input script's `trace` (K35): the next frame showing a new tick is GPU-traced.
std::atomic<bool> g_trace_requested{false};
std::atomic<uint64_t> g_trace_request_frame{0};

// Render thread, when a frame showing a new tick starts (before any of its draws): point the SDK's GPU trace at it.
void ArmGpuTrace() {
  if (!g_trace_requested.exchange(false)) {
    return;
  }
  const uint64_t swap = kuf::telemetry::FramesPresented();  // swaps before this frame's: its swap number
  const uint64_t tick_frame = kuf::telemetry::TickFramesPresented() + 1;
  const std::string enabled = rex::cvar::GetFlagByName("gpu_trace_frame");
  if (enabled.empty() || enabled.front() == '-') {
    REXLOG_ERROR("[kuf-trace] trace requested at tick {} but the GPU trace is off (--gpu_trace_frame)",
                 g_trace_request_frame.load());
    return;
  }
  if (!rex::cvar::SetFlagByName("gpu_trace_frame", std::to_string(swap))) {
    REXLOG_ERROR("[kuf-trace] could not set gpu_trace_frame to {}", swap);
    return;
  }
  const uint8_t* base = g_base.load();
  REXLOG_INFO("[kuf-trace] GPU trace of tick frame {} (requested at tick {}, game counter {}): swap {}", tick_frame,
              g_trace_request_frame.load(), base ? Load32(base, kGameTickCounter) : 0, swap);
}

// Game / render coordination (new ticks, the freeze diagnostic).
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_hold_game = false;   // freeze: the game thread may not start another tick
bool g_game_parked = false; // ... and it is waiting (or blocked on "free", g_game_blocked)
bool g_game_blocked = false;

// ---- D1 freeze diagnostic ----
struct Region {
  uint64_t guest;  // guest virtual address, or physical offset
  bool physical;
  const uint8_t* host;
  size_t size;
};
struct Snapshot {
  std::vector<Region> regions;
  std::vector<uint8_t> data;  // regions' bytes, in order
};
enum class FreezeState { kIdle, kRequested, kFirstFrame, kRendering };
struct Freeze {
  FreezeState state = FreezeState::kIdle;
  int seconds = 0;
  std::string label;
  uint64_t frame = 0;
  Clock::time_point start, end;
  int captures = 0;
  uint64_t presents_at_start = 0;
  Snapshot before;
};
std::vector<std::string> g_first_frame_clears;  // vectors the first frame emptied (end pointer back to begin)
Freeze g_freeze;  // render thread (RequestFreeze runs on it too: the input script's present hook)
int g_freezes = 0;

constexpr uint64_t kVirtualEnd = 0xA0000000ull;    // 0xA0000000.. are views of physical memory
constexpr uint64_t kPhysicalOffset = 0x100000000ull, kPhysicalSize = 0x20000000ull;
constexpr size_t kPage = 4096;
constexpr size_t kMaxWords = 1000000;

std::vector<Region> GuestRegions(const uint8_t* base) {
  std::vector<Region> out;
  const uintptr_t vlo = reinterpret_cast<uintptr_t>(base), vhi = vlo + kVirtualEnd;
  const uintptr_t plo = vlo + kPhysicalOffset, phi = plo + kPhysicalSize;
  std::ifstream maps("/proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    unsigned long lo = 0, hi = 0;
    char perms[8] = {};
    if (std::sscanf(line.c_str(), "%lx-%lx %7s", &lo, &hi, perms) != 3 || perms[0] != 'r') {
      continue;
    }
    auto add = [&](uintptr_t rlo, uintptr_t rhi, bool physical, uintptr_t origin) {
      const uintptr_t a = std::max<uintptr_t>(lo, rlo), b = std::min<uintptr_t>(hi, rhi);
      if (a < b) {
        out.push_back({a - origin, physical, reinterpret_cast<const uint8_t*>(a), b - a});
      }
    };
    add(vlo, vhi, false, vlo);
    add(plo, phi, true, plo);
  }
  return out;
}

Snapshot Take(const uint8_t* base) {
  Snapshot s;
  s.regions = GuestRegions(base);
  size_t total = 0;
  for (const auto& r : s.regions) total += r.size;
  s.data.resize(total);
  size_t at = 0;
  for (const auto& r : s.regions) {
    std::memcpy(s.data.data() + at, r.host, r.size);
    at += r.size;
  }
  return s;
}

const char* RegionName(bool physical, uint64_t a) {
  if (physical) return "physical";
  if (a < 0x40000000) return "virtual 0x00000000";
  if (a < 0x80000000) return "virtual 0x40000000";
  if (a < 0x90000000) return "image 0x80000000";
  return "image 0x90000000";
}

struct Changes {
  std::map<std::pair<bool, uint64_t>, uint32_t> pages;  // (physical, page) -> bytes changed
  std::map<uint64_t, std::pair<uint32_t, uint32_t>> words;  // virtual word -> (before, after), up to kMaxWords
  uint64_t bytes = 0, compared = 0;
};

// Compares a snapshot with guest memory now, over the pages readable in both.
Changes Diff(const Snapshot& s, const uint8_t* base) {
  Changes c;
  const auto now = GuestRegions(base);
  size_t at = 0;
  for (const auto& r : s.regions) {
    for (const auto& n : now) {
      if (n.physical != r.physical) continue;
      const uint64_t a = std::max(r.guest, n.guest), b = std::min(r.guest + r.size, n.guest + n.size);
      for (uint64_t p = a; p < b; p += kPage) {
        const size_t len = std::min<uint64_t>(kPage, b - p);
        const uint8_t* old = s.data.data() + at + (p - r.guest);
        const uint8_t* cur = r.host + (p - r.guest);
        c.compared += len;
        if (std::memcmp(old, cur, len) == 0) continue;
        uint32_t changed = 0;
        for (size_t i = 0; i < len; ++i) changed += old[i] != cur[i];
        c.pages[{r.physical, p & ~uint64_t(kPage - 1)}] += changed;
        c.bytes += changed;
        if (!r.physical && c.words.size() < kMaxWords) {
          for (size_t i = 0; i + 4 <= len; i += 4) {
            uint32_t o, w;
            std::memcpy(&o, old + i, 4);
            std::memcpy(&w, cur + i, 4);
            if (o != w) c.words[p + i] = {__builtin_bswap32(o), __builtin_bswap32(w)};
          }
        }
      }
    }
    at += r.size;
  }
  return c;
}

void WriteRegions(std::ostream& out, const char* title, const Changes& c) {
  std::map<std::string, uint64_t> by_region;
  for (const auto& [k, n] : c.pages) by_region[RegionName(k.first, k.second)] += n;
  out << title << ": " << c.bytes << " bytes changed (" << c.compared << " compared)\n";
  for (const auto& [name, n] : by_region) out << "  " << name << ": " << n << "\n";
}

void WriteWords(std::ostream& out, const Changes& c, const Changes* exclude, size_t limit) {
  size_t lines = 0;
  for (const auto& [a, v] : c.words) {
    if (exclude && exclude->words.count(a)) continue;
    if (++lines > limit) {
      out << "  ...\n";
      break;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "  %08llx: %08x -> %08x\n", static_cast<unsigned long long>(a), v.first, v.second);
    out << buf;
  }
}

void WriteFreezeReport(const Freeze& f, const Changes* first, const Changes& repeats, const Changes& control,
                       uint64_t frames_drawn) {
  const auto path = g_user_root / ("freeze-" + f.label + ".txt");
  std::ofstream out(path);
  out << "freeze " << f.label << ": game held for " << f.seconds << " s from frame " << f.frame << "; "
      << (first ? "a new tick was taken and drawn once, then " : "the tick on screen was ") << "drawn again "
      << frames_drawn << " times; control: the same time without drawing, the game still held.\n\n";
  if (first) {
    WriteRegions(out, "first frame of the tick (from before it was drawn to after)", *first);
  }
  WriteRegions(out, "repeated frames (from after the first frame to the end)", repeats);
  WriteRegions(out, "control period (nothing drawn)", control);
  if (first) {
    out << "\nrenderer " << std::hex << g_renderer << std::dec << "; vectors emptied by the first frame ("
        << g_first_frame_clears.size() << "):\n";
    for (const auto& line : g_first_frame_clears) out << line << "\n";
    out << "\nvirtual words changed by the first frame and not in the control period:\n";
    WriteWords(out, *first, &control, 20000);
  }
  out << "\nvirtual words changed by the repeated frames and not in the control period:\n";
  WriteWords(out, repeats, &control, 20000);
  out << "\nvirtual words changed in the control period:\n";
  WriteWords(out, control, nullptr, 2000);
  REXLOG_INFO("[kuf-pacing] freeze {}: {} frames of one tick; changed bytes: first frame {}, repeats {}, control {} "
              "-> {}",
              f.label, frames_drawn, first ? int64_t(first->bytes) : -1, repeats.bytes, control.bytes, path.string());
}

Changes g_first_frame;  // the first frame's changes of the current freeze
bool g_have_first_frame = false;

bool Plausible(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000; }

// Vectors (+4 begin, +8 end) the first frame emptied: a changed word whose new value equals the word before it,
// below its old value. Named after the nearest renderer object (renderer + 29272: the object array) when close.
void FindClears(const uint8_t* base) {
  g_first_frame_clears.clear();
  std::vector<std::pair<uint32_t, int>> objects;
  if (g_renderer) {
    const uint32_t array = Load32(base, g_renderer + kRendererObjects);
    for (int i = 0; Plausible(array) && i < 256; ++i) {
      const uint32_t o = Load32(base, array + i * 4);
      if (Plausible(o)) objects.push_back({o, i});
    }
  }
  std::sort(objects.begin(), objects.end());
  for (const auto& [a, v] : g_first_frame.words) {
    const uint32_t old_end = v.first, end = v.second;
    if (a < 4 || end == 0 || end >= old_end || old_end - end > 0x100000 || Load32(base, uint32_t(a - 4)) != end) {
      continue;
    }
    const uint32_t vec = uint32_t(a - 8);
    char buf[160];
    std::string owner = "";
    if (vec >= g_renderer && vec < g_renderer + 0x10000) {
      std::snprintf(buf, sizeof(buf), " = renderer + %u", vec - g_renderer);
      owner = buf;
    } else {
      auto it = std::upper_bound(objects.begin(), objects.end(), std::make_pair(vec, 1 << 30));
      if (it != objects.begin() && vec - std::prev(it)->first < 0x4000) {
        --it;
        std::snprintf(buf, sizeof(buf), " = object[%d] %08x + %u", it->second, it->first, vec - it->first);
        owner = buf;
      }
    }
    // The owning object: the nearest word before the vector that looks like a vtable pointer (0x8200xxxx-0x8210xxxx).
    for (uint32_t back = 0; back <= 0x8000 && owner.empty(); back += 4) {
      const uint32_t w = Load32(base, vec - back);
      if (w >= 0x82000000 && w < 0x82110000 && (w & 3) == 0) {
        std::snprintf(buf, sizeof(buf), " = object with vtable %08x at %08x + %u", w, vec - back, back);
        owner = buf;
      }
    }
    std::snprintf(buf, sizeof(buf), "  vector %08x: %u bytes -> 0%s", vec, old_end - end, owner.c_str());
    g_first_frame_clears.push_back(buf);
  }
}


void EndFreeze(const uint8_t* base) {
  auto& f = g_freeze;
  const uint64_t drawn = g_presents.load() - f.presents_at_start;
  const Changes repeats = Diff(f.before, base);
  f.before = Take(base);
  std::this_thread::sleep_for(std::chrono::seconds(f.seconds));  // control: nothing drawn, the game still held
  const Changes control = Diff(f.before, base);
  f.before = {};
  WriteFreezeReport(f, g_have_first_frame ? &g_first_frame : nullptr, repeats, control, drawn);
  g_first_frame = {};
  {
    std::lock_guard lock(g_mutex);
    g_hold_game = false;
  }
  g_cv.notify_all();
  ++g_freezes;
  f.state = FreezeState::kIdle;
}

void StartRepeats(const uint8_t* base) {
  auto& f = g_freeze;
  f.start = Clock::now();
  f.end = f.start + std::chrono::seconds(f.seconds);
  f.captures = 0;
  f.presents_at_start = g_presents.load();
  f.state = FreezeState::kRendering;
}

// The freeze's steps after it began (BeginFreeze, when the next tick is taken).
void FreezeStep(const uint8_t* base) {
  auto& f = g_freeze;
  if (f.state == FreezeState::kFirstFrame) {
    g_first_frame = Diff(f.before, base);
    g_have_first_frame = true;
    FindClears(base);
    f.before = Take(base);
    StartRepeats(base);
  }
  if (f.state != FreezeState::kRendering) {
    return;
  }
  const auto now = Clock::now();
  static constexpr double kCaptureAt[] = {0.2, 0.5, 0.9};
  while (f.captures < 3 && now >= f.start + std::chrono::duration_cast<Clock::duration>(
                                                   std::chrono::duration<double>(kCaptureAt[f.captures] * f.seconds))) {
    kuf::capture::RequestLabeled(f.frame, f.label + "_" + std::to_string(++f.captures));
  }
  if (now >= f.end) {
    EndFreeze(base);
  }
}

// ---- pacing ----

// Game thread, after its own wait for "free": sleep until this tick's time.
void PaceGameTick() {
  std::this_thread::sleep_until(g_schedule.Next(Clock::now()));
  if (REXCVAR_GET(kuf_debug_tick_stall_at) >= 0 && g_ticks.load() + 1 == uint64_t(REXCVAR_GET(kuf_debug_tick_stall_at))) {
    REXLOG_INFO("[kuf-clock] test stall: tick {} held {} us", g_ticks.load() + 1, REXCVAR_GET(kuf_debug_tick_stall_us));
    std::this_thread::sleep_for(std::chrono::microseconds(REXCVAR_GET(kuf_debug_tick_stall_us)));
  }
  g_backlog_drops.store(g_schedule.drops(), std::memory_order_relaxed);
  std::unique_lock lock(g_mutex);
  if (g_hold_game) {
    g_game_parked = true;
    g_cv.notify_all();
    g_cv.wait(lock, [] { return !g_hold_game; });
    g_game_parked = false;
    g_schedule.Restart(Clock::now());
  }
  g_tick_began = Clock::now();
}

// Render thread: wait for the next frame's time.
void PaceFrame() {
  const bool freezing = g_freeze.state == FreezeState::kRendering;
  if (g_target < 0 && !freezing) {
    return;  // unlimited
  }
  if (g_target <= 30 && !freezing) {
    // One frame per tick: wait for it (as the original does); during a long tick (loading) draw again every 100 ms.
    std::unique_lock lock(g_mutex);
    g_cv.wait_for(lock, kTickWaitLimit, [] { return g_produced.load() > g_consumed; });
    return;
  }
  const auto period = std::chrono::nanoseconds(1000000000 / std::max(g_target, 60));
  const auto now = Clock::now();
  if (!g_have_frame_time || now - g_next_frame > period) {
    g_next_frame = now;  // first frame, or more than a frame late: no catching up
    g_have_frame_time = true;
    return;
  }
  g_next_frame += period;
  std::this_thread::sleep_until(g_next_frame);
}

// The deferred rest of the end of the last frame of a tick: the original sub_821A3BA0 without its per-frame calls.
void FinishSlot(PPCContext& ctx, uint8_t* base) {
  if (!g_finish_pending) {
    return;
  }
  g_finish_pending = false;
  t_finishing = true;
  for (const uint32_t view : g_deferred_view_clears) {
    ctx.r3.u64 = view;
    __imp__sub_821EA3A0(ctx, base);
  }
  g_deferred_view_clears.clear();
  ctx.r3.u64 = g_renderer;
  __imp__sub_821A3BA0(ctx, base);
  t_finishing = false;
}

REX_EXTERN(__imp__sub_826F2EA0);
// Moves the render thread on to the next tick (one is ready): finishes the old slot, releases "free" for it and
// takes the new tick through the game's own wait, which does not block.
void TakeTick(PPCContext& ctx, uint8_t* base, uint32_t sync) {
  FinishSlot(ctx, base);
  ctx.r3.u64 = sync;
  __imp__sub_826F2E40(ctx, base);
  ctx.r3.u64 = sync;
  __imp__sub_826F2EA0(ctx, base);
  ++g_consumed;
  g_new_tick = true;
  ArmGpuTrace();
}

// A freeze begins when the next tick is taken, so that tick's first frame is measured too.
void BeginFreeze(PPCContext& ctx, uint8_t* base, uint32_t sync) {
  auto& f = g_freeze;
  {
    std::unique_lock lock(g_mutex);
    g_hold_game = true;
    if (!g_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_game_parked || g_game_blocked; })) {
      g_hold_game = false;
      g_cv.notify_all();
      REXLOG_ERROR("[kuf-pacing] freeze {}: the game thread did not stop; freeze cancelled", f.label);
      f.state = FreezeState::kIdle;
      TakeTick(ctx, base, sync);
      return;
    }
  }
  // The game thread is blocked on "free" (or parked); the tick it finished is taken now, and once "free" is released
  // it can only reach the hold in PaceGameTick, never another tick.
  TakeTick(ctx, base, sync);
  REXLOG_INFO("[kuf-pacing] freeze {}: game held, tick {} taken (game counter {}); drawing it for {} s", f.label,
              g_consumed, Load32(base, kGameTickCounter), f.seconds);
  f.before = Take(base);
  g_have_first_frame = false;
  f.state = FreezeState::kFirstFrame;
}

// --kuf_trace_render_lists: the renderer's slot list (renderer[(slot + 6782) * 4]) and each view's per-slot draw
// list (views: indices at renderer + 26548 into the object array at renderer + 29272; list: view + 1156 + slot * 16),
// as element counts (vectors: +4 begin, +8 end).
int g_list_frames = 0;
uint32_t VectorCount(const uint8_t* base, uint32_t vec, uint32_t element) {
  if (!Plausible(vec)) return 0xFFFFFFFF;
  const uint32_t b = Load32(base, vec + 4), e = Load32(base, vec + 8);
  return b && e >= b ? (e - b) / element : 0;
}
void TraceLists(const uint8_t* base, const char* when) {
  const int from = REXCVAR_GET(kuf_trace_render_lists);
  if (from <= 0 || !g_renderer || Load32(base, kGameTickCounter) < uint32_t(from) || g_list_frames >= 96) {
    return;
  }
  ++g_list_frames;
  const uint32_t r = g_renderer, slot = Load32(base, kRenderSlotIndex);
  const uint32_t objects = Load32(base, r + kRendererObjects);
  const uint32_t vb = Load32(base, r + kRendererViews + 4), ve = Load32(base, r + kRendererViews + 8);
  std::string out;
  for (uint32_t sl = 0; sl < 2; ++sl) {
    out += " slot" + std::to_string(sl) + ": list " +
           std::to_string(VectorCount(base, Load32(base, r + kRendererSlotList + sl * 4), 4)) + " views";
    for (uint32_t v = vb; Plausible(vb) && v < ve && v < vb + 8 * 4; v += 4) {
      const uint32_t index = Load32(base, v);
      const uint32_t view = Plausible(objects) && index < 0x10000 ? Load32(base, objects + index * 4) : 0;
      out += " " + (Plausible(view) ? std::to_string(VectorCount(base, view + kViewSlotList + sl * 16, 4))
                                    : std::string("-"));
    }
  }
  REXLOG_INFO("[kuf-pacing] lists {} ({} frame, render slot {}, tick {}):{}", when, g_new_tick ? "new" : "repeat", slot,
              Load32(base, kGameTickCounter), out);
}

// --kuf_trace_render_lists with --kuf_render_builds: the slot containers a frame consumes, for both slots, at the
// render slot build (game thread: "tick" or "build") and at the frame's end (render thread): the 2D list at renderer
// + 28976 + slot * 16 (sub_821ABE90 / sub_821ABF50), the slot's draw list (its frame packet's object list), each
// view's per-slot list, the per-slot vectors at renderer + 29276 (consumed by sub_821F49E8) and + 29444 (28-byte
// entries, consumed by the frame end's sub_821AA7A0), and the frame-memory lists at renderer + 8 + slot * 48 and
// + 29308 + slot * 48 (sub_821B03D8, sub_821B0648 at the frame end).
void TraceBuildLists(const uint8_t* base, uint32_t r, const char* when, const char* kind) {
  const int from = REXCVAR_GET(kuf_trace_render_lists);
  if (from <= 0 || REXCVAR_GET(kuf_render_builds) <= 0 || Load32(base, kGameTickCounter) < uint32_t(from) ||
      g_list_frames >= 96) {
    return;
  }
  ++g_list_frames;
  const uint32_t objects = Load32(base, r + kRendererObjects);
  const uint32_t vb = Load32(base, r + kRendererViews + 4), ve = Load32(base, r + kRendererViews + 8);
  std::string out;
  for (uint32_t sl = 0; sl < 2; ++sl) {
    out += fmt::format(" slot{}: hud {} list {} queued {} commands {} release {} {} views", sl,
                       VectorCount(base, r + 28976 + sl * 16, 4),
                       VectorCount(base, Load32(base, r + kRendererSlotList + sl * 4), 4),
                       VectorCount(base, r + 29276 + sl * 16, 4), VectorCount(base, r + 29444 + sl * 16, 28),
                       VectorCount(base, r + 8 + sl * 48 + 4, 4), VectorCount(base, r + 29308 + sl * 48 + 4, 4));
    for (uint32_t v = vb; Plausible(vb) && v < ve && v < vb + 8 * 4; v += 4) {
      const uint32_t index = Load32(base, v);
      const uint32_t view = Plausible(objects) && index < 0x10000 ? Load32(base, objects + index * 4) : 0;
      out += " " + (Plausible(view) ? std::to_string(VectorCount(base, view + kViewSlotList + sl * 16, 4))
                                    : std::string("-"));
    }
  }
  REXLOG_INFO("[kuf-pacing] lists {} {} (game slot {}, render slot {}, tick {}):{}", when, kind,
              Load32(base, kGameSlotIndex), Load32(base, kRenderSlotIndex), Load32(base, kGameTickCounter), out);
}

// --kuf_trace_render_lists with --kuf_render_builds: in that tick, who writes the game slot's frame packet (renderer
// + 26584 + slot * 272, the slot list's target): the guest call chains (LR, then the r1 back chain; Xbox 360 ABI: the
// caller's LR at back chain - 8) of sub_821A2CA0 (an object added to the packet), sub_821A3390 (a record queued in
// one of its 16 buckets) and sub_821ABE90 (an item added to the 2D list), with counts, logged at the tick's end.
std::map<std::string, int> g_packet_writers;  // game thread
bool g_packet_writers_logged = false;
void NotePacketWriter(const uint8_t* base, const PPCContext& ctx, const char* what) {
  const int from = REXCVAR_GET(kuf_trace_render_lists);
  if (from <= 0 || g_packet_writers_logged || REXCVAR_GET(kuf_render_builds) <= 0 ||
      Load32(base, kGameTickCounter) != uint32_t(from)) {
    return;
  }
  std::string chain = fmt::format("{} {:08X}", what, uint32_t(ctx.lr));
  // The outermost back chain points past the stack's mapped end: read with process_vm_readv, which fails there.
  const auto safe_load32 = [base](uint32_t a, uint32_t* out) {
    uint32_t raw = 0;
    iovec local{&raw, sizeof(raw)};
    iovec remote{const_cast<uint8_t*>(base) + a, sizeof(raw)};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != sizeof(raw)) return false;
    *out = __builtin_bswap32(raw);
    return true;
  };
  uint32_t sp = ctx.r1.u32, back = 0, lr = 0;
  for (int frame = 0; frame < 10; ++frame) {
    if (!safe_load32(sp, &back) || back <= sp || !safe_load32(back - 8, &lr)) break;
    chain += fmt::format(" {:08X}", lr);
    sp = back;
  }
  ++g_packet_writers[chain];
}
void LogPacketWriters(const uint8_t* base) {
  if (g_packet_writers.empty() || g_packet_writers_logged) return;
  g_packet_writers_logged = true;
  for (const auto& [chain, count] : g_packet_writers) {
    REXLOG_INFO("[kuf-pacing] packet writer x{}: {}", count, chain);
  }
}

// ---- render-only frames (D3b-1: --kuf_frame_rate; the D3a diagnostic --kuf_render_builds) ----
// A tick ends with three calls that build its render slot from the game's state and hand it on: sub_821F49E8 (the
// slot's queued work and draw lists), sub_821F4D00 (each view's matrices and lists, the frame's lighting and shadow
// setup) and sub_821F4CD8 (the flip of the game's slot index 0x82AE760C). A render-only frame runs the first two again
// on the game thread between ticks, for the slot the tick wrote (both slot indices set back to it), while the game
// thread holds both "free" slots (the render thread has drawn every slot it was given and waits), and hands the slot on
// as the tick does; the renderer draws it once, as it draws a tick. render_frames.cpp makes it the tick's frame again
// and keeps the game out of it. The frames follow a presentation clock at --kuf_frame_rate (the tick's own frame takes
// one of its steps); a frame is drawn only if the render thread has finished the one before and the build can finish
// before the next tick is due, else it is dropped: frames, never ticks. --kuf_render_builds=n instead builds n frames
// per tick, spaced evenly; with it the input script's freeze holds the ticks and keeps building, and compares guest
// memory with a control period.
int g_builds_per_tick = 0;         // --kuf_render_builds
int g_frame_rate = 0;              // --kuf_frame_rate: 0 = off, -1 = as many frames as fit
int g_free_held = 0;               // game thread: "free" slots it holds
bool g_tick_marked_world = false;  // the last tick called sub_821A2E70 (not while the game is paused)
uint32_t g_world = 0;              // the tick's argument
thread_local bool t_building = false;
std::mutex g_kinds_mutex;
struct SlotKind {
  bool tick = true;   // a tick's frame, else a render-only build's
  double shown = 0;   // its game time (ticks)
  double effect_lag = 0;  // how far its effect clock is behind the tick's (ticks: kuf::presentation::EffectLag)
  uint64_t ticks = 0;     // ticks completed when it was handed over (its tick's number)
};
std::deque<SlotKind> g_slot_kinds;  // slots handed to the render thread, in order
double g_tick_frame_shown = 0;      // game thread: the game time of the tick's own frame (its scene build)
double g_tick_frame_effect_lag = 0;  // game thread: its effect clock's lag
std::atomic<double> g_render_effect_lag{0};  // render thread: the effect clock's lag of the frame being drawn
std::atomic<uint64_t> g_render_ticks{0};       // render thread: the tick number of the frame being drawn
double g_render_shown = 0;          // render thread: the game time of the frame being drawn
std::atomic<uint64_t> g_builds{0};
std::atomic<uint64_t> g_builds_late{0};  // frames dropped: no time left before the next tick
std::atomic<uint64_t> g_builds_busy{0};  // frames dropped: the render thread still drawing the frame before
// --kuf_frame_rate: frames per tick interval (the tick's own included; the last bucket: that many or more), ticks with a
// frame step dropped as late, and the presentation clock's restarts at a tick's start.
constexpr int kFramesPerTickBuckets = 10;
std::atomic<uint64_t> g_frames_per_tick[kFramesPerTickBuckets] = {};
std::atomic<uint64_t> g_late_ticks{0}, g_clock_restarts{0};
std::atomic<uint64_t> g_rand_in_builds{0}, g_rand_in_ticks{0};    // CRT rand() calls (sub_827FACD0)
std::atomic<uint64_t> g_physx_in_builds{0}, g_physx_in_ticks{0};  // PhysX simulate calls (sub_82230E00)
// Slots handed to the render thread and slots it has finished (released "free"): equal = the render thread waits.
std::atomic<uint64_t> g_frames_handed{0}, g_frames_done{0};
std::mutex g_idle_mutex;
std::condition_variable g_idle_cv;
Clock::time_point g_present_clock;  // game thread: the next step of the presentation clock (--kuf_frame_rate)
double g_build_ms_estimate = 4.0;   // game thread: a build's cost, followed by its last measurements
struct BuildFreeze {
  bool requested = false;
  int seconds = 0;
  std::string label;
  uint64_t frame = 0;
};
BuildFreeze g_build_freeze;  // g_mutex

bool Building() { return g_builds_per_tick > 0 || g_frame_rate != 0; }

// M8 adaptive frame rate (--kuf_frame_rate_adaptive). When the GPU cannot draw the limit's frames, the render thread
// waits for ring space, steps are dropped as "render busy" and the frames that are drawn come in bursts (at 3840x2160
// with a 120 limit: alternately about 5 and 21 ms apart), so the shown motion advances unevenly. Instead the frames
// per tick follow the GPU's measured time per frame (SDK patch 0012), on the tick's schedule: an even 60 or 90
// (adaptive_rate.h: down after half a second below the rate in effect, up one step after two seconds above it).
kuf::pacing::AdaptiveRate g_adaptive;  // game thread
int EffectiveFrameRate() {
  if (!REXCVAR_GET(kuf_frame_rate_adaptive) || g_frame_rate <= 30 || g_frame_rate % 30 != 0) {
    g_adaptive.Reset();
    return g_frame_rate;
  }
  // The GPU plugin's measurement (SDK patch 0012, --gpu_timestamp_stats), through the cvar registry: the plugin is
  // loaded at run time, its functions are not on this executable's link line.
  const double gpu_ms = rex::cvar::Query<int32_t>("gpu_recent_frame_us") / 1000.0;
  const int before = g_adaptive.n() > 0 ? g_adaptive.n() : g_frame_rate / 30;
  const int n = g_adaptive.Update(g_frame_rate / 30, gpu_ms,
                                  kuf::pacing::TickSchedule::kTickNs / 1e6 * REXCVAR_GET(kuf_frame_rate_adaptive_load));
  if (n != before) {
    REXLOG_INFO("[kuf-pacing] adaptive frame rate: {} -> {} frames per second (GPU {:.2f} ms per frame, limit {})",
                before * 30, n * 30, gpu_ms, g_frame_rate);
  }
  return n * 30;
}

// The frame-rate settings (D5): g_frame_rate from the setting's limit (30: the original, 0: no render-only frames;
// unlimited: -1), and what is interpolated.
int RateOf(int fps_limit) {
  return fps_limit == kuf::pc::kFpsOriginal ? 0 : fps_limit == kuf::pc::kFpsUnlimited ? -1 : fps_limit;
}
kuf::presentation::Interpolation InterpolationOf(const kuf::pc::FrameRateSettings& f) {
  return {f.interpolation, f.objects, f.animation, f.effects};
}
std::string RateName(int rate) { return rate == 0 ? "30 (original)" : rate < 0 ? "unlimited" : std::to_string(rate); }
kuf::presentation::Interpolation g_interpolation;  // game thread
kuf::pc::FrameRateSettings g_active_frame;         // game thread: the settings in effect
std::mutex g_request_mutex;
kuf::pc::FrameRateSettings g_requested_frame;      // g_request_mutex
std::atomic<bool> g_frame_requested{false};
std::atomic<uint64_t> g_frame_switches{0};

void PushSlotKind(bool tick, double shown, double effect_lag) {
  std::lock_guard lock(g_kinds_mutex);
  g_slot_kinds.push_back({tick, shown, effect_lag, g_ticks.load(std::memory_order_relaxed)});
}

bool PopSlotKind() {
  std::lock_guard lock(g_kinds_mutex);
  if (g_slot_kinds.empty()) return true;
  const SlotKind kind = g_slot_kinds.front();
  g_slot_kinds.pop_front();
  g_render_shown = kind.shown;
  g_render_effect_lag.store(kind.effect_lag, std::memory_order_relaxed);
  g_render_ticks.store(kind.ticks, std::memory_order_relaxed);
  return kind.tick;
}

// The real time (steady clock, ns) of game time `shown` (ticks): between the starts of the two ticks around it.
// g_tick_times_mutex held. 0 if not known.
int64_t TimeOfGameTime(double shown) {
  const double whole = std::floor(shown);
  const uint64_t n = uint64_t(std::max(whole, 0.0));
  if (n < 1 || n > g_tick_start_ns.size()) return 0;
  const int64_t at = g_tick_start_ns[n - 1];
  const double fraction = shown - whole;
  if (fraction <= 0 || n >= g_tick_start_ns.size()) return at;
  return at + int64_t(fraction * double(g_tick_start_ns[n] - at));
}

// Render thread, after a present of a frame showing game time `shown`.
void NoteShown(double shown, int64_t now) {
  std::lock_guard lock(g_tick_times_mutex);
  const int64_t at = TimeOfGameTime(shown);
  if (at && g_state_age_ns.size() < kMaxTickTimes * 8) g_state_age_ns.push_back(now - at);
  for (; double(g_next_full_tick) <= shown && g_next_full_tick <= g_tick_start_ns.size(); ++g_next_full_tick) {
    if (g_full_response_ns.size() < kMaxTickTimes) {
      g_full_response_ns.push_back(now - g_tick_start_ns[g_next_full_tick - 1]);
    }
  }
}

void HoldFree(PPCContext& ctx, uint8_t* base, uint32_t sync) {
  ctx.r3.u64 = sync;
  __imp__sub_826F2DB8(ctx, base);
  ++g_free_held;
}

// Both "free" slots: everything handed to the render thread has been drawn.
void HoldBoth(PPCContext& ctx, uint8_t* base, uint32_t sync) {
  while (g_free_held < 2) HoldFree(ctx, base, sync);
}

void ReturnFree(PPCContext& ctx, uint8_t* base, uint32_t sync) {
  for (; g_free_held > 0; --g_free_held) {
    ctx.r3.u64 = sync;
    __imp__sub_826F2E40(ctx, base);
  }
}

// Game thread: until the render thread has finished every slot handed to it, or `deadline`.
bool WaitRenderIdle(Clock::time_point deadline) {
  std::unique_lock lock(g_idle_mutex);
  return g_idle_cv.wait_until(lock, deadline, [] { return g_frames_done.load() >= g_frames_handed.load(); });
}

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

// The presentation interpolation factor of a frame whose scene build starts now and takes about `build_ms`
// (presentation.h): the fraction of a tick from the current tick's start to when the frame is handed to the render
// thread, at most 1. (The tick's own scene build takes about 2.6 ms, a render-only one about 0.9 ms: frames are
// placed by when they reach the renderer, not by when their build starts.)
// Never before the tick's previous frame: the tick's own frame is placed with its scene build's estimate (2.6 ms),
// the render-only frame right after with a build's (0.9 ms), and if the tick's scene was quicker than its estimate the
// next frame's hand-off would be placed earlier than the frame handed before it (seen once in 32,000 frames, 0.0055
// of a tick; D4b-4's effect check).
Clock::time_point g_alpha_tick;  // game thread: the tick g_alpha_floor belongs to (its g_tick_began)
double g_alpha_floor = 0.0;
std::atomic<uint64_t> g_alpha_held{0};  // frames placed at the floor instead of before it
double FrameAlpha(double build_ms) {
  const double since = std::chrono::duration<double>(Clock::now() - g_tick_began).count() + build_ms / 1000.0;
  double alpha = kuf::presentation::Alpha(since, kuf::pacing::TickSchedule::kTickNs / 1e9);
  if (g_alpha_tick != g_tick_began) {
    g_alpha_tick = g_tick_began;
    g_alpha_floor = 0.0;
  }
  if (alpha < g_alpha_floor) {
    alpha = g_alpha_floor;
    g_alpha_held.fetch_add(1, std::memory_order_relaxed);
  }
  g_alpha_floor = alpha;
  return alpha;
}
double g_tick_scene_ms_estimate = 2.6;  // game thread: the tick's own scene build, followed by its measurements

// Build costs (game thread), logged every 900 builds: a build's scene build, the tick's own, the whole tick.
std::vector<double> g_cost_build_ms, g_cost_tick_scene_ms, g_cost_tick_ms;
Clock::time_point g_tick_scene_began;

void LogCosts() {
  auto stats = [](std::vector<double> v) {
    if (v.empty()) return std::string("-");
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    return fmt::format("mean {:.2f} p50 {:.2f} p95 {:.2f} max {:.2f} ms", sum / v.size(), v[v.size() / 2],
                       v[std::min(v.size() - 1, v.size() * 95 / 100)], v.back());
  };
  REXLOG_INFO("[kuf-pacing] build costs over {} builds: render-only build (scene build) {}; the tick's scene build {}; "
              "the whole tick {}; restored vectors {}, failed {}, records dropped {}; rand() in builds {}, in ticks {}; PhysX simulate in "
              "builds {}, in ticks {}; frames dropped: late {}, render busy {}",
              g_cost_build_ms.size(), stats(g_cost_build_ms), stats(g_cost_tick_scene_ms), stats(g_cost_tick_ms),
              kuf::render_frames::RestoredVectors(), kuf::render_frames::RestoreFailures(),
              kuf::render_frames::RestoreDropped(), g_rand_in_builds.load(),
              g_rand_in_ticks.load(), g_physx_in_builds.load(), g_physx_in_ticks.load(), g_builds_late.load(),
              g_builds_busy.load());
  g_cost_build_ms.clear();
  g_cost_tick_scene_ms.clear();
  g_cost_tick_ms.clear();
}

// One render-only frame (both "free" slots held, the render thread waiting): built and handed to the render thread.
// `alpha`: its presentation interpolation factor (NaN: from the time it is built).
bool BuildOnce(PPCContext& ctx, uint8_t* base, uint32_t sync, double alpha = NAN) {
  const uint32_t renderer = Load32(base, kRendererPointer);
  if (!renderer || Load32(base, sync + kSyncEnabled) == 0 || g_free_held < 2) {
    return false;
  }
  const uint32_t slot = Load32(base, kGameSlotIndex) ^ 1;  // the slot the last tick wrote
  Store32(base, kGameSlotIndex, slot);
  Store32(base, kRenderSlotIndex, slot);
  const auto began = Clock::now();
  t_building = true;
  const auto state = kuf::render_frames::BeginBuild(ctx, base, renderer, g_tick_marked_world);
  double shown = kuf::presentation::OnBuildFrame(base, renderer,
                                                 std::isnan(alpha) ? FrameAlpha(g_build_ms_estimate) : alpha);
  const double effect_lag = std::isnan(shown) ? 0.0 : kuf::presentation::EffectLag();
  if (std::isnan(shown)) shown = double(g_ticks.load());  // the tick's frame again
  TraceBuildLists(base, renderer, "slot build in", "build");
  ctx.r3.u64 = renderer;
  __imp__sub_821F49E8(ctx, base);
  ctx.r3.u64 = renderer;
  __imp__sub_821F4D00(ctx, base);
  kuf::render_frames::EndBuild(base, state);
  TraceBuildLists(base, renderer, "views set up", "build");
  __imp__sub_821F4CD8(ctx, base);
  t_building = false;
  const double ms = Ms(Clock::now() - began);
  g_build_ms_estimate = 0.9 * g_build_ms_estimate + 0.1 * ms;
  g_cost_build_ms.push_back(ms);
  if (g_cost_build_ms.size() >= 900) LogCosts();
  PushSlotKind(false, shown, effect_lag);
  --g_free_held;
  g_frames_handed.fetch_add(1);
  ctx.r3.u64 = sync;
  __imp__sub_826F2E10(ctx, base);
  g_builds.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void WriteBuildFreezeReport(const BuildFreeze& f, const Changes& builds, const Changes& control, uint64_t built,
                            uint64_t presented) {
  const auto path = g_user_root / ("freeze-" + f.label + ".txt");
  std::ofstream out(path);
  out << "freeze " << f.label << " (render-only builds): game "
      << "held for " << f.seconds << " s from frame " << f.frame << "; " << built << " render-only builds, "
      << presented << " frames presented; control: the same time with nothing built or drawn.\n\n";
  WriteRegions(out, "render-only builds and their frames", builds);
  WriteRegions(out, "control period (nothing built or drawn)", control);
  out << "\nvirtual words changed by the builds and not in the control period:\n";
  WriteWords(out, builds, &control, 50000);
  out << "\nvirtual words changed in the control period:\n";
  WriteWords(out, control, nullptr, 2000);
  REXLOG_INFO("[kuf-pacing] freeze {}: {} render-only builds, {} frames; changed bytes: builds {}, control {} -> {}",
              f.label, built, presented, builds.bytes, control.bytes, path.string());
}

// The freeze with render-only builds (game thread, after a tick was handed on): holds the ticks for f.seconds while
// building at the configured rate, then the control period.
void RunBuildFreeze(PPCContext& ctx, uint8_t* base, uint32_t sync, const BuildFreeze& f) {
  HoldBoth(ctx, base, sync);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the render thread back in its wait
  REXLOG_INFO("[kuf-pacing] freeze {}: game held at game counter {}; render-only builds for {} s", f.label,
              Load32(base, kGameTickCounter), f.seconds);
  kuf::capture::RequestLabeled(f.frame, f.label + "_tick");    // the last tick's frame, still on screen
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  if (REXCVAR_GET(kuf_freeze_alpha_sweep) && kuf::presentation::Enabled()) {
    // The interval between the last two ticks, drawn at fixed points and captured (each frame on screen while the
    // capture reads it: the game is held and nothing else is drawn).
    for (const int percent : {0, 25, 50, 75, 100}) {
      HoldBoth(ctx, base, sync);
      if (!BuildOnce(ctx, base, sync, percent / 100.0)) break;
      HoldBoth(ctx, base, sync);  // drawn
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      kuf::capture::RequestLabeled(f.frame, fmt::format("{}_a{:03d}", f.label, percent));
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    // The world objects alone (D4a): any change between these frames is theirs.
    kuf::presentation::SetObjectsOnly(true);
    for (const int percent : {0, 25, 50, 75, 100}) {
      HoldBoth(ctx, base, sync);
      if (!BuildOnce(ctx, base, sync, percent / 100.0)) break;
      HoldBoth(ctx, base, sync);
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      kuf::capture::RequestLabeled(f.frame, fmt::format("{}_o{:03d}", f.label, percent));
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    kuf::presentation::SetObjectsOnly(false);
    // The skeletal poses alone (D4b).
    kuf::presentation::SetBonesOnly(true);
    for (const int percent : {0, 25, 50, 75, 100}) {
      HoldBoth(ctx, base, sync);
      if (!BuildOnce(ctx, base, sync, percent / 100.0)) break;
      HoldBoth(ctx, base, sync);
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      kuf::capture::RequestLabeled(f.frame, fmt::format("{}_b{:03d}", f.label, percent));
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    kuf::presentation::SetBonesOnly(false);
    // The effect clock alone (D4b-4).
    kuf::presentation::SetEffectsOnly(true);
    for (const int percent : {0, 25, 50, 75, 100}) {
      HoldBoth(ctx, base, sync);
      if (!BuildOnce(ctx, base, sync, percent / 100.0)) break;
      HoldBoth(ctx, base, sync);
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      kuf::capture::RequestLabeled(f.frame, fmt::format("{}_e{:03d}", f.label, percent));
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    kuf::presentation::SetEffectsOnly(false);
  }
  Snapshot before = Take(base);
  const uint64_t presents0 = g_presents.load(), builds0 = g_builds.load();
  const uint64_t rand0 = g_rand_in_builds.load(), physx0 = g_physx_in_builds.load();
  const int per_tick = g_builds_per_tick > 0 ? g_builds_per_tick : std::max(1, g_frame_rate / 30 - 1);
  const auto period = std::chrono::nanoseconds(kuf::pacing::TickSchedule::kTickNs / (per_tick + 1));
  const auto start = Clock::now(), end = start + std::chrono::seconds(f.seconds);
  static constexpr double kCaptureAt[] = {0.2, 0.5, 0.9};
  int captures = 0;
  const auto capture_time = [&](int i) {
    const std::chrono::duration<double> after(kCaptureAt[i] * f.seconds);
    return start + std::chrono::duration_cast<Clock::duration>(after);
  };
  for (auto next = start; Clock::now() < end; next += period) {
    HoldBoth(ctx, base, sync);  // the frame before drawn
    if (!BuildOnce(ctx, base, sync)) break;
    while (captures < 3 && Clock::now() >= capture_time(captures)) {
      kuf::capture::RequestLabeled(f.frame, f.label + "_" + std::to_string(++captures));
    }
    std::this_thread::sleep_until(next + period);
  }
  HoldBoth(ctx, base, sync);
  const uint64_t built = g_builds.load() - builds0, presented = g_presents.load() - presents0;
  const Changes builds = Diff(before, base);
  before = {};  // released before the next copy: two copies of guest memory at once took a test run to 9.8 GB
  before = Take(base);
  std::this_thread::sleep_for(std::chrono::seconds(f.seconds));
  const Changes control = Diff(before, base);
  before = {};
  WriteBuildFreezeReport(f, builds, control, built, presented);
  REXLOG_INFO("[kuf-pacing] freeze {}: rand() calls in the builds {} (in all game ticks so far {}); PhysX simulate "
              "in the builds {} (in all game ticks so far {})",
              f.label, g_rand_in_builds.load() - rand0, g_rand_in_ticks.load(), g_physx_in_builds.load() - physx0,
              g_physx_in_ticks.load());
  ++g_freezes;
  g_schedule.Restart(Clock::now());
  g_present_clock = Clock::now();
  ReturnFree(ctx, base, sync);
}

// Game thread, right after a tick was handed to the render thread (the slots synchronised): a frame-rate setting
// changed in the menu takes effect here (D5, docs/HIGH_FPS.md 21). With both "free" slots held the render thread has
// drawn everything handed to it and waits; then every value written for presented frames goes back to the game's own
// in both slots (presentation::Reset), and when render-only frames stop, both slots' kept releases run (as the next
// ticks into them would have, render_frames.cpp); the SDK's vblank mode follows (render-only frames need its 1000 Hz
// vblanks, the original frame rate its 60 Hz ones). Returns true if the settings changed (this tick then builds no
// render-only frames: those start from the next tick's own scene build, which saves what they need).
bool ApplyFrameRequest(PPCContext& ctx, uint8_t* base, uint32_t sync) {
  if (!g_frame_requested.exchange(false)) return false;
  kuf::pc::FrameRateSettings want;
  {
    std::lock_guard lock(g_request_mutex);
    want = g_requested_frame;
  }
  const int rate = RateOf(want.fps_limit);
  const auto what = InterpolationOf(want);
  if (rate == g_frame_rate && what == g_interpolation) {
    g_active_frame = want;
    return false;
  }
  if (g_builds_per_tick != 0 || g_target != 0 || !g_fixed) {
    REXLOG_WARN("[kuf-pacing] frame rate not changed: a diagnostic mode (--kuf_render_builds, --kuf_render_repeat) or "
                "the original pacing (--kuf_fixed_tick=false) is in use");
    return false;
  }
  const auto began = Clock::now();
  HoldBoth(ctx, base, sync);
  const int old_rate = g_frame_rate;
  const bool was = Building();
  kuf::presentation::Reset(base);
  if (was && rate == 0) {
    kuf::render_frames::OnTickStart(ctx, base, 0);
    kuf::render_frames::OnTickStart(ctx, base, 1);
  }
  g_frame_rate = rate;
  g_adaptive.Reset();  // M8: the adaptive rate starts again from the new limit
  g_interpolation = what;
  g_active_frame = want;
  kuf::presentation::Configure(Building(), what);
  if (was != Building()) rex::cvar::SetFlagByName("vsync", Building() ? "false" : "true");
  g_present_clock = {};  // restarts from the next tick's start
  ReturnFree(ctx, base, sync);
  g_frame_switches.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("[kuf-pacing] tick {}: frame rate {} -> {}, interpolation {} objects {} animation {} effects {} ({:.2f} ms; "
              "presents {}, guest vblank {})",
              g_ticks.load(), RateName(old_rate), RateName(rate), what.camera_player, what.objects, what.bones,
              what.effects, Ms(Clock::now() - began), g_presents.load(),
              rex::cvar::Query<bool>("vsync") ? "60 Hz" : "1000 Hz");
  return true;
}

// After a tick was handed to the render thread: the freeze, or this tick's render-only frames.
void AfterTick(PPCContext& ctx, uint8_t* base, uint32_t sync) {
  BuildFreeze freeze;
  {
    std::lock_guard lock(g_mutex);
    std::swap(freeze, g_build_freeze);
  }
  if (freeze.requested) {
    RunBuildFreeze(ctx, base, sync, freeze);
    return;
  }
  const auto due = g_schedule.Following();  // the next tick
  const auto margin = std::chrono::microseconds(500);
  const auto cost = std::chrono::microseconds(int64_t(g_build_ms_estimate * 1000.0)) + margin;
  if (g_builds_per_tick > 0) {
    // The D3a diagnostic: n frames per tick, spaced evenly.
    const auto period = std::chrono::nanoseconds(kuf::pacing::TickSchedule::kTickNs) / (g_builds_per_tick + 1);
    for (int k = 1; k <= g_builds_per_tick; ++k) {
      const auto at = g_tick_began + k * period;
      if (std::max(at, Clock::now()) + cost > due) {
        g_builds_late.fetch_add(g_builds_per_tick - k + 1, std::memory_order_relaxed);
        break;
      }
      std::this_thread::sleep_until(at);
      if (!WaitRenderIdle(due - cost)) {
        g_builds_busy.fetch_add(g_builds_per_tick - k + 1, std::memory_order_relaxed);
        break;
      }
      HoldBoth(ctx, base, sync);
      if (!BuildOnce(ctx, base, sync)) break;
    }
    ReturnFree(ctx, base, sync);
    return;
  }
  if (g_frame_rate < 0) {
    // As many frames as fit: each as soon as the render thread has drawn the one before.
    while (Clock::now() + cost <= due && WaitRenderIdle(due - cost)) {
      if (Clock::now() + cost > due) break;
      HoldBoth(ctx, base, sync);
      if (!BuildOnce(ctx, base, sync)) break;
    }
    ReturnFree(ctx, base, sync);
    return;
  }
  // The presentation clock: steps of 1 / rate; the tick's own frame takes the first step from the tick's start.
  // A step too late to build before the next tick is due (and the steps after it) goes to the next tick's own frame:
  // the clock stays there, behind the next tick's start, which restarts it (K44: the clock kept its phase past such a
  // step, so after one late tick every tick could skip the same step: 60 locked at 30, 120 at 90).
  const int rate = EffectiveFrameRate();
  const auto period = std::chrono::nanoseconds(1000000000ll / rate);
  const bool trace = REXCVAR_GET(kuf_trace_frame_clock) >= 0 &&
                     g_ticks.load() >= uint64_t(REXCVAR_GET(kuf_trace_frame_clock)) &&
                     g_ticks.load() < uint64_t(REXCVAR_GET(kuf_trace_frame_clock)) + 900;
  const auto scheduled = due - std::chrono::nanoseconds(kuf::pacing::TickSchedule::kTickNs);
  const double phase_in_ms = Ms(g_present_clock - g_tick_began);
  bool restart = false;
  auto end = due;  // steps from here on are the next tick's frame
  auto step = std::chrono::duration_cast<Clock::duration>(period);  // between the frames of this tick
  if (rate % 30 == 0 && REXCVAR_GET(kuf_present_clock_align) &&
      !REXCVAR_GET(kuf_present_clock_keep_late_phase)) {  // (the K44 fault test keeps the old clock)
    // D6 (K49): a whole number of frames per tick: the steps on the tick's schedule (phase 0), so the frames are
    // evenly spaced and the last one leaves a full step before the next tick. Steps already past when the tick
    // started late are the tick's own frame's.
    if (REXCVAR_GET(kuf_present_clock_from_handoff)) {
      // M8: the tick's own frame was handed to the render thread just now, after the tick's work and its scene build
      // (several ms after the tick's start); stepping from the tick's start put the first render-only frame that much
      // closer to it than the next tick's frame is to the last one (60 fps: 14 / 19.5 ms; at 120 the first step also
      // found the renderer still busy with the tick's frame and was dropped). The steps run from the hand-off; a step
      // that cannot be built before the next tick is due is still late (below).
      // When the hand-off came so late that the last step could not be built before the next tick is due, the steps
      // are drawn closer together (all of them fit) instead of dropping the last one (at 120 fps in 2560x1440 the
      // hand-off came 5-7 ms into the tick and a third of the ticks lost their last frame).
      const auto handoff = Clock::now();
      const int render_only = rate / 30 - 1;
      if (render_only > 0) {
        step = std::min(step, (due - cost - handoff) / render_only);
        step = std::max(step, std::chrono::duration_cast<Clock::duration>(period * 3 / 4));  // never bunched
      }
      g_present_clock = handoff;  // the tick's frame: the loop below starts by stepping past it
      end = handoff + render_only * step + step / 2;
    } else {
      g_present_clock = scheduled + period;
      while (g_present_clock <= g_tick_began) g_present_clock += period;
      g_present_clock -= period;  // the loop below starts by stepping past the tick's frame
      end = due - period / 2;  // n steps of 1 / rate end nanoseconds short of the tick: that last one is the next tick's
    }
  } else {
    restart = g_present_clock < g_tick_began;
    if (restart) {
      g_present_clock = g_tick_began;  // behind: from the tick's start, no catching up
      g_clock_restarts.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_present_clock += period;  // the tick's frame
  int frames = 1, late = 0, busy = 0;
  std::string steps;
  for (;; g_present_clock += step) {
    const auto at = g_present_clock;
    if (at >= end) break;  // the next tick's frame
    if (std::max(at, Clock::now()) + cost > due) {
      if (REXCVAR_GET(kuf_present_clock_keep_late_phase)) {
        ++late;
        if (trace) steps += fmt::format(" {:+.2f}:late", Ms(at - g_tick_began));
        continue;
      }
      for (auto a = at; a < due; a += period) {
        ++late;
        if (trace) steps += fmt::format(" {:+.2f}:late", Ms(a - g_tick_began));
      }
      break;
    }
    std::this_thread::sleep_until(at);
    if (!WaitRenderIdle(std::min(at + period, due - cost))) {
      ++busy;
      if (trace) steps += fmt::format(" {:+.2f}:busy", Ms(at - g_tick_began));
      continue;
    }
    HoldBoth(ctx, base, sync);
    if (!BuildOnce(ctx, base, sync)) break;
    ++frames;
    if (trace) steps += fmt::format(" {:+.2f}:built", Ms(at - g_tick_began));
  }
  ReturnFree(ctx, base, sync);
  g_builds_late.fetch_add(late, std::memory_order_relaxed);
  g_builds_busy.fetch_add(busy, std::memory_order_relaxed);
  if (late) g_late_ticks.fetch_add(1, std::memory_order_relaxed);
  g_frames_per_tick[std::min(frames, kFramesPerTickBuckets - 1)].fetch_add(1, std::memory_order_relaxed);
  if (trace) {
    REXLOG_INFO("[kuf-clock] tick {} start {:+.3f} ms after its schedule, clock phase {:+.3f} ms{}, cost {:.2f} ms, "
                "next tick due {:+.2f}; frames {}:{}",
                g_ticks.load(), Ms(g_tick_began - scheduled), phase_in_ms, restart ? " (restart)" : "", Ms(cost),
                Ms(due - g_tick_began), frames, steps);
  }
}

// --kuf_trace_slot_writes: guest memory around 4 ticks, with the render thread idle during each tick (the game thread
// takes the second "free" slot first). Phases: "between" (from the end of the previous traced tick to the start of
// this one: the render thread draws the previous tick's slot), "game" (the tick up to its render slot build) and
// "build" (sub_821F49E8, sub_821F4D00, sub_821F4CD8). Each changed virtual word is one line, for offline analysis.
constexpr uint32_t kSyncObject = 0x82AC6658;
constexpr int kSlotTraceTicks = 4;
struct SlotTrace {
  int ticks_left = -1;  // -1: not started
  bool in_tick = false;
  Snapshot snap;
  std::ofstream out;
};
SlotTrace g_slot_trace;  // game thread

void SlotTracePhase(const uint8_t* base, const char* phase) {
  auto& t = g_slot_trace;
  const Changes c = Diff(t.snap, base);
  uint64_t physical = 0;
  for (const auto& [k, n] : c.pages) physical += k.first ? n : 0;
  t.out << "phase " << phase << " tick " << Load32(base, kGameTickCounter) << " game_slot "
        << Load32(base, kGameSlotIndex) << " render_slot " << Load32(base, kRenderSlotIndex) << " bytes " << c.bytes
        << " physical_bytes " << physical << " words " << c.words.size() << "\n";
  char buf[64];
  for (const auto& [a, v] : c.words) {
    std::snprintf(buf, sizeof(buf), "%08llx %08x %08x\n", static_cast<unsigned long long>(a), v.first, v.second);
    t.out << buf;
  }
  t.snap = Take(base);
}

void SlotTraceTickStart(PPCContext& ctx, uint8_t* base) {
  auto& t = g_slot_trace;
  const int from = REXCVAR_GET(kuf_trace_slot_writes);
  if (from <= 0 || t.ticks_left == 0 || Load32(base, kGameTickCounter) < uint32_t(from) ||
      Load32(base, kSyncObject + kSyncEnabled) == 0) {
    return;
  }
  HoldFree(ctx, base, kSyncObject);  // the second slot: the render thread has drawn everything and waits
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (t.ticks_left < 0) {
    t.ticks_left = kSlotTraceTicks;
    const auto path = g_user_root / ("slot-writes-" + std::to_string(Load32(base, kGameTickCounter)) + ".txt");
    t.out.open(path);
    const uint32_t r = Load32(base, kRendererPointer), objects = Load32(base, r + kRendererObjects);
    t.out << std::hex << "renderer " << r << " objects " << objects << "\n";
    for (int i = 0; Plausible(objects) && i < 256; ++i) {
      const uint32_t o = Load32(base, objects + i * 4);
      if (Plausible(o)) t.out << "object " << std::dec << i << std::hex << " " << o << " vtable " << Load32(base, o)
                              << "\n";
    }
    const uint32_t vb = Load32(base, r + kRendererViews + 4), ve = Load32(base, r + kRendererViews + 8);
    for (uint32_t v = vb; Plausible(vb) && v < ve && v < vb + 64 * 4; v += 4) {
      t.out << "view_index " << std::dec << Load32(base, v) << std::hex << "\n";
    }
    t.out << std::dec;
    REXLOG_INFO("[kuf-pacing] slot write trace: {} ticks from game counter {} -> {}", kSlotTraceTicks,
                Load32(base, kGameTickCounter), path.string());
    t.snap = Take(base);
  } else {
    SlotTracePhase(base, "between");
  }
  t.in_tick = true;
}

void SlotTraceTickEnd(PPCContext& ctx, uint8_t* base) {
  auto& t = g_slot_trace;
  if (!t.in_tick) return;
  t.in_tick = false;
  SlotTracePhase(base, "build");
  if (--t.ticks_left == 0) {
    t.snap = {};
    t.out.close();
    REXLOG_INFO("[kuf-pacing] slot write trace written");
  }
  ReturnFree(ctx, base, kSyncObject);  // the slot taken at the start (the tick's own slot is handed on as usual)
}

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}

}  // namespace

namespace kuf::pacing {

void Configure(const std::filesystem::path& user_data_root) {
  g_user_root = user_data_root;
  g_fixed = REXCVAR_GET(kuf_fixed_tick);
  g_target = REXCVAR_GET(kuf_render_repeat);
  if (g_target != 0 && g_target != -1 && g_target < 30) {
    REXLOG_ERROR("[kuf-pacing] --kuf_render_repeat={} is below the game's 30 ticks per second; using 30", g_target);
    g_target = 30;
  }
  if (g_target != 0 && !g_fixed) {
    REXLOG_ERROR("[kuf-pacing] --kuf_render_repeat needs the fixed tick; turning --kuf_fixed_tick on");
    g_fixed = true;
  }
  g_builds_per_tick = std::clamp(REXCVAR_GET(kuf_render_builds), 0, 7);
  // The frame rate and what is interpolated: the PC settings (Settings > PC Settings > Frame Rate, D5), unless the
  // command line (or a REX_ environment value) gives the flag, which then wins for this run (shown in the menu, not
  // saved unless changed there).
  auto frame = kuf::pc::Current().frame_rate;
  std::string given;
  auto explicit_flag = [&given](const char* name) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) return false;
    given += fmt::format("{}--{}={}", given.empty() ? "" : " ", name, rex::cvar::GetFlagByName(name));
    return true;
  };
  if (explicit_flag("kuf_frame_rate")) {
    const int rate = REXCVAR_GET(kuf_frame_rate);
    if (rate == 0 || rate == -1 || (rate > 30 && rate <= 480)) {
      frame.fps_limit = rate == 0 ? kuf::pc::kFpsOriginal : rate == -1 ? kuf::pc::kFpsUnlimited : rate;
    } else {
      REXLOG_ERROR("[kuf-pacing] --kuf_frame_rate={} is not 0, -1 or 31-480; drawing one frame per game tick", rate);
      frame.fps_limit = kuf::pc::kFpsOriginal;
    }
  }
  if (explicit_flag("kuf_frame_interpolation")) frame.interpolation = rex::cvar::Query<bool>("kuf_frame_interpolation");
  if (explicit_flag("kuf_frame_interpolation_objects")) frame.objects = rex::cvar::Query<bool>("kuf_frame_interpolation_objects");
  if (explicit_flag("kuf_frame_interpolation_bones")) frame.animation = rex::cvar::Query<bool>("kuf_frame_interpolation_bones");
  if (explicit_flag("kuf_frame_interpolation_effects")) frame.effects = rex::cvar::Query<bool>("kuf_frame_interpolation_effects");
  if (!given.empty()) {
    kuf::pc::SetFrameRateForThisRun(frame);
    REXLOG_INFO("[kuf-pacing] frame rate from the command line for this run (the saved settings unchanged): {}", given);
  }
  g_frame_rate = RateOf(frame.fps_limit);
  g_interpolation = InterpolationOf(frame);
  if (g_frame_rate != 0 && g_builds_per_tick != 0) {
    REXLOG_ERROR("[kuf-pacing] --kuf_frame_rate and the diagnostic --kuf_render_builds exclude each other; using "
                 "--kuf_frame_rate");
    g_builds_per_tick = 0;
  }
  if (Building() && g_target != 0) {
    REXLOG_ERROR("[kuf-pacing] render-only frames and --kuf_render_repeat exclude each other; no render-only frames");
    g_builds_per_tick = 0;
    g_frame_rate = 0;
  }
  if (Building() && !g_fixed) {
    REXLOG_ERROR("[kuf-pacing] render-only frames need the fixed tick; turning --kuf_fixed_tick on");
    g_fixed = true;
  }
  kuf::presentation::Configure(Building(), g_interpolation);
  g_active_frame = frame;
  g_tick_start_ns.reserve(kMaxTickTimes);
  if (!g_fixed) {
    REXLOG_INFO("[kuf-pacing] original pacing: one game tick per presented frame");
    return;
  }
  REXLOG_INFO("[kuf-pacing] fixed tick: 30 game ticks per second on a host clock");
  if (g_target != 0) {
    // 1000 guest vblanks per second (the SDK's --vsync=false), so the console's present interval no longer limits the
    // frame rate. The cvar belongs to the GPU plugin, which loads later and applies REX_<NAME> environment values.
    setenv("REX_VSYNC", "false", 1);
    REXLOG_WARN("[kuf-pacing] D1 diagnostic --kuf_render_repeat: drawing the latest tick again at {} (repeated frames "
                "are black; not for play)",
                g_target < 0 ? std::string("an unlimited rate") : "up to " + std::to_string(g_target) + " fps");
  }
  if (Building()) {
    setenv("REX_VSYNC", "false", 1);  // as above: the frames between ticks need more than two vblanks of 60
  }
  if (g_frame_rate != 0) {
    REXLOG_INFO("[kuf-pacing] render-only frames: up to {} frames per second between the game's 30 ticks",
                g_frame_rate < 0 ? std::string("as many") : std::to_string(g_frame_rate));
  }
  if (g_builds_per_tick != 0) {
    REXLOG_WARN("[kuf-pacing] D3a diagnostic --kuf_render_builds={}: {} render-only builds per tick", g_builds_per_tick,
                g_builds_per_tick);
  }
}

void RequestFrameRate(const kuf::pc::FrameRateSettings& frame) {
  if (REXCVAR_GET(kuf_debug_frame_rate_ignored)) {
    REXLOG_WARN("[kuf-pacing] frame rate request dropped (fault test --kuf_debug_frame_rate_ignored)");
    return;
  }
  {
    std::lock_guard lock(g_request_mutex);
    g_requested_frame = frame;
  }
  g_frame_requested = true;
}

bool RenderOnlyFrames() { return Building(); }
bool InRenderOnlyBuild() { return t_building; }
bool OnRenderThread() { return std::this_thread::get_id() == g_render_thread.load(); }
double RenderEffectLag() { return g_render_effect_lag.load(std::memory_order_relaxed); }
uint64_t RenderFrameTicks() {
  return Building() && OnRenderThread() ? g_render_ticks.load(std::memory_order_relaxed) : TicksCompleted();
}
bool DrawingRenderOnlyFrame() { return Building() && !g_new_tick && OnRenderThread(); }

bool FixedTick() { return g_fixed; }
bool Repeating() { return g_target != 0; }

int RepeatTarget() { return g_target; }

bool OnPresent() {
  g_presents.fetch_add(1, std::memory_order_relaxed);
  {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    std::lock_guard lock(g_tick_times_mutex);
    if (g_present_ns.size() < kMaxTickTimes * 8) g_present_ns.push_back(now);
  }
  if (!Repeating()) {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    if (Building() && std::this_thread::get_id() == g_render_thread.load()) {
      NoteShown(g_render_shown, now);
      const bool fresh = g_new_tick;  // a tick's frame, not a render-only build's
      g_new_tick = false;
      if (!fresh) return false;
    }
    const uint64_t shown = g_presents_new.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!Building() && shown >= 2) NoteShown(double(shown - 1), now);
    {
      // From the start of the tick shown (when it read its input) to its frame's present. The first frame is presented
      // before the first tick: tick frame N shows tick N - 1.
      const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
      std::lock_guard lock(g_tick_times_mutex);
      if (shown >= 2 && shown - 1 <= g_tick_start_ns.size() && g_tick_latency_ns.size() < kMaxTickTimes) {
        g_tick_latency_ns.push_back(now - g_tick_start_ns[shown - 2]);
      }
    }
    const int64_t lead = int64_t(GameTickCounter()) - int64_t(shown);
    g_lead_counts[std::clamp<int64_t>(lead + 1, 0, 4)].fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (std::this_thread::get_id() != g_render_thread.load()) {
    if (!g_logged_foreign_present.exchange(true)) {
      REXLOG_WARN("[kuf-pacing] a frame was presented outside the render thread's loop (counted as a new tick)");
    }
    g_presents_new.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  const bool fresh = g_new_tick;
  g_new_tick = false;
  if (fresh) {
    g_presents_new.fetch_add(1, std::memory_order_relaxed);
  }
  return fresh;
}

uint64_t TicksCompleted() { return g_ticks.load(std::memory_order_relaxed); }
uint64_t FramesDrawn() { return g_presents.load(std::memory_order_relaxed); }

uint32_t GameTickCounter() {
  const uint8_t* base = g_base.load();
  return base ? Load32(base, kGameTickCounter) : 0;
}

void WriteJson(std::ostream& out) {
  std::vector<double> intervals, presents, latency, age, full;
  {
    // Tick intervals once the game runs (from the 61st tick; the first ones include start-up).
    std::lock_guard lock(g_tick_times_mutex);
    for (size_t i = 61; i < g_tick_start_ns.size(); ++i) {
      intervals.push_back((g_tick_start_ns[i] - g_tick_start_ns[i - 1]) / 1e6);
    }
    for (size_t i = 61; i < g_present_ns.size(); ++i) {
      presents.push_back((g_present_ns[i] - g_present_ns[i - 1]) / 1e6);
    }
    for (size_t i = 61; i < g_tick_latency_ns.size(); ++i) latency.push_back(g_tick_latency_ns[i] / 1e6);
    for (size_t i = 61 * 4; i < g_state_age_ns.size(); ++i) age.push_back(g_state_age_ns[i] / 1e6);
    for (size_t i = 61; i < g_full_response_ns.size(); ++i) full.push_back(g_full_response_ns[i] / 1e6);
  }
  auto mean = [](const std::vector<double>& v) {
    double sum = 0;
    for (double x : v) sum += x;
    return v.empty() ? 0 : sum / v.size();
  };
  auto largest = [](const std::vector<double>& v) { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); };
  const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
  char buf[2048];
  std::snprintf(buf, sizeof(buf),
                "  \"pacing\": {\"mode\": \"%s\", \"render_repeat\": %d, \"ticks\": %llu, \"presents\": %llu, "
                "\"presents_new_tick\": %llu, \"backlog_drops\": %llu, \"freezes\": %d, \"render_builds\": %d, "
                "\"frame_rate\": %d, \"builds\": %llu, \"builds_skipped\": %llu, \"builds_late\": %llu, "
                "\"builds_busy\": %llu, \"rand_in_builds\": %llu, \"physx_in_builds\": %llu, "
                "\"restore_failures\": %llu, \"restore_dropped\": %llu, \"restore_injected\": %llu, "
                "\"effect_clock_frames\": %llu, \"effect_clock_sets\": %llu, \"effect_clock_leaks\": %llu, "
                "\"alpha_held\": %llu, \"frame_rate_switches\": %llu, \"late_ticks\": %llu, \"clock_restarts\": %llu, "
                "\"tick_interval_ms\": "
                "{\"count\": %zu, \"mean\": %.3f, \"p1\": %.3f, \"p50\": %.3f, \"p99\": %.3f, \"max\": %.3f}, "
                "\"present_interval_ms\": "
                "{\"count\": %zu, \"mean\": %.3f, \"p1\": %.3f, \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"max\": %.3f}, "
                "\"tick_to_present_ms\": {\"count\": %zu, \"mean\": %.3f, \"p50\": %.3f, \"p99\": %.3f}, "
                "\"tick_lead_at_present\": {\"<=-1\": %llu, \"0\": %llu, \"1\": %llu, \"2\": %llu, \">=3\": %llu}},\n",
                g_fixed ? "fixed_tick" : "original", g_target, u(g_ticks.load()), u(g_presents.load()),
                u(g_presents_new.load()), u(g_backlog_drops.load()), g_freezes, g_builds_per_tick, g_frame_rate,
                u(g_builds.load()), u(g_builds_late.load() + g_builds_busy.load()), u(g_builds_late.load()),
                u(g_builds_busy.load()), u(g_rand_in_builds.load()), u(g_physx_in_builds.load()),
                u(kuf::render_frames::RestoreFailures()), u(kuf::render_frames::RestoreDropped()),
                u(kuf::render_frames::RestoreInjected()), u(kuf::render_frames::EffectClockFrames()),
                u(kuf::render_frames::EffectClockSets()), u(kuf::render_frames::EffectClockLeaks()),
                u(g_alpha_held.load()), u(g_frame_switches.load()), u(g_late_ticks.load()),
                u(g_clock_restarts.load()), intervals.size(), mean(intervals),
                Percentile(intervals, 0.01), Percentile(intervals, 0.5), Percentile(intervals, 0.99),
                largest(intervals), presents.size(), mean(presents), Percentile(presents, 0.01),
                Percentile(presents, 0.5), Percentile(presents, 0.95), Percentile(presents, 0.99), largest(presents),
                latency.size(),
                mean(latency), Percentile(latency, 0.5), Percentile(latency, 0.99),
                u(g_lead_counts[0].load()), u(g_lead_counts[1].load()), u(g_lead_counts[2].load()),
                u(g_lead_counts[3].load()), u(g_lead_counts[4].load()));
  out << buf;
  out << "  \"frames_per_tick\": [";
  for (int i = 0; i < kFramesPerTickBuckets; ++i) out << (i ? ", " : "") << g_frames_per_tick[i].load();
  out << "],\n";
  std::snprintf(buf, sizeof(buf),
                "  \"latency\": {\"state_age_ms\": {\"count\": %zu, \"mean\": %.3f, \"p50\": %.3f, \"p99\": %.3f}, "
                "\"full_response_ms\": {\"count\": %zu, \"mean\": %.3f, \"p50\": %.3f, \"p99\": %.3f}},\n",
                age.size(), mean(age), Percentile(age, 0.5), Percentile(age, 0.99), full.size(), mean(full),
                Percentile(full, 0.5), Percentile(full, 0.99));
  out << buf;
  kuf::presentation::WriteJson(out);
}

void RequestGpuTrace(uint64_t frame) {
  g_trace_request_frame = frame;
  g_trace_requested = true;
}

bool RequestFreeze(int seconds, const std::string& label, uint64_t frame) {
  if (Building()) {
    std::lock_guard lock(g_mutex);
    if (g_build_freeze.requested) {
      REXLOG_ERROR("[kuf-pacing] freeze {}: another freeze is running", label);
      return false;
    }
    g_build_freeze = {true, seconds, label, frame};
    return true;
  }
  if (!Repeating() || !g_render_started) {
    REXLOG_ERROR("[kuf-pacing] freeze {}: needs --kuf_render_repeat and a running game", label);
    return false;
  }
  if (g_freeze.state != FreezeState::kIdle) {
    REXLOG_ERROR("[kuf-pacing] freeze {}: another freeze is running", label);
    return false;
  }
  g_freeze.state = FreezeState::kRequested;
  g_freeze.seconds = seconds;
  g_freeze.label = label;
  g_freeze.frame = frame;
  return true;
}

}  // namespace kuf::pacing

namespace {
// The calling thread's CRT rand() seed (sub_827FACD0: seed = seed * 214013 + 2531011 at _getptd() + 20).
REX_EXTERN(__imp__sub_827FE1A8);
uint32_t RandSeed(PPCContext& ctx, uint8_t* base) {
  const uint64_t r3 = ctx.r3.u64, lr = ctx.lr;
  __imp__sub_827FE1A8(ctx, base);
  const uint32_t ptd = ctx.r3.u32;
  ctx.r3.u64 = r3;
  ctx.lr = lr;
  return Plausible(ptd) ? Load32(base, ptd + 20) : 0;
}

float AsFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// --kuf_trace_game_state: at the tick's slot build (its game step done, nothing of it drawn yet). Objects as
// index:class:x,y,z (their world matrix's translation for the game slot, object + 28 + slot * 64 + 48).
void LogGameState(PPCContext& ctx, uint8_t* base, uint32_t renderer) {
  const uint32_t slot = Load32(base, kGameSlotIndex) & 1;
  const uint32_t list = Load32(base, renderer + kRendererSlotList + slot * 4);
  const uint32_t objects = Load32(base, renderer + kRendererObjects);
  std::string line;
  uint32_t count = 0;
  if (Plausible(list) && Plausible(objects)) {
    const uint32_t b = Load32(base, list + 4), e = Load32(base, list + 8);
    for (uint32_t a = b; Plausible(b) && a < e && a < b + 0x4000; a += 4, ++count) {
      const uint32_t index = Load32(base, a);
      const uint32_t object = index < 0x1000 ? Load32(base, objects + index * 4) : 0;
      if (!Plausible(object)) continue;
      const uint32_t m = object + 28 + slot * 64;
      line += fmt::format(" {}:{:08X}:{:.6f},{:.6f},{:.6f}", index, Load32(base, object), AsFloat(Load32(base, m + 48)),
                          AsFloat(Load32(base, m + 52)), AsFloat(Load32(base, m + 56)));
    }
  }
  REXLOG_INFO("[kuf-state] tick {} counter {} seed {:08X} objects {}:{}", g_ticks.load() + 1,
              Load32(base, kGameTickCounter), RandSeed(ctx, base), count, line);
}
}  // namespace

// ---- game thread ----

REX_EXTERN(__imp__sub_826F0D58);
REX_HOOK_RAW(sub_826F0D58) {  // one game tick: (world)
  g_base = base;
  {  // M7: title list labels, PC Settings key captures, the menu pointer (guest calls: the tick's registers kept)
    const PPCContext saved = ctx;
    kuf::title_menu::OnTick(ctx, base);
    kuf::pc_menu::OnTick(ctx, base);
    kuf::menu_pointer::OnTick(ctx, base);  // UX fix 1: hover and clicks before the tick reads input
    kuf::lobby_screen::OnTick(ctx, base);  // the multiplayer menus on the game type screen
    kuf::coop::Maintain();                 // the lobby's events and Steam's callbacks, once per tick
    kuf::controls_screen::OnTick(ctx, base);  // UI polish: the Controls page for the device in use
    ctx = saved;
  }
  kuf::input::script::OnGameTick(g_ticks.load(std::memory_order_relaxed) + 1);  // before the tick reads input (K38)
  if (Building() && Load32(base, kSyncObject + kSyncEnabled) != 0) {
    kuf::render_frames::OnTickStart(ctx, base, Load32(base, kGameSlotIndex));
    kuf::presentation::OnTickStart(base, Load32(base, kGameSlotIndex));
  }
  const auto now = Clock::now().time_since_epoch();
  {
    std::lock_guard lock(g_tick_times_mutex);
    if (g_tick_start_ns.size() < kMaxTickTimes) {
      g_tick_start_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }
  }
  g_tick_marked_world = false;
  const uint32_t world = ctx.r3.u32;
  g_world = world;
  SlotTraceTickStart(ctx, base);
  const auto tick_began = Clock::now();
  ctx.r3.u64 = world;
  __imp__sub_826F0D58(ctx, base);
  if (Building()) g_cost_tick_ms.push_back(Ms(Clock::now() - tick_began));
  SlotTraceTickEnd(ctx, base);
  if (Load32(base, kGameTickCounter) > uint32_t(REXCVAR_GET(kuf_trace_render_lists))) LogPacketWriters(base);
  g_ticks.fetch_add(1, std::memory_order_relaxed);
}

REX_EXTERN(__imp__sub_827FACD0);
REX_HOOK_RAW(sub_827FACD0) {  // the CRT's rand()
  if (Building()) (t_building ? g_rand_in_builds : g_rand_in_ticks).fetch_add(1, std::memory_order_relaxed);
  __imp__sub_827FACD0(ctx, base);
}

REX_EXTERN(__imp__sub_821A2CA0);
REX_HOOK_RAW(sub_821A2CA0) {  // an object added to the game slot's frame packet: (renderer, object index, ...)
  NotePacketWriter(base, ctx, "object");
  __imp__sub_821A2CA0(ctx, base);
}

REX_EXTERN(__imp__sub_821ABE90);
REX_HOOK_RAW(sub_821ABE90) {  // an item added to the game slot's 2D (HUD) list, renderer + 28976 + slot * 16
  NotePacketWriter(base, ctx, "hud");
  __imp__sub_821ABE90(ctx, base);
}

REX_EXTERN(__imp__sub_821A3390);
REX_HOOK_RAW(sub_821A3390) {  // a record queued in a bucket of the game slot's frame packet
  NotePacketWriter(base, ctx, "bucket");
  __imp__sub_821A3390(ctx, base);
}

REX_HOOK_RAW(sub_821F49E8) {  // the tick's render slot build: (renderer)
  if (g_slot_trace.in_tick && !t_building) {
    SlotTracePhase(base, "game");
  }
  const uint32_t renderer = ctx.r3.u32;
  if (Building() && !t_building) kuf::presentation::OnTickSlotBuild(base, renderer);
  TraceBuildLists(base, renderer, "slot build in", t_building ? "build" : "tick");
  if (REXCVAR_GET(kuf_trace_game_state) && !t_building) LogGameState(ctx, base, renderer);
  if (Building() && !t_building) {
    kuf::render_frames::SaveTickOutputs(base, renderer);
    g_tick_scene_began = Clock::now();
    g_tick_frame_shown = kuf::presentation::OnTickFrame(base, renderer, g_ticks.load() + 1, g_tick_marked_world,
                                                        FrameAlpha(g_tick_scene_ms_estimate));
    g_tick_frame_effect_lag = std::isnan(g_tick_frame_shown) ? 0.0 : kuf::presentation::EffectLag();
    if (std::isnan(g_tick_frame_shown)) g_tick_frame_shown = double(g_ticks.load() + 1);
  }
  __imp__sub_821F49E8(ctx, base);
  TraceBuildLists(base, renderer, "slot build out", t_building ? "build" : "tick");
}

REX_HOOK_RAW(sub_821F4D00) {  // ... the views and the frame's setup: (renderer)
  const uint32_t renderer = ctx.r3.u32;
  __imp__sub_821F4D00(ctx, base);
  if (Building() && !t_building) kuf::presentation::OnTickSceneDone();
  if (Building() && !t_building) {
    const double ms = Ms(Clock::now() - g_tick_scene_began);
    g_cost_tick_scene_ms.push_back(ms);
    g_tick_scene_ms_estimate = 0.9 * g_tick_scene_ms_estimate + 0.1 * ms;
  }
  TraceBuildLists(base, renderer, "views set up", t_building ? "build" : "tick");
}

REX_HOOK_RAW(sub_821A2E70) {  // the tick marks its slot as a world frame: (renderer)
  if (!t_building) g_tick_marked_world = true;
  __imp__sub_821A2E70(ctx, base);
}

REX_HOOK_RAW(sub_826F2DB8) {  // the game thread's wait for "free": (sync)
  if (!g_logged_game_thread.exchange(true)) {
    REXLOG_INFO("[kuf-pacing] game thread (this log line's thread tag)");
  }
  if (!kuf::pacing::FixedTick()) {
    __imp__sub_826F2DB8(ctx, base);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_game_blocked = true;
  }
  g_cv.notify_all();
  __imp__sub_826F2DB8(ctx, base);
  {
    std::lock_guard lock(g_mutex);
    g_game_blocked = false;
  }
  PaceGameTick();
}

REX_HOOK_RAW(sub_826F2E10) {  // the game thread's release of "filled": (sync)
  const uint32_t sync = ctx.r3.u32;
  if (Building()) {
    PushSlotKind(true, g_tick_frame_shown, g_tick_frame_effect_lag);
    g_frames_handed.fetch_add(1);
  }
  __imp__sub_826F2E10(ctx, base);
  g_produced.fetch_add(1);
  if (Load32(base, sync + kSyncEnabled) != 0 && ApplyFrameRequest(ctx, base, sync)) {
    // Changed now: this tick's frame was drawn as before; render-only frames (if any) from the next tick.
  } else if (Building() && Load32(base, sync + kSyncEnabled) != 0) {
    AfterTick(ctx, base, sync);
  }
  if (kuf::pacing::Repeating()) {
    std::lock_guard lock(g_mutex);  // pairs with PaceFrame's wait
    g_cv.notify_all();
  }
}

// ---- render thread ----

REX_HOOK_RAW(sub_826F2EA0) {  // the render thread's wait for "filled": (sync)
  const uint32_t sync = ctx.r3.u32;
  if (!kuf::pacing::Repeating() || Load32(base, sync + kSyncEnabled) == 0) {
    if (g_render_thread.load() == std::thread::id()) {  // in every mode: render-only frames can start later (D5)
      g_render_thread = std::this_thread::get_id();
      if (Building()) REXLOG_INFO("[kuf-pacing] render thread (this log line's thread tag)");
    }
    __imp__sub_826F2EA0(ctx, base);
    g_new_tick = Building() ? PopSlotKind() : true;
    if (g_new_tick) {
      ArmGpuTrace();
    } else {
      kuf::render_frames::OnBuildFrameStart(base);
    }
    return;
  }
  if (!g_render_started) {
    g_render_thread = std::this_thread::get_id();
    REXLOG_INFO("[kuf-pacing] render thread (this log line's thread tag)");
    __imp__sub_826F2EA0(ctx, base);
    g_render_started = true;
    ++g_consumed;
    g_new_tick = true;
    ArmGpuTrace();
    return;
  }
  FreezeStep(base);
  PaceFrame();
  bool take = false;
  {
    std::lock_guard lock(g_mutex);
    take = !g_hold_game && g_produced.load() > g_consumed;
  }
  if (!take) {
    g_new_tick = false;  // draw the current tick again
    TraceLists(base, "before");
    return;
  }
  if (g_freeze.state == FreezeState::kRequested) {
    BeginFreeze(ctx, base, sync);  // holds the game, then takes this tick
    return;
  }
  TakeTick(ctx, base, sync);
  TraceLists(base, "before");
}

REX_HOOK_RAW(sub_826F2E40) {  // the render thread's release of "free": (sync)
  if (kuf::pacing::Repeating() && g_render_started && Load32(base, ctx.r3.u32 + kSyncEnabled) != 0) {
    return;  // released when the next tick is taken (sub_826F2EA0 above)
  }
  __imp__sub_826F2E40(ctx, base);
  if (Building()) {
    {
      std::lock_guard lock(g_idle_mutex);
      g_frames_done.fetch_add(1);
    }
    g_idle_cv.notify_all();
  }
}

REX_HOOK_RAW(sub_821A3BA0) {  // the end of a frame: (renderer)
  if (!kuf::pacing::Repeating() || !g_render_started) {
    TraceBuildLists(base, ctx.r3.u32, "frame end", g_new_tick ? "tick" : "build");
    __imp__sub_821A3BA0(ctx, base);
    return;
  }
  // Every frame: the device's end-of-frame step and the GPU resource collection; the slot stays as it is.
  const uint32_t renderer = ctx.r3.u32;
  g_renderer = renderer;
  TraceLists(base, "after");
  ctx.r3.u64 = Load32(base, renderer + kRendererDevice);
  __imp__sub_821E3F48(ctx, base);
  ctx.r3.u64 = renderer;
  __imp__sub_821C8710(ctx, base);
  g_finish_pending = true;
}

// A view's per-slot lists (view + 1124 / 1156 / 1188 + slot * 16: what the view draws) are cleared by the draw
// itself (sub_821F5608, once per view after drawing); a decoupled frame leaves them for FinishSlot.
REX_HOOK_RAW(sub_821EA3A0) {  // (view)
  if (kuf::pacing::Repeating() && g_render_started && !t_finishing && uint32_t(ctx.lr) == kDrawViewClearCaller &&
      std::this_thread::get_id() == g_render_thread.load()) {
    const uint32_t view = ctx.r3.u32;
    if (std::find(g_deferred_view_clears.begin(), g_deferred_view_clears.end(), view) == g_deferred_view_clears.end()) {
      g_deferred_view_clears.push_back(view);
    }
    return;
  }
  __imp__sub_821EA3A0(ctx, base);
}

// sub_821A3BA0's per-frame calls, skipped when FinishSlot runs the rest of it (sub_821E3F48 has one other caller).
REX_HOOK_RAW(sub_821E3F48) {
  if (!t_finishing) __imp__sub_821E3F48(ctx, base);
}
REX_HOOK_RAW(sub_821C8710) {
  if (!t_finishing) __imp__sub_821C8710(ctx, base);
}

REX_EXTERN(__imp__sub_82230E00);
REX_HOOK_RAW(sub_82230E00) {  // PhysX simulate (docs/HIGH_FPS.md 1.6): counted in render-only builds and in ticks
  if (Building()) (t_building ? g_physx_in_builds : g_physx_in_ticks).fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82230E00(ctx, base);
}
