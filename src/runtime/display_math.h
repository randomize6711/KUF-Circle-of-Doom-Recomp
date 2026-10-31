// Display math (M6C), pure and unit-tested (tests/display): the output's aspect, the widened 3D projection, the
// 16:9 box the HUD and menus are drawn in, the render scale, the pointer mapping and the resolution lists. The
// game-facing part is display.cpp; docs/DISPLAY.md explains both.
//
// The game renders into a 1280x720 frame (16:9). For another output aspect the port keeps that frame size and
// changes what is drawn into it: the 3D views get the output's aspect (more of the world to the sides on wider
// screens, Hor+; more above and below on narrower ones, Vert+: the 16:9 view is always fully visible), the HUD
// and menus are compressed into a centred 16:9 box, and the presenter stretches the frame to fill the output.
// The 3D image is then geometrically correct (nothing is stretched), and the HUD and menus keep their shapes.
// The frame is rendered at a per-axis multiple of 1280x720 (the SDK's resolution scale), at least the output's
// size, so the stretch does not cost sharpness.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace kuf::display {

constexpr uint32_t kGuestWidth = 1280, kGuestHeight = 720;
constexpr double kGuestAspect = 16.0 / 9.0;

// The output's aspect (width / height) the game draws for: the output's own, or 16:9 when `widescreen` is off
// (the original look: the presenter then pillarboxes or letterboxes). Degenerate sizes give 16:9.
double OutputAspect(uint32_t width, uint32_t height, bool widescreen);

// True if `aspect` is 16:9 (within rounding of real display sizes such as 1366x768).
bool IsGuestAspect(double aspect);

// A 3D view's projection for the output aspect. The game's projection has vertical field of view `fov` (radians)
// and aspect 16:9. Wider outputs keep the vertical field of view and widen the horizontal one (Hor+); narrower
// ones keep the horizontal one and widen the vertical one (Vert+).
struct Projection {
  double fov;     // vertical, radians
  double aspect;
};
Projection WidenProjection(double fov, double output_aspect);

// The HUD's and menus' orthographic projection is scaled by these (clip space, about the centre) so they fill a
// centred 16:9 box of the output: x < 1 on wider outputs, y < 1 on narrower ones.
struct UiScale {
  double x = 1, y = 1;
};
UiScale UiBoxScale(double output_aspect);

// The rectangle of the output (window pixels) the 16:9 HUD and menus fill.
struct Rect {
  double x = 0, y = 0, width = 0, height = 0;
};
Rect UiBox(uint32_t output_width, uint32_t output_height);

// A window position (pixels) to HUD and menu coordinates (0..1280, 0..720): through the UI box. False outside it.
struct Point {
  int32_t x = 0, y = 0;
};
bool WindowToUi(uint32_t output_width, uint32_t output_height, int32_t x, int32_t y, Point* out);

// The resolution scale (per axis, 1..kMaxRenderScale) that renders the 1280x720 frame at least at the output's
// size along each axis.
constexpr uint32_t kMaxRenderScale = 7;  // the SDK's limit (TextureCache::kMaxDrawResolutionScaleAlongAxis)
struct RenderScale {
  uint32_t x = 1, y = 1;
  bool operator==(const RenderScale&) const = default;
};
RenderScale RenderScaleFor(uint32_t output_width, uint32_t output_height);
// The render scale of the PC setting [display] render_scale: 0 (native) = RenderScaleFor the output, N = N per axis.
RenderScale RenderScaleForSetting(int render_scale, uint32_t output_width, uint32_t output_height);
// "Native" or "2x" (the menu's and the log's name of a render_scale value).
std::string RenderSettingName(int render_scale);

// Stores a window size into the two atomics the render and input threads read: always both. True if either
// changed. (A resize changes both at once; skipping the height when the width changed drew a 3840x2160 window
// for the aspect of 3840x1043, the size before it.)
bool StoreWindowSize(std::atomic<uint32_t>& width, std::atomic<uint32_t>& height, uint32_t w, uint32_t h);

// --- Resolutions ----------------------------------------------------------------------------------------
struct Mode {
  uint32_t width = 0, height = 0;
  uint32_t refresh_mhz = 0;  // refresh rate in millihertz (0: unknown / any)
  bool operator==(const Mode&) const = default;
};

// "16:9", "21:9", ... for a size: the nearest common name within 1.5 %, else the reduced ratio.
std::string AspectName(uint32_t width, uint32_t height);

// The fullscreen choices from a monitor's modes (as SDL reports them, any order): one entry per size and refresh
// rate, largest first, refresh rates highest first; duplicates removed.
std::vector<Mode> FullscreenModes(const std::vector<Mode>& modes);

// The window sizes offered for a monitor: each distinct size of its modes (refresh ignored) that fits in its
// usable area (`usable_width` x `usable_height`, the desktop less panels), largest first; plus the guest size
// 1280x720 if it fits and is missing. Sizes are in pixels.
std::vector<Mode> WindowSizes(const std::vector<Mode>& modes, uint32_t usable_width, uint32_t usable_height);

// The entry of `choices` for a saved size and refresh rate: the exact one, else the same size with the nearest
// refresh rate, else -1.
int FindMode(const std::vector<Mode>& choices, uint32_t width, uint32_t height, uint32_t refresh_mhz);

// "2560 x 1440", "2560 x 1440 120 Hz" (refresh rounded to 0.01 Hz, trailing zeros dropped).
std::string ModeName(const Mode& mode, bool with_refresh);

}  // namespace kuf::display
