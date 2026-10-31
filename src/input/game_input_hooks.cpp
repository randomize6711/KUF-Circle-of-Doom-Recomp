// What the game's own input layer tells us (v1 addresses, docs/ENGINE_ANALYSIS.md "Input").
//
// sub_825EAD58 is the game's per-frame input update: it polls XamInputGetState for users 0-3, edge-detects
// 14 buttons and both triggers, and sends each press/release as a message (0x40000, sub_825EB570) to the
// user's current input target. Its object (r3) holds:
//   +8 * (user + 1)  the user's input-target handle (8 bytes): valid (not 0xFFFFFFFF) exactly while the
//                    player character has control (gameplay, tutorial included); invalid on the title
//                    screen, in the menus, and in the in-game status/options menus (pause)
//   +44              the active user, stored by the first START press
//   +48              "waiting for START": set while the game waits for a START press to pick the active
//                    controller (the title screen's "Press START")
// The keyboard and mouse use these to choose their meaning (KeyboardPad contexts) and to capture the pointer
// for mouse look only in gameplay. Read-only; game behaviour is unchanged.

#include "input/game_input_hooks.h"

#include <array>
#include <atomic>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "input/input_script.h"
#include "runtime/telemetry.h"

REXCVAR_DEFINE_BOOL(kuf_trace_game_input, false, "KUF/Debug",
                    "Log the game's input layer: user 0's polled buttons, its press gates and target handle when "
                    "they change, and every press or release it sends ([kuf-game-input])");

REX_EXTERN(__imp__sub_825EAD58);
REX_EXTERN(__imp__sub_825EB570);

namespace kuf::input {
namespace {

constexpr uint32_t kTargetHandleOffset = 8;
constexpr uint32_t kActiveUserOffset = 44;
constexpr uint32_t kWaitingForStartOffset = 48;
constexpr uint32_t kInvalidHandle = 0xFFFFFFFF;

std::atomic<GameContext> g_context{GameContext::kMenu};

const char* Name(GameContext c) {
  switch (c) {
    case GameContext::kStartPrompt: return "start prompt";
    case GameContext::kGameplay: return "gameplay";
    default: return "menu";
  }
}

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return __builtin_bswap32(v);
}

}  // namespace

GameContext CurrentGameContext() { return g_context.load(std::memory_order_relaxed); }

namespace {
std::atomic<uint32_t> g_input_object{0};  // the input update's object (its last call)
}  // namespace

GameContext LiveGameContext(const uint8_t* base) {
  const uint32_t address = g_input_object.load(std::memory_order_relaxed);
  if (!address) return CurrentGameContext();
  const uint8_t* obj = base + address;
  if (obj[kWaitingForStartOffset] != 0) return GameContext::kStartPrompt;
  uint32_t user = LoadBE32(obj + kActiveUserOffset);
  if (user > 3) user = 0;
  return LoadBE32(obj + kTargetHandleOffset * (user + 1)) != kInvalidHandle ? GameContext::kGameplay
                                                                            : GameContext::kMenu;
}

bool GameWaitsForStart() { return CurrentGameContext() == GameContext::kStartPrompt; }

}  // namespace kuf::input

REX_HOOK_RAW(sub_825EAD58) {
  using namespace kuf::input;
  // The polls below (XamInputGetState) run inside this call, so they see this frame's context.
  const uint8_t* obj = base + ctx.r3.u32;
  g_input_object.store(ctx.r3.u32, std::memory_order_relaxed);
  GameContext c = GameContext::kMenu;
  if (obj[kWaitingForStartOffset] != 0) {
    c = GameContext::kStartPrompt;
  } else {
    uint32_t user = LoadBE32(obj + kActiveUserOffset);
    if (user > 3) {
      user = 0;  // the keyboard and the first controller feed user 0
    }
    if (LoadBE32(obj + kTargetHandleOffset * (user + 1)) != kInvalidHandle) {
      c = GameContext::kGameplay;
    }
  }
  const GameContext previous = g_context.exchange(c, std::memory_order_relaxed);
  if (previous != c) {
    // The input script's anchors count from this tick (K38); the frame (the tick frames presented so far) shows how
    // far the render thread is behind.
    REXLOG_INFO("[kuf-input] context: {} (tick {}, frame {})", Name(c), kuf::input::script::CurrentTick(),
                kuf::telemetry::TickFramesPresented());
    kuf::input::script::OnContextEntered(
        c == GameContext::kGameplay ? "gameplay" : c == GameContext::kStartPrompt ? "prompt" : "menu");
    // Kept from M4: tests and docs refer to these two lines.
    if (c == GameContext::kStartPrompt) {
      REXLOG_INFO("[kuf-input] game waits for START");
    } else if (previous == GameContext::kStartPrompt) {
      REXLOG_INFO("[kuf-input] game no longer waits for START");
    }
  }
  const uint32_t input = ctx.r3.u32;
  __imp__sub_825EAD58(ctx, base);
  if (REXCVAR_GET(kuf_trace_game_input)) {
    // After the poll: user 0's buttons as read (+50), the handler's gates (+4 on, +5 and +6 off), the target handle.
    static std::array<uint32_t, 4> last{};
    const uint8_t* o = base + input;
    const std::array<uint32_t, 4> now{uint32_t(o[50] << 8 | o[51]), uint32_t(o[4] << 16 | o[5] << 8 | o[6]),
                                      LoadBE32(o + 8), LoadBE32(o + 12)};
    if (now != last) {
      last = now;
      REXLOG_INFO("[kuf-game-input] tick {}: buttons {:04X} gates {:06X} handle {:08X}:{:08X}",
                  kuf::input::script::CurrentTick(), now[0], now[1], now[2], now[3]);
    }
  }
}

REX_HOOK_RAW(sub_825EB570) {  // a press or release to a user's input target: (input, user, event, ...)
  if (REXCVAR_GET(kuf_trace_game_input)) {
    const uint32_t user = ctx.r4.u32, event = ctx.r5.u32;
    REXLOG_INFO("[kuf-game-input] tick {}: user {} event {}{} to {:08X}:{:08X}", kuf::input::script::CurrentTick(),
                user, event & 0x7FFFFFFF, event & 0x80000000 ? " up" : " down",
                kuf::input::LoadBE32(base + ctx.r3.u32 + 8 * (user + 1)),
                kuf::input::LoadBE32(base + ctx.r3.u32 + 8 * (user + 1) + 4));
  }
  __imp__sub_825EB570(ctx, base);
}
