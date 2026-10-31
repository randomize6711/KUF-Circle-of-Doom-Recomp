// Display math (src/runtime/display_math.cpp): aspect, widened projection, the HUD box, pointer mapping, render
// scale, resolution lists. The outputs are the common PC shapes (16:9, 16:10, 4:3, 5:4, 21:9, 32:9) at their usual
// sizes; the monitors' mode lists are simulated (real ones come from SDL at run time).

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "input/keyboard_pad.h"
#include "runtime/display_math.h"

using Catch::Approx;
using namespace kuf::display;

namespace {

double HorizontalFov(double vertical, double aspect) { return 2 * std::atan(std::tan(vertical / 2) * aspect); }

struct Output {
  uint32_t w, h;
  const char* name;
};
const Output kOutputs[] = {{1280, 720, "16:9"},   {1920, 1080, "16:9"}, {2560, 1440, "16:9"}, {3840, 2160, "16:9"},
                           {1366, 768, "16:9"},   {1920, 1200, "16:10"}, {2560, 1600, "16:10"}, {1024, 768, "4:3"},
                           {1600, 1200, "4:3"},   {1280, 1024, "5:4"},  {2560, 1080, "21:9"},  {3440, 1440, "21:9"},
                           {5120, 2160, "21:9"},  {3840, 1080, "32:9"}, {5120, 1440, "32:9"}, {3840, 1200, "32:10"}};

}  // namespace

TEST_CASE("aspect names of common sizes", "[display]") {
  for (const auto& o : kOutputs) {
    INFO(o.w << "x" << o.h);
    CHECK(AspectName(o.w, o.h) == o.name);
  }
  CHECK(AspectName(1000, 300) == "10:3");  // no common name: the reduced ratio
  CHECK(AspectName(0, 720) == "?");
}

TEST_CASE("output aspect: the output's own, or 16:9 with widescreen off or no size", "[display]") {
  CHECK(OutputAspect(3440, 1440, true) == Approx(3440.0 / 1440));
  CHECK(OutputAspect(3440, 1440, false) == Approx(16.0 / 9));
  CHECK(OutputAspect(0, 0, true) == Approx(16.0 / 9));
  CHECK(IsGuestAspect(OutputAspect(1366, 768, true)));  // 1366x768 is 16:9 to within rounding
  CHECK_FALSE(IsGuestAspect(OutputAspect(1920, 1200, true)));
}

TEST_CASE("wider outputs keep the vertical angle and widen the horizontal one (Hor+)", "[display]") {
  const double fov = 0.6;  // the game's own vertical angle
  for (const auto& o : kOutputs) {
    const double aspect = double(o.w) / o.h;
    if (aspect <= 16.0 / 9 || IsGuestAspect(aspect)) continue;
    INFO(o.w << "x" << o.h);
    const Projection p = WidenProjection(fov, aspect);
    CHECK(p.fov == Approx(fov));
    CHECK(p.aspect == Approx(aspect));
    CHECK(HorizontalFov(p.fov, p.aspect) > HorizontalFov(fov, 16.0 / 9));
  }
  // 21:9 (3440x1440): 57.6 degrees at 16:9 become 72.9.
  CHECK(HorizontalFov(WidenProjection(0.6, 3440.0 / 1440).fov, 3440.0 / 1440) * 180 / M_PI ==
        Approx(72.9).margin(0.1));
}

TEST_CASE("narrower outputs keep the 16:9 horizontal angle and widen the vertical one (Vert+)", "[display]") {
  const double fov = 0.6;
  for (const auto& o : kOutputs) {
    const double aspect = double(o.w) / o.h;
    if (aspect >= 16.0 / 9 || IsGuestAspect(aspect)) continue;
    INFO(o.w << "x" << o.h);
    const Projection p = WidenProjection(fov, aspect);
    CHECK(p.aspect == Approx(aspect));
    CHECK(p.fov > fov);
    CHECK(HorizontalFov(p.fov, p.aspect) == Approx(HorizontalFov(fov, 16.0 / 9)));
  }
}

TEST_CASE("16:9 outputs leave the projection exactly as the game's", "[display]") {
  const Projection p = WidenProjection(0.6, 16.0 / 9);
  CHECK(p.fov == 0.6);
  CHECK(p.aspect == 16.0 / 9);
  const UiScale s = UiBoxScale(1920.0 / 1080);
  CHECK(s.x == 1);
  CHECK(s.y == 1);
}

TEST_CASE("the HUD box is a centred 16:9 rectangle filling one side of the output", "[display]") {
  for (const auto& o : kOutputs) {
    INFO(o.w << "x" << o.h);
    const Rect box = UiBox(o.w, o.h);
    CHECK(box.width / box.height == Approx(16.0 / 9).epsilon(0.002));  // 1366x768 counts as 16:9
    CHECK(box.x + box.width / 2 == Approx(o.w / 2.0));
    CHECK(box.y + box.height / 2 == Approx(o.h / 2.0));
    const bool wider = double(o.w) / o.h > 16.0 / 9;
    CHECK((wider ? box.height : box.width) == Approx(wider ? o.h : o.w));
    CHECK(box.x >= 0);
    CHECK(box.y >= 0);
    // The scale the 2D vertices get is the box's share of the output.
    const UiScale s = UiBoxScale(double(o.w) / o.h);
    CHECK(s.x == Approx(box.width / o.w));
    CHECK(s.y == Approx(box.height / o.h));
  }
  // 32:9: the box is the middle half.
  const Rect super = UiBox(5120, 1440);
  CHECK(super.x == Approx(1280));
  CHECK(super.width == Approx(2560));
}

TEST_CASE("the pointer maps through the HUD box, the same as the input driver's 16:9 fit", "[display]") {
  for (const auto& o : kOutputs) {
    INFO(o.w << "x" << o.h);
    const Rect box = UiBox(o.w, o.h);
    Point p;
    // The box's corners and centre.
    REQUIRE(WindowToUi(o.w, o.h, int32_t(std::ceil(box.x)), int32_t(std::ceil(box.y)), &p));
    CHECK(p.x <= 1);
    CHECK(p.y <= 1);
    REQUIRE(WindowToUi(o.w, o.h, int32_t(o.w / 2), int32_t(o.h / 2), &p));
    CHECK(std::abs(p.x - 640) <= 1);
    CHECK(std::abs(p.y - 360) <= 1);
    REQUIRE(WindowToUi(o.w, o.h, int32_t(std::floor(box.x + box.width - 1)), int32_t(std::floor(box.y + box.height - 1)),
                       &p));
    CHECK(p.x >= 1278);
    CHECK(p.y >= 718);
    // Outside the box (the side bars of an ultrawide output) nothing.
    if (box.x >= 1) {
      CHECK_FALSE(WindowToUi(o.w, o.h, int32_t(box.x) - 1, int32_t(o.h / 2), &p));
    }
    // The input driver's mapping (keyboard_pad.cpp, the presenter's 16:9 fit) agrees to within a pixel.
    for (int32_t x : {int32_t(box.x + 3), int32_t(o.w / 3), int32_t(o.w / 2), int32_t(box.x + box.width - 4)}) {
      Point a;
      kuf::input::GuestPoint b;
      const bool in_a = WindowToUi(o.w, o.h, x, int32_t(o.h / 4), &a);
      const bool in_b = kuf::input::WindowToGuest(int32_t(o.w), int32_t(o.h), x, int32_t(o.h / 4), 1280, 720, &b);
      CHECK(in_a == in_b);
      if (in_a && in_b) {
        CHECK(std::abs(a.x - b.x) <= 1);
        CHECK(std::abs(a.y - b.y) <= 1);
      }
    }
  }
}

TEST_CASE("render scale covers the output per axis, within the SDK's limit", "[display]") {
  CHECK(RenderScaleFor(1280, 720) == RenderScale{1, 1});
  CHECK(RenderScaleFor(1920, 1080) == RenderScale{2, 2});
  CHECK(RenderScaleFor(2560, 1440) == RenderScale{2, 2});
  CHECK(RenderScaleFor(3840, 2160) == RenderScale{3, 3});
  CHECK(RenderScaleFor(3440, 1440) == RenderScale{3, 2});
  CHECK(RenderScaleFor(5120, 1440) == RenderScale{4, 2});
  CHECK(RenderScaleFor(1024, 768) == RenderScale{1, 2});
  CHECK(RenderScaleFor(16384, 16384) == RenderScale{kMaxRenderScale, kMaxRenderScale});
  CHECK(RenderScaleFor(0, 0) == RenderScale{1, 1});
}

TEST_CASE("fullscreen modes: deduplicated, largest first, highest refresh first", "[display]") {
  // A 21:9 monitor as SDL could list it (unordered, with a duplicate).
  const std::vector<Mode> sdl = {{2560, 1080, 60000}, {3440, 1440, 59973}, {3440, 1440, 144000},
                                 {1920, 1080, 60000}, {3440, 1440, 144000}, {3440, 1440, 100000},
                                 {1280, 720, 60000}};
  const auto modes = FullscreenModes(sdl);
  REQUIRE(modes.size() == 6);
  CHECK(modes[0] == Mode{3440, 1440, 144000});
  CHECK(modes[1] == Mode{3440, 1440, 100000});
  CHECK(modes[2] == Mode{3440, 1440, 59973});
  CHECK(modes[3] == Mode{2560, 1080, 60000});
  CHECK(modes.back() == Mode{1280, 720, 60000});
}

TEST_CASE("window sizes: each size that fits the usable area once, plus 1280x720", "[display]") {
  // A 32:9 monitor with a 40-pixel panel; no 1280x720 mode.
  const std::vector<Mode> sdl = {{5120, 1440, 240000}, {5120, 1440, 120000}, {3840, 1080, 120000},
                                 {2560, 1440, 120000}, {1920, 1080, 60000}};
  const auto sizes = WindowSizes(sdl, 5120, 1400);
  // 5120x1440 and 2560x1440 do not fit 1400 rows.
  REQUIRE(sizes.size() == 3);
  CHECK(sizes[0] == Mode{3840, 1080, 0});
  CHECK(sizes[1] == Mode{1920, 1080, 0});
  CHECK(sizes[2] == Mode{1280, 720, 0});
  // A small 4:3 monitor: 1280x720 still fits.
  const auto small = WindowSizes({{1024, 768, 60000}, {800, 600, 60000}}, 1024, 740);
  CHECK(small.size() == 1);  // only 800x600 fits (1024x768 is taller than the usable area; 1280 too wide)
  CHECK(small[0] == Mode{800, 600, 0});
}

TEST_CASE("finding a saved mode: exact, else the nearest refresh rate, else none", "[display]") {
  const std::vector<Mode> modes = {{3840, 2160, 120000}, {3840, 2160, 59940}, {2560, 1440, 120000}};
  CHECK(FindMode(modes, 3840, 2160, 59940) == 1);
  CHECK(FindMode(modes, 3840, 2160, 100000) == 0);  // 120 is nearer than 59.94
  CHECK(FindMode(modes, 3840, 2160, 0) == 1);
  CHECK(FindMode(modes, 1920, 1080, 60000) == -1);  // the monitor has no such mode (another monitor's)
}

TEST_CASE("mode names", "[display]") {
  CHECK(ModeName({2560, 1440, 120000}, true) == "2560 x 1440 120 Hz");
  CHECK(ModeName({2560, 1440, 59940}, true) == "2560 x 1440 59.94 Hz");
  CHECK(ModeName({2560, 1440, 144000}, false) == "2560 x 1440");
  CHECK(ModeName({1920, 1080, 0}, true) == "1920 x 1080");
}

TEST_CASE("render scale of the PC setting: native follows the output, 1-4 are fixed", "[display]") {
  CHECK(RenderScaleForSetting(0, 1280, 720) == RenderScale{1, 1});
  CHECK(RenderScaleForSetting(0, 1920, 1080) == RenderScale{2, 2});  // 2560x1440, filtered down to 1080p
  CHECK(RenderScaleForSetting(0, 2560, 1440) == RenderScale{2, 2});  // exactly native
  CHECK(RenderScaleForSetting(0, 3840, 2160) == RenderScale{3, 3});  // exactly native
  CHECK(RenderScaleForSetting(0, 3440, 1440) == RenderScale{3, 2});  // 3840x1440 for 21:9
  CHECK(RenderScaleForSetting(0, 5120, 1440) == RenderScale{4, 2});  // exactly native for 32:9
  CHECK(RenderScaleForSetting(0, 0, 0) == RenderScale{1, 1});        // no output known yet
  CHECK(RenderSettingName(0) == "Native");
  CHECK(RenderSettingName(3) == "3x");
  for (int n = 1; n <= 4; ++n) {
    CHECK(RenderScaleForSetting(n, 5120, 1440) == RenderScale{uint32_t(n), uint32_t(n)});
  }
}

TEST_CASE("a resize stores the window's width and height together", "[display]") {
  std::atomic<uint32_t> width{0}, height{0};
  CHECK(StoreWindowSize(width, height, 1904, 1043));  // the window as first mapped (a tiling compositor's cell)
  CHECK(StoreWindowSize(width, height, 3840, 2160));  // then borderless on the 4K output: both change
  CHECK(width.load() == 3840);
  CHECK(height.load() == 2160);
  CHECK(OutputAspect(width.load(), height.load(), true) == Approx(16.0 / 9.0));
  CHECK_FALSE(StoreWindowSize(width, height, 3840, 2160));  // unchanged
  CHECK(StoreWindowSize(width, height, 3840, 1600));        // height alone
  CHECK(height.load() == 1600);
  CHECK(StoreWindowSize(width, height, 2560, 1600));        // width alone
  CHECK(width.load() == 2560);
}
