// The PC settings of the running game (pc_settings.h): loaded once at start, changed by the PC Settings page
// of the game's Settings, written when the player leaves Settings after a change.

#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "runtime/pc_settings.h"
#include "runtime/platform_info.h"

REXCVAR_DEFINE_STRING(kuf_pc_settings, "", "KUF/Settings",
                      "Override PC settings for this run: table.key=value,... (e.g. camera.modern_camera=false); "
                      "the file is not changed unless the value is changed in the game's PC Settings");

namespace kuf::pc {
namespace {

std::mutex g_mutex;
Settings g_current;
std::string g_file_text;  // as last read or written (its unknown keys are kept)
std::filesystem::path g_path;
bool g_changed = false;
// The command line set the frame rate for this run (SetFrameRateForThisRun): the file keeps its own frame-rate values
// until the player changes one in the menu.
bool g_frame_rate_run_only = false;
FrameRateSettings g_frame_rate_saved;

std::string ReadFile(const std::filesystem::path& path, bool* ok) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  *ok = bool(in) || in.eof();
  return text.str();
}

}  // namespace

void Init(const std::filesystem::path& user_data_dir) {
  std::lock_guard lock(g_mutex);
  const bool steam_deck = kuf::platform::Current().steam_deck;  // logs the platform (bug reports)
  g_path = user_data_dir / "pc_settings.toml";
  g_current = {};
  g_file_text.clear();
  std::error_code ec;
  if (std::filesystem::exists(g_path, ec)) {
    bool ok = false;
    const std::string text = ReadFile(g_path, &ok);
    std::vector<std::string> warnings;
    const auto parsed = ok ? Parse(text, &warnings) : std::nullopt;
    for (const auto& w : warnings) {
      REXLOG_WARN("[kuf-settings] {}: {}", g_path.string(), w);
    }
    if (parsed) {
      g_current = *parsed;
      g_file_text = text;
      REXLOG_INFO("[kuf-settings] loaded {}", g_path.string());
    } else {
      // Keep the damaged file for the player to inspect, and start from the defaults.
      const auto stamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      const auto bad = std::filesystem::path(g_path.string() + ".bad-" + std::to_string(int64_t(stamp)));
      std::filesystem::rename(g_path, bad, ec);
      REXLOG_WARN("[kuf-settings] {} could not be read; {}; using the defaults", g_path.string(),
                  ec ? "it was left in place: " + ec.message() : "kept as " + bad.filename().string());
    }
  } else {
    ApplyFirstStartDefaults(&g_current, steam_deck);
    REXLOG_INFO("[kuf-settings] no {} yet: defaults{}", g_path.string(),
                steam_deck ? " for the Steam Deck (render scale 1x, VSync on)" : "");
  }
  const std::string overrides = REXCVAR_GET(kuf_pc_settings);
  if (!overrides.empty()) {
    std::string error;
    if (ApplyOverrides(overrides, &g_current, &error)) {
      REXLOG_INFO("[kuf-settings] overrides for this run: {}", overrides);
    } else {
      REXLOG_ERROR("[kuf-settings] --kuf_pc_settings ignored: {}", error);
    }
  }
  const auto& c = g_current.camera;
  REXLOG_INFO("[kuf-settings] camera: modern {} range {} recenter {} mouse {}/{} invert {} stick {} deadzone {}% "
              "distance {}% field of view {}",
              c.modern, c.vertical_range, c.auto_recenter, c.mouse_horizontal, c.mouse_vertical, c.invert_mouse_y,
              c.stick_sensitivity, c.stick_deadzone, c.distance,
              c.field_of_view == kFovOriginal ? std::string("original") : std::to_string(c.field_of_view));
  const auto& d = g_current.display;
  REXLOG_INFO("[kuf-settings] display: {} {} refresh {} monitor {} widescreen {} render {}", DisplayModeName(d.mode),
              d.width ? std::to_string(d.width) + "x" + std::to_string(d.height) : std::string("desktop size"),
              d.refresh_mhz ? std::to_string(d.refresh_mhz) + " mHz" : std::string("desktop"),
              d.monitor.empty() ? std::string("primary") : "'" + d.monitor + "'", d.widescreen,
              d.render_scale == kRenderScaleNative ? std::string("native") : std::to_string(d.render_scale) + "x");
  const auto& f = g_current.frame_rate;
  REXLOG_INFO("[kuf-settings] frame rate: {} interpolation {} objects {} animation {} effects {}",
              f.fps_limit == kFpsOriginal    ? std::string("30 (original)")
              : f.fps_limit == kFpsUnlimited ? std::string("unlimited")
                                             : std::to_string(f.fps_limit),
              f.interpolation, f.objects, f.animation, f.effects);
}

Settings Current() {
  std::lock_guard lock(g_mutex);
  return g_current;
}

void Update(const Settings& settings) {
  std::lock_guard lock(g_mutex);
  if (settings == g_current) {
    return;
  }
  if (!(settings.frame_rate == g_current.frame_rate)) g_frame_rate_run_only = false;
  g_current = settings;
  g_changed = true;
}

void SetFrameRateForThisRun(const FrameRateSettings& frame_rate) {
  std::lock_guard lock(g_mutex);
  if (!g_frame_rate_run_only) g_frame_rate_saved = g_current.frame_rate;
  g_frame_rate_run_only = true;
  g_current.frame_rate = frame_rate;
}

bool SaveIfChanged() {
  std::lock_guard lock(g_mutex);
  if (!g_changed || g_path.empty()) {
    return true;
  }
  Settings out = g_current;
  if (g_frame_rate_run_only) out.frame_rate = g_frame_rate_saved;
  const std::string text = Serialize(out, g_file_text);
  std::string error;
  if (!WriteFileAtomically(g_path, text, &error)) {
    REXLOG_ERROR("[kuf-settings] not saved: {}", error);
    return false;
  }
  g_file_text = text;
  g_changed = false;
  REXLOG_INFO("[kuf-settings] saved {}", g_path.string());
  return true;
}

std::filesystem::path FilePath() {
  std::lock_guard lock(g_mutex);
  return g_path;
}

}  // namespace kuf::pc
