// Display (M6C): widescreen and ultrawide rendering, the HUD's and menus' 16:9 box, display modes and
// resolutions (src/runtime/display.cpp; the math is display_math.h; docs/DISPLAY.md).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ppc.h>

#include "runtime/display_math.h"
#include "runtime/pc_settings.h"

namespace rex::ui {
class Presenter;
class Window;
}

namespace kuf::display {

// Before the graphics system starts (OnPreSetup): the render scale for the saved display settings.
void Configure();
// After the window exists (OnPostSetup): applies the saved display mode and follows the window's size.
void Start(rex::ui::Window* window);
void Stop();

// The output the game draws for (window pixels; or --kuf_display_size), and its aspect for the 3D views.
struct Output {
  uint32_t width = 0, height = 0;
  double aspect = 16.0 / 9.0;  // 16:9 when widescreen is off, a movie is on screen or the output is 16:9
};
Output CurrentOutput();

// A monitor as the system reports it (or simulated with --kuf_display_modes). Sizes in pixels.
struct Monitor {
  uint32_t id = 0;            // SDL display id (0: simulated)
  std::string name;
  Mode desktop;               // its current (desktop) mode
  std::vector<Mode> modes;    // its fullscreen modes (FullscreenModes order)
  uint32_t usable_width = 0, usable_height = 0;  // the desktop less panels
  bool primary = false;
};
// The connected monitors (queried on the UI thread; any thread may call this).
std::vector<Monitor> Monitors();
// The monitor `name` stands for: that one if connected, else the primary one (*fallback set). Index into `monitors`.
size_t FindMonitor(const std::vector<Monitor>& monitors, const std::string& name, bool* fallback);

// Applies a display mode / size / monitor (any thread; done on the UI thread, waited for). *result describes what
// was applied (or why not). The output size follows the window; widescreen takes effect at once by itself.
bool Apply(const kuf::pc::DisplaySettings& display, std::string* result);

// The output's aspect may have changed (Widescreen toggled, a movie started or stopped): updates the presenter.
void Refresh();
// Host vertical sync (M7 UX fix 3; SDK patch 0011 --present_vsync): the swapchain is recreated with FIFO (on) or the
// fastest mode (off) through the presenter's resize path, on the UI thread. Presentation only.
void SetVsync(bool on);
void SetPresenter(rex::ui::Presenter* presenter);
// A movie is on screen: the output is treated as 16:9 (pillarboxed) until it stops.
bool MovieOnScreen();

// The render scale chosen at start (the frame is 1280x720 times this), and the one the current output would need.
RenderScale StartRenderScale();
RenderScale NeededRenderScale();

// The game's perspective projection builder (sub_826FC108) is about to run (its hook is in modern_camera.cpp):
// widens f1 / f2 for the 3D views when the output is not 16:9.
void BeforePerspective(PPCContext& ctx, uint8_t* base);
// How many 2D quads the HUD box moved (logged at exit).
void LogUiStats();

}  // namespace kuf::display
