#include "input/input_script.h"

#include <SDL3/SDL.h>

#include <fstream>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "input/input_script_parse.h"
#include "input/keyboard_pad.h"
#include "input/kuf_input_driver.h"
#include "runtime/frame_capture.h"
#include "runtime/frame_pacing.h"

REXCVAR_DEFINE_STRING(kuf_input_script, "", "KUF/Input",
                      "Scripted input for tests: '<frame> <verb> [args]; ...' (src/input/input_script.h)");
REXCVAR_DEFINE_STRING(kuf_input_script_file, "", "KUF/Input",
                      "Scripted input from a file: items as for kuf_input_script, one or more per line, '#' "
                      "comment lines (for long scenarios; the command line is limited to 128 KiB per argument)");
REXCVAR_DEFINE_STRING(kuf_input_script_pad, "xbox360", "KUF/Input",
                      "Virtual gamepad model for 'pad connect': xbox360 or switchpro (Nintendo face-button labels)");

namespace kuf::input::script {
namespace {

// Script time is game ticks (K38): an item at time N that is input is applied on the game thread when tick N + 1
// starts, before the tick reads input; an item about the presented image (capture, trace, freeze) after tick frame N
// is presented (every tick is drawn once and the frames that show a tick for the first time are counted,
// telemetry.cpp; the first is presented before the first tick, so tick frame N shows tick N - 1). Both follow the
// game's own progress, so which tick sees an item no longer depends on how far the game thread runs ahead of the
// render thread. Anchors count from the tick in which the game entered the context.
std::mutex g_mutex;
std::multimap<uint64_t, Event> g_events;                 // script time -> input item (game thread)
std::multimap<uint64_t, Event> g_present_events;         // script time -> presentation item (present hook)
std::vector<std::pair<uint64_t, Event>> g_anchored;      // (offset, event) waiting for their context entry
std::map<std::string, int> g_context_entries;
uint64_t g_tick = 0;  // the game tick running now (1-based; 0 before the first)

bool IsPresentation(const Event& e) { return e.verb == "capture" || e.verb == "trace" || e.verb == "freeze"; }

std::multimap<uint64_t, Event>& QueueFor(const Event& e) { return IsPresentation(e) ? g_present_events : g_events; }
SDL_JoystickID g_pad_id = 0;
SDL_Joystick* g_pad = nullptr;

void PadConnect() {
  if (g_pad) {
    return;
  }
  const bool switch_pro = REXCVAR_GET(kuf_input_script_pad) == "switchpro";
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.vendor_id = switch_pro ? 0x057E : 0x045E;   // Nintendo / Microsoft
  desc.product_id = switch_pro ? 0x2009 : 0x028E;  // Switch Pro Controller / Xbox 360 Controller
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;  // the standard buttons, in SDL_GamepadButton order
  desc.name = switch_pro ? "KUF virtual Switch Pro gamepad" : "KUF virtual Xbox 360 gamepad";
  g_pad_id = SDL_AttachVirtualJoystick(&desc);
  if (!g_pad_id) {
    REXLOG_ERROR("[kuf-input-script] SDL_AttachVirtualJoystick failed: {}", SDL_GetError());
    return;
  }
  g_pad = SDL_OpenJoystick(g_pad_id);
  if (!g_pad) {
    REXLOG_ERROR("[kuf-input-script] SDL_OpenJoystick failed: {}", SDL_GetError());
  }
}

void PadDisconnect() {
  if (g_pad) {
    SDL_CloseJoystick(g_pad);
    g_pad = nullptr;
  }
  if (g_pad_id) {
    SDL_DetachVirtualJoystick(g_pad_id);
    g_pad_id = 0;
  }
}

SDL_WindowID FirstWindowId() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_WindowID id = (windows && count > 0) ? SDL_GetWindowID(windows[0]) : 0;
  SDL_free(windows);
  return id;
}

Uint8 SdlMouseButton(const std::string& name) {
  return name == "left" ? SDL_BUTTON_LEFT : name == "right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE;
}

// Pushes a real SDL event for the game window; WindowSDL handles it like input from the desktop.
void ApplySdl(const Event& e, KufInputDriver* driver) {
  const auto& a = e.args;
  const SDL_WindowID window_id = FirstWindowId();
  if (!window_id) {
    REXLOG_ERROR("[kuf-input-script] '{}': no SDL window", e.text);
    return;
  }
  SDL_Event ev{};
  const Uint64 now = SDL_GetTicksNS();
  if (a[0] == "key") {
    const SDL_Scancode sc = SDL_GetScancodeFromName(a[2].c_str());
    ev.type = a[1] == "up" ? SDL_EVENT_KEY_UP : SDL_EVENT_KEY_DOWN;
    ev.key.timestamp = now;
    ev.key.windowID = window_id;
    ev.key.scancode = sc;
    ev.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    ev.key.down = a[1] != "up";
    ev.key.repeat = a[1] == "repeat";
  } else if (a[0] == "button") {
    ev.type = a[1] == "down" ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.timestamp = now;
    ev.button.windowID = window_id;
    ev.button.button = SdlMouseButton(a[2]);
    ev.button.down = a[1] == "down";
    ev.button.clicks = 1;
    ev.button.x = float(std::stoi(a[3]));
    ev.button.y = float(std::stoi(a[4]));
  } else if (a[0] == "wheel") {
    ev.type = SDL_EVENT_MOUSE_WHEEL;
    ev.wheel.timestamp = now;
    ev.wheel.windowID = window_id;
    ev.wheel.y = float(std::stoi(a[1]));
    ev.wheel.mouse_x = float(std::stoi(a[2]));
    ev.wheel.mouse_y = float(std::stoi(a[3]));
  } else if (a[0] == "motion") {
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.timestamp = now;
    ev.motion.windowID = window_id;
    ev.motion.x = float(std::stoi(a[1]));
    ev.motion.y = float(std::stoi(a[2]));
  } else if (a[0] == "focus") {
    ev.type = a[1] == "on" ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    ev.window.timestamp = now;
    ev.window.windowID = window_id;
  } else if (a[0] == "resize") {
    rex::ui::Window* window = driver ? driver->window() : nullptr;
    if (!window) {
      REXLOG_ERROR("[kuf-input-script] '{}': no window", e.text);
      return;
    }
    const int w = std::stoi(a[1]), h = std::stoi(a[2]);
    window->app_context().CallInUIThread([window_id, w, h] {
      SDL_SetWindowSize(SDL_GetWindowFromID(window_id), w, h);
      SDL_SyncWindow(SDL_GetWindowFromID(window_id));
    });
    return;
  }
  if (!SDL_PushEvent(&ev)) {
    REXLOG_ERROR("[kuf-input-script] SDL_PushEvent failed: {}", SDL_GetError());
  }
}

void Apply(uint64_t frame, const Event& e) {
  REXLOG_INFO("[kuf-input-script] tick {}: {}", frame, e.text);
  const auto& a = e.args;
  if (e.verb == "log") {
    // The frames presented so far, so a test can measure the frame rate between marks (D5, frame_rate_check.py).
    REXLOG_INFO("[kuf-frames] {}: {} frames presented", e.text, kuf::pacing::FramesDrawn());
    return;
  }
  if (e.verb == "capture") {
    kuf::capture::RequestLabeled(frame, a[0]);
    return;
  }
  if (e.verb == "freeze") {
    kuf::pacing::RequestFreeze(std::stoi(a[0]), a[1], frame);
    return;
  }
  if (e.verb == "trace") {
    kuf::pacing::RequestGpuTrace(frame);
    return;
  }
  if (e.verb == "pad") {
    if (a[0] == "connect") {
      PadConnect();
    } else if (a[0] == "disconnect") {
      PadDisconnect();
    } else if (!g_pad) {
      REXLOG_ERROR("[kuf-input-script] '{}' without a connected virtual pad", e.text);
    } else if (a[0] == "down" || a[0] == "up") {
      SDL_SetJoystickVirtualButton(g_pad, PadButton(a[1]), a[0] == "down");
    } else if (a[0] == "axis") {
      SDL_SetJoystickVirtualAxis(g_pad, PadAxis(a[1]), static_cast<Sint16>(std::stol(a[2])));
    }
    return;
  }
  KufInputDriver* driver = KufInputDriver::Instance();
  if (e.verb == "sdl") {
    ApplySdl(e, driver);
    return;
  }
  if (!driver) {
    REXLOG_ERROR("[kuf-input-script] no keyboard driver (--kuf_keyboard=false?)");
    return;
  }
  if (e.verb == "down" || e.verb == "up") {
    driver->InjectKey(ParseHostKey(a[0]), e.verb == "down");
  } else if (e.verb == "wheel") {
    driver->InjectWheel(std::stoi(a[0]));
  } else if (e.verb == "focus") {
    driver->InjectFocus(a[0] == "on");
  } else if (e.verb == "look") {
    driver->InjectLook(std::stoi(a[0]), std::stoi(a[1]));
  } else if (e.verb == "pointer") {
    rex::ui::Window* window = driver->window();
    GuestPoint p;
    const bool inside = driver->PointerInGuest(&p);
    REXLOG_INFO("[kuf-input-script] pointer {} (window {}x{})",
                inside ? fmt::format("at guest ({}, {})", p.x, p.y) : std::string("outside the guest image"),
                window ? window->GetActualPhysicalWidth() : 0, window ? window->GetActualPhysicalHeight() : 0);
  } else if (e.verb == "mouse") {
    driver->InjectMouseMove(std::stoi(a[0]), std::stoi(a[1]), std::stoi(a[2]), std::stoi(a[3]));
    GuestPoint p;
    if (driver->PointerInGuest(&p)) {
      REXLOG_INFO("[kuf-input-script] pointer at guest ({}, {})", p.x, p.y);
    } else {
      REXLOG_INFO("[kuf-input-script] pointer outside the guest image");
    }
  }
}

}  // namespace

bool Start(std::string* error) {
  std::lock_guard lock(g_mutex);
  std::string text = REXCVAR_GET(kuf_input_script);
  const std::string file = REXCVAR_GET(kuf_input_script_file);
  if (!file.empty()) {
    if (!text.empty()) {
      *error = "input script: give kuf_input_script or kuf_input_script_file, not both";
      return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      *error = "input script: cannot read " + file;
      return false;
    }
    constexpr std::streamsize kMaxBytes = 4 << 20;
    std::string file_text(kMaxBytes + 1, '\0');
    in.read(file_text.data(), kMaxBytes + 1);
    if (in.gcount() > kMaxBytes) {
      *error = "input script: " + file + " is larger than 4 MiB";
      return false;
    }
    file_text.resize(static_cast<size_t>(in.gcount()));
    text = ScriptFromFileText(file_text);
    REXLOG_INFO("[kuf-input-script] script file {}", file);
  }
  const int n = Parse(text, g_events, error);
  if (n < 0) {
    return false;
  }
  for (auto it = g_events.begin(); it != g_events.end();) {
    if (it->second.anchor_count > 0) {
      g_anchored.emplace_back(it->first, std::move(it->second));
      it = g_events.erase(it);
    } else if (IsPresentation(it->second)) {
      g_present_events.emplace(it->first, std::move(it->second));
      it = g_events.erase(it);
    } else {
      ++it;
    }
  }
  if (n > 0) {
    REXLOG_INFO("[kuf-input-script] {} scripted input items", n);
  }
  return true;
}

void Stop() {
  std::lock_guard lock(g_mutex);
  g_events.clear();
  g_present_events.clear();
  g_anchored.clear();
  PadDisconnect();
}

void OnGameTick(uint64_t tick) {
  std::lock_guard lock(g_mutex);
  g_tick = tick;
  std::vector<std::pair<uint64_t, Event>> due;
  TakeDue(g_events, tick, g_context_entries, due);
  bool pad = false;
  for (const auto& [when, e] : due) {
    pad = pad || e.verb == "pad";
    Apply(when, e);
  }
  // The virtual pad's new state reaches the SDK's SDL driver as SDL gamepad events, which SDL generates when it
  // updates its joysticks (normally in the UI thread's event pump, at its own time). Updated here, the events reach
  // the driver's event watch on this thread now, so this tick's poll sees them.
  if (pad && g_pad) {
    SDL_UpdateJoysticks();
  }
}

uint64_t CurrentTick() {
  std::lock_guard lock(g_mutex);
  return g_tick;
}

void OnFramePresented(uint64_t frame) {
  std::lock_guard lock(g_mutex);
  while (!g_present_events.empty() && g_present_events.begin()->first <= frame) {
    auto node = g_present_events.extract(g_present_events.begin());
    Apply(node.key(), node.mapped());
  }
}

}  // namespace kuf::input::script

namespace kuf::input::script {

void OnContextEntered(const char* context) {
  std::lock_guard lock(g_mutex);
  const uint64_t tick = g_tick;
  const int n = ++g_context_entries[context];
  int scheduled = 0;
  for (auto it = g_anchored.begin(); it != g_anchored.end();) {
    if (it->second.anchor_context == context && it->second.anchor_count == n) {
      QueueFor(it->second).emplace(tick + it->first, std::move(it->second));
      it = g_anchored.erase(it);
      ++scheduled;
    } else {
      ++it;
    }
  }
  if (scheduled) {
    REXLOG_INFO("[kuf-input-script] @{}#{} at tick {}: {} events scheduled", context, n, tick, scheduled);
  }
}

}  // namespace kuf::input::script
