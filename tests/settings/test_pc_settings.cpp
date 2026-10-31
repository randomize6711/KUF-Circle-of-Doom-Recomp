// pc_settings.toml (src/runtime/pc_settings.cpp): defaults, validation, malformed files, unknown keys,
// overrides and the atomic write.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "runtime/pc_settings.h"

using namespace kuf::pc;

namespace {

std::string ReadAll(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    path = std::filesystem::temp_directory_path() / ("kuf-settings-test-" + std::to_string(::getpid()) + "-" +
                                                     std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

}  // namespace

TEST_CASE("an empty file means all defaults", "[settings]") {
  std::vector<std::string> warnings;
  const auto s = Parse("", &warnings);
  REQUIRE(s);
  CHECK(*s == Settings{});
  CHECK(warnings.empty());
  // The defaults themselves.
  CHECK(s->camera.modern);
  CHECK(s->camera.vertical_range == 85);
  CHECK_FALSE(s->camera.auto_recenter);
  CHECK(s->camera.stick_deadzone == 25);
  CHECK(s->camera.distance == 100);       // the game's own distance
  CHECK(s->camera.field_of_view == 0);    // the game's own field of view
}

TEST_CASE("a file from the first M6B build keeps its values; the new settings get their defaults", "[settings]") {
  const char* text =
      "version = 1\n[camera]\nmodern_camera = true\nvertical_range_degrees = 75\nauto_recenter = true\n"
      "[mouse]\nsensitivity_horizontal = 10\nsensitivity_vertical = 3\ninvert_y = true\n"
      "[controller]\ncamera_sensitivity = 10\ncamera_deadzone_percent = 20\n";
  std::vector<std::string> warnings;
  const auto s = Parse(text, &warnings);
  REQUIRE(s);
  CHECK(warnings.empty());
  CHECK(s->camera.mouse_horizontal == 10);  // same level, same speed (0.25 degrees per count)
  CHECK(s->camera.mouse_vertical == 3);
  CHECK(s->camera.stick_sensitivity == 10);
  CHECK(s->camera.stick_deadzone == 20);
  CHECK(s->camera.vertical_range == 75);
  CHECK(s->camera.distance == 100);
  CHECK(s->camera.field_of_view == 0);
  // Written back: the old values unchanged, the new keys added.
  const std::string out = Serialize(*s, text);
  CHECK(out.find("sensitivity_horizontal = 10") != std::string::npos);
  CHECK(out.find("distance_percent = 100") != std::string::npos);
  CHECK(out.find("field_of_view_degrees = 0") != std::string::npos);
}

TEST_CASE("distance and field of view: the allowed values", "[settings]") {
  for (int v : {50, 100, 150, 200}) {
    const auto s = Parse("[camera]\ndistance_percent = " + std::to_string(v) + "\n", nullptr);
    CHECK(s->camera.distance == v);
  }
  for (int v : {40, 210, 105, -100}) {
    std::vector<std::string> warnings;
    const auto s = Parse("[camera]\ndistance_percent = " + std::to_string(v) + "\n", &warnings);
    CHECK(s->camera.distance == 100);
    CHECK(warnings.size() == 1);
  }
  for (int v : {0, 60, 75, 110}) {
    const auto s = Parse("[camera]\nfield_of_view_degrees = " + std::to_string(v) + "\n", nullptr);
    CHECK(s->camera.field_of_view == v);
  }
  for (int v : {55, 115, 72, 30}) {
    std::vector<std::string> warnings;
    const auto s = Parse("[camera]\nfield_of_view_degrees = " + std::to_string(v) + "\n", &warnings);
    CHECK(s->camera.field_of_view == 0);
    CHECK(warnings.size() == 1);
  }
  for (int v : {1, 10, 11, 20}) {
    const auto s = Parse("[mouse]\nsensitivity_horizontal = " + std::to_string(v) + "\n", nullptr);
    CHECK(s->camera.mouse_horizontal == v);
  }
}

TEST_CASE("every key round-trips", "[settings]") {
  Settings in;
  in.camera.modern = false;
  in.camera.vertical_range = 60;
  in.camera.auto_recenter = true;
  in.camera.mouse_horizontal = 7;
  in.camera.mouse_vertical = 2;
  in.camera.invert_mouse_y = true;
  in.camera.stick_sensitivity = 9;
  in.camera.stick_deadzone = 40;
  in.camera.distance = 150;
  in.camera.field_of_view = 90;
  const std::string text = Serialize(in, "");
  std::vector<std::string> warnings;
  const auto out = Parse(text, &warnings);
  REQUIRE(out);
  CHECK(*out == in);
  CHECK(warnings.empty());
  CHECK(text.find("version = 1") != std::string::npos);
}

TEST_CASE("invalid values fall back to their defaults, the rest is kept", "[settings]") {
  const char* text =
      "[camera]\nmodern_camera = \"yes\"\nvertical_range_degrees = 50\nauto_recenter = true\n"
      "distance_percent = 45\nfield_of_view_degrees = 112\n"
      "[mouse]\nsensitivity_horizontal = 21\nsensitivity_vertical = 3\n"
      "[controller]\ncamera_deadzone_percent = 33\ncamera_sensitivity = 2.5\n";
  std::vector<std::string> warnings;
  const auto s = Parse(text, &warnings);
  REQUIRE(s);
  CHECK(s->camera.modern == CameraSettings{}.modern);
  CHECK(s->camera.vertical_range == 85);
  CHECK(s->camera.auto_recenter);
  CHECK(s->camera.mouse_horizontal == CameraSettings{}.mouse_horizontal);
  CHECK(s->camera.mouse_vertical == 3);
  CHECK(s->camera.stick_deadzone == 25);
  CHECK(s->camera.stick_sensitivity == CameraSettings{}.stick_sensitivity);
  CHECK(s->camera.distance == 100);
  CHECK(s->camera.field_of_view == 0);
  CHECK(warnings.size() == 7);
}

TEST_CASE("a malformed file is rejected (the caller keeps it aside and uses the defaults)", "[settings]") {
  std::vector<std::string> warnings;
  CHECK_FALSE(Parse("[camera\nmodern_camera = true", &warnings));
  REQUIRE(warnings.size() == 1);
  CHECK(warnings[0].find("not valid TOML") != std::string::npos);
  CHECK_FALSE(Parse(std::string("\x00\xff\xfe garbage", 12), &warnings));
}

TEST_CASE("unknown keys and tables are kept when the file is rewritten", "[settings]") {
  const char* previous =
      "version = 1\nfuture_option = \"x\"\n[camera]\nmodern_camera = true\nzoom = 3\n"
      "[display]\nresolution = \"2560x1440\"\n";
  Settings s;
  s.camera.modern = false;
  const std::string text = Serialize(s, previous);
  CHECK(text.find("future_option = ") != std::string::npos);
  CHECK(text.find("zoom = 3") != std::string::npos);
  CHECK(text.find("2560x1440") != std::string::npos);
  const auto back = Parse(text, nullptr);
  REQUIRE(back);
  CHECK_FALSE(back->camera.modern);
}

TEST_CASE("a newer version is read for the keys this build knows", "[settings]") {
  std::vector<std::string> warnings;
  const auto s = Parse("version = 7\n[camera]\nvertical_range_degrees = 45\n", &warnings);
  REQUIRE(s);
  CHECK(s->camera.vertical_range == 45);
  CHECK(warnings.size() == 1);
}

TEST_CASE("overrides for one run", "[settings]") {
  Settings s;
  std::string error;
  CHECK(ApplyOverrides("camera.modern_camera=false, controller.camera_deadzone_percent=10", &s, &error));
  CHECK_FALSE(s.camera.modern);
  CHECK(s.camera.stick_deadzone == 10);
  const Settings before = s;
  CHECK_FALSE(ApplyOverrides("camera.nope=1", &s, &error));
  CHECK(error.find("unknown") != std::string::npos);
  CHECK_FALSE(ApplyOverrides("camera.vertical_range_degrees=50", &s, &error));
  CHECK_FALSE(ApplyOverrides("modern_camera=false", &s, &error));
  CHECK(s == before);  // a failed override changes nothing
}

TEST_CASE("atomic write: new file, then replacement with a backup of the previous one", "[settings]") {
  TempDir dir;
  const auto file = dir.path / "pc_settings.toml";
  std::string error;
  REQUIRE(WriteFileAtomically(file, "first\n", &error));
  CHECK(ReadAll(file) == "first\n");
  CHECK_FALSE(std::filesystem::exists(file.string() + ".bak"));
  REQUIRE(WriteFileAtomically(file, "second\n", &error));
  CHECK(ReadAll(file) == "second\n");
  CHECK(ReadAll(file.string() + ".bak") == "first\n");
  CHECK_FALSE(std::filesystem::exists(file.string() + ".tmp"));
  CHECK_FALSE(std::filesystem::exists(file.string() + ".bak.tmp"));
}

TEST_CASE("atomic write: an unwritable directory fails and leaves nothing behind", "[settings]") {
  TempDir dir;
  const auto blocked = dir.path / "not-a-dir";
  { std::ofstream(blocked) << "x"; }
  std::string error;
  CHECK_FALSE(WriteFileAtomically(blocked / "pc_settings.toml", "x", &error));
  CHECK_FALSE(error.empty());
  CHECK(ReadAll(blocked) == "x");
}

// --- Display (M6C) -------------------------------------------------------------------------------------

TEST_CASE("display: defaults (borderless at the desktop's size, primary monitor, widescreen)", "[settings]") {
  std::vector<std::string> warnings;
  const auto s = Parse("", &warnings);
  REQUIRE(s);
  CHECK(s->display.mode == DisplayMode::kBorderless);
  CHECK(s->display.width == 0);
  CHECK(s->display.height == 0);
  CHECK(s->display.refresh_mhz == 0);
  CHECK(s->display.monitor.empty());
  CHECK(s->display.widescreen);
  CHECK(warnings.empty());
}

TEST_CASE("display: an M6B file (no [display] table) keeps its camera values and gets the display defaults",
          "[settings]") {
  const char* m6b =
      "version = 1\n[camera]\nmodern_camera = true\ndistance_percent = 150\nfield_of_view_degrees = 90\n"
      "[mouse]\nsensitivity_horizontal = 20\n";
  std::vector<std::string> warnings;
  const auto s = Parse(m6b, &warnings);
  REQUIRE(s);
  CHECK(warnings.empty());
  CHECK(s->camera.distance == 150);
  CHECK(s->camera.field_of_view == 90);
  CHECK(s->camera.mouse_horizontal == 20);
  CHECK(s->display == DisplaySettings{});
  // Written back: the camera values unchanged, the display table added.
  const auto again = Parse(Serialize(*s, m6b), nullptr);
  REQUIRE(again);
  CHECK(*again == *s);
}

TEST_CASE("display: every value round-trips", "[settings]") {
  Settings s;
  s.display.mode = DisplayMode::kFullscreen;
  s.display.width = 3440;
  s.display.height = 1440;
  s.display.refresh_mhz = 143998;
  s.display.monitor = "DELL U3423WE (DP-2)";
  s.display.widescreen = false;
  const auto parsed = Parse(Serialize(s, ""), nullptr);
  REQUIRE(parsed);
  CHECK(*parsed == s);
  for (const auto mode : {DisplayMode::kWindowed, DisplayMode::kBorderless}) {
    s.display.mode = mode;
    CHECK(Parse(Serialize(s, ""), nullptr)->display.mode == mode);
  }
}

TEST_CASE("display: invalid values fall back to their defaults, one by one", "[settings]") {
  const char* text =
      "[display]\nmode = 'exclusive'\nwidth = 100\nheight = 99999\nrefresh_millihertz = -5\n"
      "monitor = 7\nwidescreen = 'yes'\n";
  std::vector<std::string> warnings;
  const auto s = Parse(text, &warnings);
  REQUIRE(s);
  CHECK(s->display == DisplaySettings{});
  CHECK(warnings.size() == 6);
  // A control character in the monitor name, a name that is too long.
  CHECK(Parse("[display]\nmonitor = \"a\\u0001b\"\n", nullptr)->display.monitor.empty());
  CHECK(Parse("[display]\nmonitor = \"" + std::string(300, 'x') + "\"\n", nullptr)->display.monitor.empty());
}

TEST_CASE("display: a size needs both sides", "[settings]") {
  std::vector<std::string> warnings;
  const auto s = Parse("[display]\nmode = 'windowed'\nwidth = 1920\n", &warnings);
  REQUIRE(s);
  CHECK(s->display.mode == DisplayMode::kWindowed);
  CHECK(s->display.width == 0);
  CHECK(s->display.height == 0);
  CHECK(warnings.size() == 1);
}

TEST_CASE("display: a saved monitor that is not connected is kept in the file (the game falls back at run time)",
          "[settings]") {
  const auto s = Parse("[display]\nmonitor = 'HDMI-A-9'\n", nullptr);
  REQUIRE(s);
  CHECK(s->display.monitor == "HDMI-A-9");
  CHECK(Serialize(*s, "").find("monitor = 'HDMI-A-9'") != std::string::npos);
}

TEST_CASE("display: overrides, text without quotes", "[settings]") {
  Settings s;
  std::string error;
  REQUIRE(ApplyOverrides("display.mode=windowed,display.width=1600,display.height=900,display.widescreen=false", &s,
                         &error));
  CHECK(s.display.mode == DisplayMode::kWindowed);
  CHECK(s.display.width == 1600);
  CHECK(s.display.height == 900);
  CHECK_FALSE(s.display.widescreen);
  CHECK_FALSE(ApplyOverrides("display.mode=exclusive", &s, &error));
  CHECK_FALSE(ApplyOverrides("display.width=12", &s, &error));
  CHECK(s.display.mode == DisplayMode::kWindowed);  // unchanged after a rejected override
}

// --- Render resolution (M6C follow-up) -----------------------------------------------------------------

TEST_CASE("display: render_scale defaults to native and round-trips 0..4", "[settings]") {
  const auto s = Parse("", nullptr);
  REQUIRE(s);
  CHECK(s->display.render_scale == kRenderScaleNative);
  CHECK(Serialize(*s, "").find("render_scale = 0") != std::string::npos);
  for (int scale = kRenderScaleNative; scale <= kRenderScaleMax; ++scale) {
    Settings t;
    t.display.render_scale = scale;
    const auto parsed = Parse(Serialize(t, ""), nullptr);
    REQUIRE(parsed);
    CHECK(parsed->display.render_scale == scale);
  }
}

TEST_CASE("display: an M6C file without render_scale renders natively; invalid values fall back to native",
          "[settings]") {
  std::vector<std::string> warnings;
  const auto m6c = Parse("[display]\nmode = 'windowed'\nwidth = 1920\nheight = 1080\nwidescreen = true\n", &warnings);
  REQUIRE(m6c);
  CHECK(warnings.empty());
  CHECK(m6c->display.render_scale == kRenderScaleNative);
  for (const char* bad : {"render_scale = 5", "render_scale = -1", "render_scale = 'native'", "render_scale = 2.0"}) {
    warnings.clear();
    const auto s = Parse(std::string("[display]\n") + bad + "\n", &warnings);
    REQUIRE(s);
    CHECK(s->display.render_scale == kRenderScaleNative);
    CHECK(warnings.size() == 1);
  }
}

TEST_CASE("display: render_scale override", "[settings]") {
  Settings s;
  std::string error;
  REQUIRE(ApplyOverrides("display.render_scale=3", &s, &error));
  CHECK(s.display.render_scale == 3);
  CHECK_FALSE(ApplyOverrides("display.render_scale=8", &s, &error));
  CHECK(s.display.render_scale == 3);
}

TEST_CASE("frame rate (M6D D5): defaults are the original 30 fps with interpolation on for when it is raised",
          "[settings]") {
  const auto s = Parse("", nullptr);
  REQUIRE(s);
  CHECK(s->frame_rate.fps_limit == 30);
  CHECK(s->frame_rate.interpolation);
  CHECK(s->frame_rate.objects);
  CHECK(s->frame_rate.animation);
  CHECK(s->frame_rate.effects);
  // An M6C file (no [frame_rate] table) stays at the original frame rate.
  const auto m6c = Parse("version = 1\n[display]\nmode = \"windowed\"\nrender_scale = 2\n", nullptr);
  REQUIRE(m6c);
  CHECK(m6c->frame_rate == FrameRateSettings{});
}

TEST_CASE("frame rate: the menu's values, custom limits and unlimited round-trip", "[settings]") {
  for (int v : {30, 60, 90, 120, 144, 165, 240, 0, 31, 100, 480}) {
    Settings in;
    in.frame_rate.fps_limit = v;
    in.frame_rate.interpolation = v % 2 == 0;
    in.frame_rate.objects = false;
    in.frame_rate.animation = v != 60;
    in.frame_rate.effects = false;
    std::vector<std::string> warnings;
    const auto out = Parse(Serialize(in, ""), &warnings);
    REQUIRE(out);
    CHECK(out->frame_rate == in.frame_rate);
    CHECK(warnings.empty());
  }
  CHECK(Serialize(Settings{}, "").find("[frame_rate]") != std::string::npos);
}

TEST_CASE("frame rate: invalid values fall back to the defaults, one by one", "[settings]") {
  for (const char* bad : {"limit_fps = 29", "limit_fps = 1", "limit_fps = 481", "limit_fps = -1", "limit_fps = 120.0",
                          "limit_fps = \"120\"", "interpolation = 1", "interpolate_objects = \"no\"",
                          "interpolate_animation = 0", "interpolate_effects = []"}) {
    std::vector<std::string> warnings;
    const auto s = Parse(std::string("[frame_rate]\n") + bad + "\n[camera]\nvertical_range_degrees = 60\n", &warnings);
    REQUIRE(s);
    INFO(bad);
    CHECK(s->frame_rate == FrameRateSettings{});
    CHECK(s->camera.vertical_range == 60);  // the rest is kept
    CHECK(warnings.size() == 1);
  }
  CHECK(ValidFpsLimit(30));
  CHECK(ValidFpsLimit(0));
  CHECK_FALSE(ValidFpsLimit(30 - 1));
  for (int v : kFpsChoices) CHECK(ValidFpsLimit(v));
}

TEST_CASE("frame rate: overrides for one run", "[settings]") {
  Settings s;
  std::string error;
  CHECK(ApplyOverrides("frame_rate.limit_fps=120,frame_rate.interpolate_effects=false", &s, &error));
  CHECK(s.frame_rate.fps_limit == 120);
  CHECK_FALSE(s.frame_rate.effects);
  CHECK_FALSE(ApplyOverrides("frame_rate.limit_fps=20", &s, &error));
  CHECK(s.frame_rate.fps_limit == 120);
}

TEST_CASE("bindings (M7): defaults store nothing and write no tables", "[settings]") {
  CHECK(Settings{}.bindings == BindingSettings{});
  const std::string text = Serialize(Settings{}, "");
  CHECK(text.find("gameplay_keys") == std::string::npos);
  CHECK(text.find("controller_buttons") == std::string::npos);
}

TEST_CASE("bindings: gameplay keys and controller buttons round-trip", "[settings]") {
  Settings s;
  s.bindings.keys["a"] = "MouseRight,Return";
  s.bindings.keys["lstick_up"] = "Up";
  s.bindings.buttons["a"] = "x";
  s.bindings.buttons["x"] = "a";
  std::vector<std::string> warnings;
  const auto back = Parse(Serialize(s, ""), &warnings);
  REQUIRE(back);
  CHECK(warnings.empty());
  CHECK(back->bindings == s.bindings);
  // Back to the defaults: the tables go, unknown keys in them stay.
  const std::string with_unknown = Serialize(s, "[gameplay_keys]\nfuture_action = \"K\"\n");
  const std::string cleared = Serialize(Settings{}, with_unknown);
  CHECK(cleared.find("future_action") != std::string::npos);
  CHECK(cleared.find("MouseRight") == std::string::npos);
  CHECK(cleared.find("controller_buttons") == std::string::npos);
}

TEST_CASE("bindings: invalid entries are dropped one by one", "[settings]") {
  for (const char* bad : {"[gameplay_keys]\na = \"NoSuchKey\"", "[gameplay_keys]\na = \"\"", "[gameplay_keys]\na = 1",
                          "[gameplay_keys]\nfly = \"F\"", "[controller_buttons]\na = \"dpad_up\"",
                          "[controller_buttons]\nnose = \"a\""}) {
    std::vector<std::string> warnings;
    const auto s = Parse(std::string(bad) + "\n[camera]\nvertical_range_degrees = 60\n", &warnings);
    REQUIRE(s);
    INFO(bad);
    CHECK(s->bindings == BindingSettings{});
    CHECK(s->camera.vertical_range == 60);
    CHECK(warnings.size() == 1);
  }
}

TEST_CASE("bindings: controller buttons must stay one permutation", "[settings]") {
  std::vector<std::string> warnings;
  // X acts as A, but nothing acts as X any more and A acts as nothing: refused as a whole.
  auto s = Parse("[controller_buttons]\na = \"x\"\n", &warnings);
  REQUIRE(s);
  CHECK(s->bindings.buttons.empty());
  CHECK(warnings.size() == 1);
  // A full swap is kept; a button to itself is not stored.
  warnings.clear();
  s = Parse("[controller_buttons]\na = \"y\"\ny = \"a\"\nb = \"b\"\n", &warnings);
  REQUIRE(s);
  CHECK(warnings.empty());
  CHECK(s->bindings.buttons == std::map<std::string, std::string>{{"a", "y"}, {"y", "a"}});
}

TEST_CASE("bindings: overrides for one run", "[settings]") {
  Settings s;
  std::string error;
  CHECK(ApplyOverrides("gameplay_keys.a=J,controller_buttons.a=b,controller_buttons.b=a", &s, &error));
  CHECK(s.bindings.keys.at("a") == "J");
  CHECK(s.bindings.buttons.at("a") == "b");
  CHECK_FALSE(ApplyOverrides("controller_buttons.a=x", &s, &error));  // not a permutation with what is there
  CHECK(s.bindings.buttons.at("a") == "b");
  CHECK_FALSE(ApplyOverrides("gameplay_keys.a=NoSuchKey", &s, &error));
  CHECK(ApplyOverrides("gameplay_keys.b=\"MouseX1\"", &s, &error));
  CHECK(s.bindings.keys.at("b") == "MouseX1");
}

TEST_CASE("vsync and button prompts (M7 UX fixes 3, 4): defaults, round-trip, invalid values", "[settings]") {
  CHECK_FALSE(Settings{}.display.vsync);
  CHECK(Settings{}.ui.button_prompts == kPromptsAuto);
  Settings s;
  s.display.vsync = true;
  s.ui.button_prompts = kPromptsController;
  std::vector<std::string> warnings;
  const auto back = Parse(Serialize(s, ""), &warnings);
  REQUIRE(back);
  CHECK(warnings.empty());
  CHECK(back->display.vsync);
  CHECK(back->ui.button_prompts == kPromptsController);
  for (const char* bad : {"[display]\nvsync = 1", "[interface]\nbutton_prompts = 3", "[interface]\nbutton_prompts = -1",
                          "[interface]\nbutton_prompts = \"auto\""}) {
    warnings.clear();
    const auto p = Parse(bad, &warnings);
    REQUIRE(p);
    INFO(bad);
    CHECK_FALSE(p->display.vsync);
    CHECK(p->ui.button_prompts == kPromptsAuto);
    CHECK(warnings.size() == 1);
  }
  std::string error;
  CHECK(ApplyOverrides("display.vsync=true,interface.button_prompts=1", &s, &error));
  CHECK(s.ui.button_prompts == kPromptsKeyboard);
}
