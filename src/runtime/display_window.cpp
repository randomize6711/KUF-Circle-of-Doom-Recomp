// Display (M6C; display.h): the window, the monitors and their modes, the output size and the render scale.
//
// Display modes (PC Settings > Display, pc_settings.toml [display]):
//   windowed    a window of the chosen size (pixels), centred on the chosen monitor; it can be resized and the
//               game follows (the output is the window's client area);
//   borderless  a borderless window covering the monitor at its desktop resolution (SDL "fullscreen desktop");
//   fullscreen  exclusive fullscreen in one of the monitor's display modes (SDL_SetWindowFullscreenMode). On
//               Wayland compositors there is no real mode change: SDL emulates it and the compositor scales the
//               window to the monitor; on X11 and Windows the monitor switches mode.
// The window calls happen on the UI thread (SDL); the menu waits for them. The game's output size is the window's
// client area in pixels, taken from every resize, so a monitor change, a compositor resize or an Alt+Tab that
// changes the window are followed at once (the HUD box and the 3D aspect use it from the next frame).
//
// Render resolution: the game draws one 1280x720 frame (its render targets, resolves and post-processing are all
// built around it). The SDK renders that frame at a whole multiple per axis (draw_resolution_scale_x / _y, fixed
// at start): every render target, resolve and texture made from one is that many times larger, so the 3D scene,
// its effects, the HUD and the menus are all drawn at that resolution. PC Settings > Display > Render
// ([display] render_scale): Native (0, default) takes the smallest multiple that is at least the output's size on
// each axis, i.e. exactly the output at 2560x1440, 3840x2160 and 5120x1440, and more (filtered down by the
// presenter) for other sizes; 1x-4x are fixed. It takes effect at the next start: the game keeps data it renders
// once (its lighting cube maps, at level load) only in the scaled form. An explicit --draw_resolution_scale_x/_y
// wins. (Scales above 1 tinted the scene red until the SDK's pixel position was fixed, KNOWN_ISSUES K31.)

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include "runtime/display.h"

REXCVAR_DECLARE(bool, present_letterbox);

REXCVAR_DEFINE_STRING(kuf_display_size, "", "KUF/Display",
                      "Draw for an output of this size (WIDTHxHEIGHT, e.g. 3440x1440) instead of the window's: the "
                      "aspect, the HUD box, the pointer mapping and the render scale follow it (tests; offscreen runs)");
REXCVAR_DEFINE_STRING(kuf_display_modes, "", "KUF/Display",
                      "Simulate one monitor with these modes instead of the system's: WIDTHxHEIGHT@HZ,... (the first is "
                      "its desktop mode) (tests)");

namespace kuf::display {
namespace {

std::atomic<uint32_t> g_window_width{0}, g_window_height{0};
uint32_t g_forced_width = 0, g_forced_height = 0;  // --kuf_display_size
rex::ui::Window* g_window = nullptr;
RenderScale g_start_scale;
std::atomic<int> g_letterbox{-1};

bool ParseSize(const std::string& text, uint32_t* w, uint32_t* h) {
  unsigned a = 0, b = 0;
  char x = 0, extra = 0;
  if (std::sscanf(text.c_str(), "%u%c%u%c", &a, &x, &b, &extra) != 3 || (x != 'x' && x != 'X') || !a || !b ||
      a > 16384 || b > 16384) {
    return false;
  }
  *w = a;
  *h = b;
  return true;
}

// "3440x1440@100,2560x1080@59.94" -> modes (the first is the desktop).
std::vector<Mode> ParseModes(const std::string& text) {
  std::vector<Mode> modes;
  std::stringstream in(text);
  std::string item;
  while (std::getline(in, item, ',')) {
    unsigned w = 0, h = 0;
    double hz = 0;
    if (std::sscanf(item.c_str(), " %ux%u@%lf", &w, &h, &hz) == 3 && w && h && hz > 0) {
      modes.push_back({w, h, uint32_t(std::lround(hz * 1000))});
    } else {
      REXLOG_ERROR("[kuf-display] --kuf_display_modes: '{}' ignored (expected WIDTHxHEIGHT@HZ)", item);
    }
  }
  return modes;
}

uint32_t Millihertz(const SDL_DisplayMode& m) {
  if (m.refresh_rate_numerator > 0 && m.refresh_rate_denominator > 0) {
    return uint32_t((int64_t(m.refresh_rate_numerator) * 1000 + m.refresh_rate_denominator / 2) /
                    m.refresh_rate_denominator);
  }
  return uint32_t(std::lround(m.refresh_rate * 1000));
}

Mode ToMode(const SDL_DisplayMode& m) {
  const float density = m.pixel_density > 0 ? m.pixel_density : 1.0f;
  return {uint32_t(std::lround(m.w * density)), uint32_t(std::lround(m.h * density)), Millihertz(m)};
}

// On the UI thread.
std::vector<Monitor> QueryMonitors() {
  std::vector<Monitor> out;
  const std::string simulated = REXCVAR_GET(kuf_display_modes);
  if (!simulated.empty()) {
    Monitor m;
    m.name = "Simulated";
    m.modes = ParseModes(simulated);
    if (!m.modes.empty()) {
      m.desktop = m.modes.front();
      m.modes = FullscreenModes(m.modes);
      m.usable_width = m.desktop.width;
      m.usable_height = m.desktop.height;
      m.primary = true;
      out.push_back(std::move(m));
      return out;
    }
  }
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
  for (int i = 0; displays && i < count; ++i) {
    Monitor m;
    m.id = displays[i];
    const char* name = SDL_GetDisplayName(m.id);
    m.name = name ? name : "Display " + std::to_string(i + 1);
    m.primary = m.id == primary;
    if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(m.id)) {
      m.desktop = ToMode(*desktop);
    }
    int n = 0;
    std::vector<Mode> modes;
    if (SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(m.id, &n)) {
      for (int j = 0; j < n; ++j) {
        modes.push_back(ToMode(*list[j]));
      }
      SDL_free(list);
    }
    if (modes.empty() && m.desktop.width) {
      modes.push_back(m.desktop);
    }
    m.modes = FullscreenModes(modes);
    SDL_Rect usable{};
    const float scale = SDL_GetDisplayContentScale(m.id) > 0 ? SDL_GetDisplayContentScale(m.id) : 1.0f;
    if (SDL_GetDisplayUsableBounds(m.id, &usable)) {
      m.usable_width = uint32_t(std::lround(usable.w * scale));
      m.usable_height = uint32_t(std::lround(usable.h * scale));
    } else {
      m.usable_width = m.desktop.width;
      m.usable_height = m.desktop.height;
    }
    out.push_back(std::move(m));
  }
  SDL_free(displays);
  return out;
}

rex::ui::Presenter* g_presenter = nullptr;  // the graphics system's (VSync: its swapchain recreated)

template <typename F>
void OnUiThread(F&& f) {
  if (g_window && !g_window->app_context().IsInUIThread()) {
    g_window->app_context().CallInUIThreadSynchronous(std::forward<F>(f));
  } else {
    f();
  }
}

SDL_Window* SdlWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* w = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  return w;
}

// The output size the settings give on `monitor` (pixels).
Mode TargetOutput(const kuf::pc::DisplaySettings& d, const Monitor* monitor) {
  using kuf::pc::DisplayMode;
  switch (d.mode) {
    case DisplayMode::kWindowed:
      return d.width ? Mode{uint32_t(d.width), uint32_t(d.height), 0} : Mode{kGuestWidth, kGuestHeight, 0};
    case DisplayMode::kFullscreen:
      if (d.width) {
        return {uint32_t(d.width), uint32_t(d.height), uint32_t(d.refresh_mhz)};
      }
      [[fallthrough]];
    default:
      return monitor ? monitor->desktop : Mode{kGuestWidth, kGuestHeight, 0};
  }
}

void UpdatePresenter() {
  const int letterbox = IsGuestAspect(CurrentOutput().aspect) ? 1 : 0;
  if (g_letterbox.exchange(letterbox) != letterbox) {
    REXCVAR_SET(present_letterbox, letterbox != 0);
    REXLOG_INFO("[kuf-display] presented {}", letterbox ? "at 16:9 (bars where the output is wider or taller)"
                                                        : "stretched to the output (drawn for its aspect)");
  }
}

void TakeWindowSize(const char* why) {
  if (!g_window) {
    return;
  }
  const uint32_t w = g_window->GetActualPhysicalWidth(), h = g_window->GetActualPhysicalHeight();
  const bool changed = StoreWindowSize(g_window_width, g_window_height, w, h);
  UpdatePresenter();
  if (changed && w && h) {
    const Output out = CurrentOutput();
    const RenderScale need = NeededRenderScale();
    REXLOG_INFO("[kuf-display] window {} {}x{} ({}){}: drawn for aspect {:.4f}{}", why, w, h, AspectName(w, h),
                g_forced_width ? " (output simulated: --kuf_display_size)" : "", out.aspect,
                need == g_start_scale
                    ? std::string()
                    : fmt::format("; rendered at {}x{} until the next start (the Render setting needs {}x{})",
                                  kGuestWidth * g_start_scale.x, kGuestHeight * g_start_scale.y,
                                  kGuestWidth * need.x, kGuestHeight * need.y));
  }
}

class Listener : public rex::ui::WindowListener {
 public:
  void OnResize(rex::ui::UISetupEvent&) override { TakeWindowSize("resized"); }
  void OnDpiChanged(rex::ui::UISetupEvent&) override { TakeWindowSize("scale changed"); }
  void OnRestored(rex::ui::UIEvent&) override { TakeWindowSize("restored"); }
};
Listener g_listener;

}  // namespace

Output CurrentOutput() {
  Output out;
  if (g_forced_width) {
    out.width = g_forced_width;
    out.height = g_forced_height;
  } else {
    out.width = g_window_width.load();
    out.height = g_window_height.load();
  }
  out.aspect = OutputAspect(out.width, out.height, kuf::pc::Current().display.widescreen && !MovieOnScreen());
  return out;
}

void Refresh() { UpdatePresenter(); }

std::vector<Monitor> Monitors() {
  std::vector<Monitor> monitors;
  OnUiThread([&] { monitors = QueryMonitors(); });
  return monitors;
}

size_t FindMonitor(const std::vector<Monitor>& monitors, const std::string& name, bool* fallback) {
  *fallback = false;
  for (size_t i = 0; i < monitors.size(); ++i) {
    if (!name.empty() && monitors[i].name == name) {
      return i;
    }
  }
  *fallback = !name.empty();
  for (size_t i = 0; i < monitors.size(); ++i) {
    if (monitors[i].primary) {
      return i;
    }
  }
  return 0;
}

RenderScale StartRenderScale() { return g_start_scale; }

RenderScale NeededRenderScale() {
  const Output out = CurrentOutput();
  return RenderScaleForSetting(kuf::pc::Current().display.render_scale, out.width, out.height);
}

void SetPresenter(rex::ui::Presenter* presenter) { g_presenter = presenter; }

void SetVsync(bool on) {
  rex::cvar::SetFlagByName("present_vsync", on ? "true" : "false");
  REXLOG_INFO("[kuf-display] vsync {}: the next swapchain presents {}", on ? "on" : "off",
              on ? "at the display's refresh (FIFO)" : "as frames come (immediate, or the fastest mode available)");
  if (!g_window || !g_presenter) return;  // before the window: the first swapchain is created with it
  OnUiThread([] {
    if (g_presenter) g_presenter->OnSurfaceResizeFromUIThread();
  });
}

void Configure() {
  rex::cvar::SetFlagByName("present_vsync", kuf::pc::Current().display.vsync ? "true" : "false");
  if (!ParseSize(REXCVAR_GET(kuf_display_size), &g_forced_width, &g_forced_height) &&
      !REXCVAR_GET(kuf_display_size).empty()) {
    REXLOG_ERROR("[kuf-display] --kuf_display_size={} ignored: expected WIDTHxHEIGHT", REXCVAR_GET(kuf_display_size));
  }
  const auto display = kuf::pc::Current().display;
  Mode target{g_forced_width, g_forced_height, 0};
  std::string monitor_name;
  if (!g_forced_width) {
    const auto monitors = QueryMonitors();
    bool fallback = false;
    const Monitor* monitor = monitors.empty() ? nullptr : &monitors[FindMonitor(monitors, display.monitor, &fallback)];
    if (fallback) {
      REXLOG_WARN("[kuf-display] monitor '{}' is not connected: using '{}'", display.monitor,
                  monitor ? monitor->name : "none");
    }
    monitor_name = monitor ? "'" + monitor->name + "'" : "no monitor";
    target = TargetOutput(display, monitor);
  }
  // The GPU plugin registers draw_resolution_scale_x / _y when it loads, after this; values for it are passed the
  // way the SDK applies them at registration: REX_<NAME> environment variables, below the command line (an
  // explicit --draw_resolution_scale_x / _y still wins; Start logs the scale in effect).
  const bool from_environment = std::getenv("REX_DRAW_RESOLUTION_SCALE_X") || std::getenv("REX_DRAW_RESOLUTION_SCALE_Y");
  if (!from_environment) {
    g_start_scale = RenderScaleForSetting(display.render_scale, target.width, target.height);
    setenv("REX_DRAW_RESOLUTION_SCALE_X", std::to_string(g_start_scale.x).c_str(), 1);
    setenv("REX_DRAW_RESOLUTION_SCALE_Y", std::to_string(g_start_scale.y).c_str(), 1);
  }
  const std::string mode =
      std::string(kuf::pc::DisplayModeName(display.mode)) + " " +
      (g_forced_width ? (display.width ? std::to_string(display.width) + "x" + std::to_string(display.height)
                                       : std::string("(desktop size)")) +
                            ", output simulated " + ModeName(target, false)
                      : ModeName(target, display.mode == kuf::pc::DisplayMode::kFullscreen) + " on " + monitor_name);
  REXLOG_INFO("[kuf-display] start: {}: render scale {}x{} (frame {}x{}); Render {}{}", mode, g_start_scale.x,
              g_start_scale.y, kGuestWidth * g_start_scale.x, kGuestHeight * g_start_scale.y,
              RenderSettingName(display.render_scale), from_environment ? " (scale from the environment)" : "");
}

bool Apply(const kuf::pc::DisplaySettings& d, std::string* result) {
  using kuf::pc::DisplayMode;
  if (!g_window) {  // not started, or shutting down (a confirmation timer may still fire)
    *result = "no window";
    return false;
  }
  const auto started = std::chrono::steady_clock::now();
  bool ok = false;
  OnUiThread([&] {
    SDL_Window* w = SdlWindow();
    if (!w || !g_window) {
      *result = "no window";
      return;
    }
    const auto monitors = QueryMonitors();
    if (monitors.empty()) {
      *result = "no monitor";
      return;
    }
    bool fallback = false;
    const Monitor& monitor = monitors[FindMonitor(monitors, d.monitor, &fallback)];
    const std::string where = "'" + monitor.name + "'" + (fallback ? " (the saved monitor is not connected)" : "");
    const SDL_DisplayID id = monitor.id ? monitor.id : SDL_GetDisplayForWindow(w);
    const bool on_monitor = SDL_GetDisplayForWindow(w) == id;
    if (!on_monitor && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) {
      g_window->SetFullscreen(false);  // leave fullscreen to move to the other monitor
      SDL_SyncWindow(w);
    }
    if (!on_monitor) {
      SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(id), SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    }
    switch (d.mode) {
      case DisplayMode::kWindowed: {
        Mode size = TargetOutput(d, &monitor);
        size.width = std::min(size.width, monitor.usable_width ? monitor.usable_width : size.width);
        size.height = std::min(size.height, monitor.usable_height ? monitor.usable_height : size.height);
        g_window->SetFullscreen(false);
        SDL_SetWindowFullscreen(w, false);
        SDL_SyncWindow(w);
        // SDL window sizes are in the desktop's units (points on scaled Wayland desktops).
        const float density = SDL_GetWindowPixelDensity(w) > 0 ? SDL_GetWindowPixelDensity(w) : 1.0f;
        SDL_SetWindowSize(w, int(std::lround(size.width / density)), int(std::lround(size.height / density)));
        SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(id), SDL_WINDOWPOS_CENTERED_DISPLAY(id));
        *result = "windowed " + ModeName(size, false) + " on " + where;
        break;
      }
      case DisplayMode::kFullscreen: {
        const SDL_DisplayMode* chosen = nullptr;
        int n = 0;
        SDL_DisplayMode** list = monitor.id ? SDL_GetFullscreenDisplayModes(id, &n) : nullptr;
        if (d.width) {
          std::vector<Mode> modes;
          for (int j = 0; j < n; ++j) modes.push_back(ToMode(*list[j]));
          const int found = FindMode(modes, uint32_t(d.width), uint32_t(d.height), uint32_t(d.refresh_mhz));
          if (found >= 0) chosen = list[found];
        } else if (n > 0) {
          chosen = list[0];  // the largest (SDL's order)
        }
        if (!chosen && monitor.id) {
          SDL_free(list);
          SDL_SetWindowFullscreenMode(w, nullptr);
          g_window->SetFullscreen(true);
          SDL_SetWindowFullscreen(w, true);
          *result = "fullscreen " + std::to_string(d.width) + "x" + std::to_string(d.height) +
                    " is not a mode of " + where + ": borderless instead";
          ok = true;
          return;
        }
        SDL_SetWindowFullscreenMode(w, chosen);
        g_window->SetFullscreen(true);
        SDL_SetWindowFullscreen(w, true);
        *result = "fullscreen " + (chosen ? ModeName(ToMode(*chosen), true) : ModeName(TargetOutput(d, &monitor), true)) +
                  " on " + where;
        SDL_free(list);
        break;
      }
      default:
        SDL_SetWindowFullscreenMode(w, nullptr);
        g_window->SetFullscreen(true);
        SDL_SetWindowFullscreen(w, true);
        *result = "borderless " + ModeName(monitor.desktop, false) + " on " + where;
        break;
    }
    SDL_SyncWindow(w);
    ok = true;
  });
  TakeWindowSize("applied");
  REXLOG_INFO("[kuf-display] display: {} ({} ms)", *result,
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
  return ok;
}

void Start(rex::ui::Window* window) {
  // The scale the GPU plugin took (the command line may have overridden ours).
  const RenderScale chosen = g_start_scale;
  g_start_scale = {uint32_t(std::max(1, rex::cvar::Query<int32_t>("draw_resolution_scale_x"))),
                   uint32_t(std::max(1, rex::cvar::Query<int32_t>("draw_resolution_scale_y")))};
  if (!(g_start_scale == chosen)) {
    REXLOG_INFO("[kuf-display] render scale {}x{} (frame {}x{}) from the command line", g_start_scale.x,
                g_start_scale.y, kGuestWidth * g_start_scale.x, kGuestHeight * g_start_scale.y);
  }
  g_window = window;
  if (window) {
    window->AddListener(&g_listener);
    std::string result;
    Apply(kuf::pc::Current().display, &result);
  }
  TakeWindowSize("opened");
  const Output out = CurrentOutput();
  UpdatePresenter();
  REXLOG_INFO("[kuf-display] output {}x{} ({}), drawn for aspect {:.4f}{}", out.width, out.height,
              AspectName(out.width, out.height), out.aspect, g_forced_width ? " (--kuf_display_size)" : "");
}

void Stop() {
  LogUiStats();
  if (g_window) {
    g_window->RemoveListener(&g_listener);
    g_window = nullptr;
  }
}

}  // namespace kuf::display
