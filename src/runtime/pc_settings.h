// PC-specific settings (M6B): pc_settings.toml in the user data directory, separate from the game's saves and
// its own options (GlobalSetting). Design: docs/MENU_ARCHITECTURE.md section 7.2, docs/SETTINGS.md.
//
//   - Defaults are used for anything missing; nothing is written until the player changes a value in the
//     game's Settings (PC Settings page).
//   - Every value is type- and range-checked; an invalid value falls back to its default (logged).
//   - A file that cannot be parsed is renamed to pc_settings.toml.bad-<time> and defaults are used.
//   - Unknown keys and tables are kept and written back unchanged.
//   - Writes are atomic (temporary file, fsync, rename); the previous file is kept as pc_settings.toml.bak.
//   - --kuf_pc_settings="key=value,..." overrides values for one run (tests); changing such a value in the
//     menu makes it a normal setting again.
//
// The parsing and validation functions are pure (unit-tested in tests/settings).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kuf::pc {

// The camera and its controls (M6B). Levels are the steps the in-game sliders show.
struct CameraSettings {
  bool modern = true;               // the modern camera; false = the original console camera exactly
  int mouse_horizontal = 4;         // 1..20 (camera_math.h MouseRadiansPerCount: 1..10 as in the first M6B build)
  int mouse_vertical = 4;           // 1..20
  bool invert_mouse_y = false;      // the controller follows the game's own Camera Normal/Inverted option
  int stick_sensitivity = 5;        // 1..20 (camera_math.h StickRadiansPerSecond)
  int stick_deadzone = 25;          // percent of the stick's travel, 0..50 in steps of 5
  int vertical_range = 85;          // degrees up and down from level: 45, 60, 75 or 85
  bool auto_recenter = false;       // ease the view back to the default height after a pause in camera input
  int distance = 100;               // orbit distance, percent of the game's own camera distance
  int field_of_view = 0;            // horizontal degrees at 16:9 (60..110 in steps of 5); 0 = the game's own
  bool operator==(const CameraSettings&) const = default;
};

// The display (M6C).
enum class DisplayMode { kWindowed, kBorderless, kFullscreen };
struct DisplaySettings {
  DisplayMode mode = DisplayMode::kBorderless;  // fullscreen = exclusive (a display mode of its own)
  int width = 0, height = 0;  // windowed: the window's size; fullscreen: the mode's; 0 = the desktop's
  int refresh_mhz = 0;        // fullscreen: the mode's refresh rate (millihertz); 0 = the desktop's
  std::string monitor;        // the monitor's name as the system reports it; empty = the primary monitor
  bool widescreen = true;     // draw for the output's aspect; false = 16:9 as the original (pillarbox/letterbox)
  int render_scale = 0;       // the game's 1280x720 frame rendered at this multiple per axis; 0 = native (the
                              // smallest multiple at least the output's size); takes effect at the next start
  bool vsync = false;         // M7: present at the display's refresh (no tearing); false: as fast as frames come.
                              // Presentation only: the game's 30 ticks per second are unchanged
  bool operator==(const DisplaySettings&) const = default;
};

// The frame rate (M6D D5): frames drawn between the game's 30 ticks and what is interpolated in them
// (docs/HIGH_FPS.md section 21). The game itself always runs 30 ticks per second.
struct FrameRateSettings {
  int fps_limit = 30;          // 30 = the original (one frame per tick); 31..480 = up to that many frames per second;
                               // 0 = unlimited (as many as the hardware draws)
  bool interpolation = true;   // draw the camera and the player between ticks (false: each frame repeats its tick)
  bool objects = true;         // ... and the world's objects (with interpolation)
  bool animation = true;       // ... and the characters' skeletal poses with their physics-driven parts
  bool effects = true;         // ... and the effect shaders' clock
  bool operator==(const FrameRateSettings&) const = default;
};

// Gameplay bindings (M7 feature 5, docs/INPUT.md "Rebinding"): only what differs from the defaults is stored.
struct BindingSettings {
  // [gameplay_keys]: a control's gameplay keys (ControlName -> key names as ParseHostKeys reads them, comma-separated;
  // never empty). --kuf_bind_game_<control> still overrides them for one run.
  std::map<std::string, std::string> keys;
  // [controller_buttons]: the physical button that acts as a button in gameplay (PadButtonName -> PadButtonName);
  // the whole table is one permutation (input/pad_remap.h), or it is dropped.
  std::map<std::string, std::string> buttons;
  bool operator==(const BindingSettings&) const = default;
};

// The interface (M7 UX fix 4).
struct InterfaceSettings {
  int button_prompts = 0;  // 0 auto (the device used last), 1 keyboard and mouse, 2 controller
  bool operator==(const InterfaceSettings&) const = default;
};
constexpr int kPromptsAuto = 0, kPromptsKeyboard = 1, kPromptsController = 2;

struct Settings {
  CameraSettings camera;
  DisplaySettings display;
  FrameRateSettings frame_rate;
  BindingSettings bindings;
  InterfaceSettings ui;  // [interface]
  bool operator==(const Settings&) const = default;
};

// The allowed values (shared by the parser, the menu and the tests).
constexpr int kLevelMin = 1, kLevelMax = 20;
constexpr int kLevelLinearMax = 10;  // levels up to here are evenly spaced; above, each is a fixed factor more
constexpr int kDeadzoneMin = 0, kDeadzoneMax = 50, kDeadzoneStep = 5;
constexpr int kVerticalRanges[] = {45, 60, 75, 85};
constexpr int kDistanceMin = 50, kDistanceMax = 200, kDistanceStep = 10;
constexpr int kFovOriginal = 0, kFovMin = 60, kFovMax = 110, kFovStep = 5;
constexpr int kDisplaySizeMin = 640, kDisplaySizeMax = 16384;  // pixels, each side (0 = the desktop's size)
constexpr int kRefreshMax = 1000000;                           // millihertz
constexpr int kRenderScaleNative = 0, kRenderScaleMax = 4;     // render_scale: native, or 1x..4x
const char* DisplayModeName(DisplayMode mode);                 // as in the file: windowed, borderless, fullscreen
constexpr int kFpsOriginal = 30, kFpsUnlimited = 0, kFpsMin = 31, kFpsMax = 480;  // fps_limit
constexpr int kFpsChoices[] = {30, 60, 90, 120, 144, 165, 240, 0};               // the menu's, in its order
bool ValidFpsLimit(int64_t v);

// Parses the text of pc_settings.toml. Invalid or missing values get their defaults (each problem is added
// to *warnings). Returns nullopt if the text is not valid TOML (*warnings holds the parser's message).
std::optional<Settings> Parse(std::string_view text, std::vector<std::string>* warnings);

// The file text for `settings`. `previous` is the current file's text (may be empty): its unknown keys and
// tables are kept; known keys get the new values.
std::string Serialize(const Settings& settings, std::string_view previous);

// Applies "key=value,key=value" (keys as in the file, e.g. camera.modern_camera=false). False (with *error)
// on an unknown key or an invalid value; `settings` is then unchanged.
bool ApplyOverrides(std::string_view spec, Settings* settings, std::string* error);

// The first start on a Steam Deck (no pc_settings.toml yet; M11, docs/STEAM_DECK.md "Settings"): the game's
// 1280x720 frame rendered once (render scale 1x) instead of "native", which for the Deck's 1280x800 screen renders
// 1280x1440 (twice the pixels for an 800-line screen), and VSync on (even frame pacing on its 60 Hz panel, no
// tearing). Everything else stays the defaults. Never applied over an existing file: the player's choices stand.
void ApplyFirstStartDefaults(Settings* settings, bool steam_deck);

// Writes `text` to `path` atomically, keeping the old file as <path>.bak. False (with *error) on failure; the
// old file is then unchanged.
bool WriteFileAtomically(const std::filesystem::path& path, std::string_view text, std::string* error);

// --- The settings of this run -------------------------------------------------------------------------
// Loads <dir>/pc_settings.toml (or the defaults) and the --kuf_pc_settings overrides. Called once at start.
void Init(const std::filesystem::path& user_data_dir);
// A copy of the current settings (any thread).
Settings Current();
// Changes the settings in memory (the menu); they take effect at once.
void Update(const Settings& settings);
// The frame rate given on the command line for this run (frame_pacing.cpp): shown by the menu, not written to the file
// unless the player changes a value there (as --kuf_pc_settings).
void SetFrameRateForThisRun(const FrameRateSettings& frame_rate);
// Writes the file if Update changed something since the last write (the menu, when the player leaves
// Settings). Returns false only if writing failed.
bool SaveIfChanged();
// Where the file is (empty before Init).
std::filesystem::path FilePath();

}  // namespace kuf::pc
