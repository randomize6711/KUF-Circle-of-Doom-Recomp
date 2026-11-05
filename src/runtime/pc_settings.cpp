// pc_settings.toml: parsing, validation, serialization and the atomic write (pure; pc_settings.h). The
// settings of the running game (Init, Current, Update, SaveIfChanged) are in pc_settings_runtime.cpp.

#include "runtime/pc_settings.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

#include <toml++/toml.hpp>

#include "input/keyboard_pad.h"
#include "input/pad_remap.h"

namespace kuf::pc {
namespace {

constexpr int64_t kVersion = 1;

constexpr const char* kHeader =
    "# Kingdom Under Fire: Circle of Doom (PC port): PC settings.\n"
    "# Change these in the game: Settings > PC Settings. This file is rewritten when you do, and comments in\n"
    "# it are not kept. Unknown keys are kept. Delete the file to return to the defaults.\n\n";

// One known key: where it lives, how to read and write it, and its check.
struct Key {
  const char* table;
  const char* name;
  std::function<bool(const toml::node&, Settings&)> read;  // false: wrong type or out of range
  std::function<void(const Settings&, toml::table&)> write;
};

bool ValidLevel(int64_t v) { return v >= kLevelMin && v <= kLevelMax; }
bool ValidDeadzone(int64_t v) { return v >= kDeadzoneMin && v <= kDeadzoneMax && v % kDeadzoneStep == 0; }
bool ValidDistance(int64_t v) { return v >= kDistanceMin && v <= kDistanceMax && v % kDistanceStep == 0; }
bool ValidFov(int64_t v) { return v == kFovOriginal || (v >= kFovMin && v <= kFovMax && v % kFovStep == 0); }
bool ValidRange(int64_t v) { return std::find(std::begin(kVerticalRanges), std::end(kVerticalRanges), v) != std::end(kVerticalRanges); }

bool ValidSize(int64_t v) { return v == 0 || (v >= kDisplaySizeMin && v <= kDisplaySizeMax); }
bool ValidRefresh(int64_t v) { return v >= 0 && v <= kRefreshMax; }
bool ValidRenderScale(int64_t v) { return v >= kRenderScaleNative && v <= kRenderScaleMax; }
bool ValidFps(int64_t v) { return ValidFpsLimit(v); }
bool ValidPrompts(int64_t v) { return v >= kPromptsAuto && v <= kPromptsController; }

template <typename G>
Key BoolKey(const char* table, const char* name, G Settings::*group, bool G::*field) {
  return {table, name,
          [group, field](const toml::node& n, Settings& s) {
            if (auto v = n.value_exact<bool>()) {
              s.*group.*field = *v;
              return true;
            }
            return false;
          },
          [table, name, group, field](const Settings& s, toml::table& t) {
            t[table].as_table()->insert_or_assign(name, s.*group.*field);
          }};
}

template <typename G>
Key IntKey(const char* table, const char* name, G Settings::*group, int G::*field, bool (*valid)(int64_t)) {
  return {table, name,
          [group, field, valid](const toml::node& n, Settings& s) {
            if (auto v = n.value_exact<int64_t>(); v && valid(*v)) {
              s.*group.*field = int(*v);
              return true;
            }
            return false;
          },
          [table, name, group, field](const Settings& s, toml::table& t) {
            t[table].as_table()->insert_or_assign(name, int64_t(s.*group.*field));
          }};
}

// The monitor's name: printable text (as the system reports it), at most 256 bytes.
Key MonitorKey() {
  return {"display", "monitor",
          [](const toml::node& n, Settings& s) {
            const auto v = n.value_exact<std::string>();
            if (!v || v->size() > 256 ||
                std::any_of(v->begin(), v->end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
              return false;
            }
            s.display.monitor = *v;
            return true;
          },
          [](const Settings& s, toml::table& t) { t["display"].as_table()->insert_or_assign("monitor", s.display.monitor); }};
}

Key ModeKey() {
  return {"display", "mode",
          [](const toml::node& n, Settings& s) {
            const auto v = n.value_exact<std::string>();
            for (const auto mode : {DisplayMode::kWindowed, DisplayMode::kBorderless, DisplayMode::kFullscreen}) {
              if (v && *v == DisplayModeName(mode)) {
                s.display.mode = mode;
                return true;
              }
            }
            return false;
          },
          [](const Settings& s, toml::table& t) {
            t["display"].as_table()->insert_or_assign("mode", std::string(DisplayModeName(s.display.mode)));
          }};
}

const std::vector<Key>& Keys() {
  static const std::vector<Key> keys = {
      BoolKey("camera", "modern_camera", &Settings::camera, &CameraSettings::modern),
      IntKey("camera", "vertical_range_degrees", &Settings::camera, &CameraSettings::vertical_range, ValidRange),
      BoolKey("camera", "auto_recenter", &Settings::camera, &CameraSettings::auto_recenter),
      IntKey("camera", "distance_percent", &Settings::camera, &CameraSettings::distance, ValidDistance),
      IntKey("camera", "field_of_view_degrees", &Settings::camera, &CameraSettings::field_of_view, ValidFov),
      IntKey("mouse", "sensitivity_horizontal", &Settings::camera, &CameraSettings::mouse_horizontal, ValidLevel),
      IntKey("mouse", "sensitivity_vertical", &Settings::camera, &CameraSettings::mouse_vertical, ValidLevel),
      BoolKey("mouse", "invert_y", &Settings::camera, &CameraSettings::invert_mouse_y),
      IntKey("controller", "camera_sensitivity", &Settings::camera, &CameraSettings::stick_sensitivity, ValidLevel),
      IntKey("controller", "camera_deadzone_percent", &Settings::camera, &CameraSettings::stick_deadzone,
             ValidDeadzone),
      ModeKey(),
      IntKey("display", "width", &Settings::display, &DisplaySettings::width, ValidSize),
      IntKey("display", "height", &Settings::display, &DisplaySettings::height, ValidSize),
      IntKey("display", "refresh_millihertz", &Settings::display, &DisplaySettings::refresh_mhz, ValidRefresh),
      MonitorKey(),
      BoolKey("display", "widescreen", &Settings::display, &DisplaySettings::widescreen),
      IntKey("display", "render_scale", &Settings::display, &DisplaySettings::render_scale, ValidRenderScale),
      BoolKey("display", "vsync", &Settings::display, &DisplaySettings::vsync),
      IntKey("frame_rate", "limit_fps", &Settings::frame_rate, &FrameRateSettings::fps_limit, ValidFps),
      BoolKey("frame_rate", "interpolation", &Settings::frame_rate, &FrameRateSettings::interpolation),
      BoolKey("frame_rate", "interpolate_objects", &Settings::frame_rate, &FrameRateSettings::objects),
      BoolKey("frame_rate", "interpolate_animation", &Settings::frame_rate, &FrameRateSettings::animation),
      BoolKey("frame_rate", "interpolate_effects", &Settings::frame_rate, &FrameRateSettings::effects),
      IntKey("interface", "button_prompts", &Settings::ui, &InterfaceSettings::button_prompts, ValidPrompts),
  };
  return keys;
}

// The binding tables: names and values checked one by one; controller_buttons also as a whole.
constexpr const char* kKeysTable = "gameplay_keys";
constexpr const char* kButtonsTable = "controller_buttons";

bool ValidControlName(std::string_view name) {
  for (size_t c = 0; c < kuf::input::kControlCount; ++c) {
    if (name == kuf::input::ControlName(static_cast<kuf::input::Control>(c))) return true;
  }
  return false;
}

bool ValidKeyList(std::string_view value) {
  std::vector<kuf::input::HostKey> keys;
  return value.size() <= 256 && kuf::input::ParseHostKeys(value, &keys) && !keys.empty();
}

// A key's value into `bindings`; false (unchanged) if the name or value is not valid.
bool ReadBinding(std::string_view table, std::string_view name, const toml::node& node, BindingSettings* bindings) {
  const auto v = node.value_exact<std::string>();
  if (!v) return false;
  if (table == kKeysTable) {
    if (!ValidControlName(name) || !ValidKeyList(*v)) return false;
    bindings->keys[std::string(name)] = *v;
    return true;
  }
  if (!kuf::input::ParsePadButton(name) || !kuf::input::ParsePadButton(*v)) return false;
  bindings->buttons[std::string(name)] = *v;
  return true;
}

// controller_buttons as a remap: false if the pairs are not one permutation. Pairs of a button with itself are not
// stored.
bool NormalizeButtons(std::map<std::string, std::string>* buttons) {
  std::array<kuf::input::PadButton, kuf::input::kPadButtonCount> sources;
  for (size_t i = 0; i < sources.size(); ++i) sources[i] = static_cast<kuf::input::PadButton>(i);
  for (const auto& [action, physical] : *buttons) {
    sources[static_cast<size_t>(*kuf::input::ParsePadButton(action))] = *kuf::input::ParsePadButton(physical);
  }
  kuf::input::PadRemap remap;
  if (!remap.Set(sources)) return false;
  std::erase_if(*buttons, [](const auto& e) { return e.first == e.second; });
  return true;
}

std::string Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return std::string(s);
}

bool SyncFd(int fd) { return fd >= 0 && ::fsync(fd) == 0; }

bool WriteAll(const std::filesystem::path& path, std::string_view text, std::string* error) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    *error = "cannot create " + path.string() + ": " + std::strerror(errno);
    return false;
  }
  size_t done = 0;
  while (done < text.size()) {
    const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      *error = "cannot write " + path.string() + ": " + std::strerror(errno);
      ::close(fd);
      return false;
    }
    done += size_t(n);
  }
  const bool synced = SyncFd(fd);
  ::close(fd);
  if (!synced) {
    *error = "cannot flush " + path.string() + ": " + std::strerror(errno);
  }
  return synced;
}

}  // namespace

bool ValidFpsLimit(int64_t v) { return v == kFpsOriginal || v == kFpsUnlimited || (v >= kFpsMin && v <= kFpsMax); }

const char* DisplayModeName(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::kWindowed: return "windowed";
    case DisplayMode::kFullscreen: return "fullscreen";
    default: return "borderless";
  }
}

std::optional<Settings> Parse(std::string_view text, std::vector<std::string>* warnings) {
  toml::table table;
  try {
    table = toml::parse(text);
  } catch (const toml::parse_error& e) {
    if (warnings) {
      std::ostringstream msg;
      msg << "not valid TOML: " << e.description() << " (line " << e.source().begin.line << ")";
      warnings->push_back(msg.str());
    }
    return std::nullopt;
  }
  Settings settings;
  if (const auto* version = table.get("version")) {
    const auto v = version->value_exact<int64_t>();
    if (!v || *v < 1) {
      if (warnings) warnings->push_back("version: not a positive integer (ignored)");
    } else if (*v > kVersion && warnings) {
      warnings->push_back("version " + std::to_string(*v) + " is newer than this build (" +
                          std::to_string(kVersion) + "): known keys are read, the others kept");
    }
  }
  for (const Key& key : Keys()) {
    const auto* section = table.get_as<toml::table>(key.table);
    const toml::node* node = section ? section->get(key.name) : nullptr;
    if (!node) {
      continue;  // missing: default
    }
    if (!key.read(*node, settings) && warnings) {
      warnings->push_back(std::string(key.table) + "." + key.name + ": invalid value (default used)");
    }
  }
  for (const char* name : {kKeysTable, kButtonsTable}) {
    const auto* section = table.get_as<toml::table>(name);
    if (!section) continue;
    for (const auto& [key, node] : *section) {
      if (!ReadBinding(name, key.str(), node, &settings.bindings) && warnings) {
        warnings->push_back(std::string(name) + "." + std::string(key.str()) + ": invalid (default used)");
      }
    }
  }
  if (!NormalizeButtons(&settings.bindings.buttons)) {
    if (warnings) warnings->push_back("controller_buttons: two buttons act as one (all buttons as they are)");
    settings.bindings.buttons.clear();
  }
  // A size needs both sides.
  if ((settings.display.width == 0) != (settings.display.height == 0)) {
    if (warnings) warnings->push_back("display.width / display.height: only one is set (the desktop's size used)");
    settings.display.width = settings.display.height = 0;
  }
  return settings;
}

std::string Serialize(const Settings& settings, std::string_view previous) {
  toml::table table;
  if (!previous.empty()) {
    try {
      table = toml::parse(previous);
    } catch (const toml::parse_error&) {
      table = {};  // a damaged file is replaced (Init keeps a copy of it)
    }
  }
  table.insert_or_assign("version", kVersion);
  for (const Key& key : Keys()) {
    if (!table.get_as<toml::table>(key.table)) {
      table.insert_or_assign(key.table, toml::table{});
    }
    key.write(settings, table);
  }
  // The binding tables: the known names replaced by the current bindings; the table is left out when empty.
  auto write_bindings = [&](const char* name, const std::map<std::string, std::string>& values, auto known) {
    toml::table section;
    if (const auto* old = table.get_as<toml::table>(name)) {
      for (const auto& [key, node] : *old) {
        if (!known(key.str())) section.insert_or_assign(key.str(), node);
      }
    }
    for (const auto& [key, value] : values) section.insert_or_assign(key, value);
    if (section.empty()) {
      table.erase(name);
    } else {
      table.insert_or_assign(name, std::move(section));
    }
  };
  write_bindings(kKeysTable, settings.bindings.keys, [](std::string_view n) { return ValidControlName(n); });
  write_bindings(kButtonsTable, settings.bindings.buttons,
                 [](std::string_view n) { return kuf::input::ParsePadButton(n).has_value(); });
  std::ostringstream out;
  out << kHeader << table << "\n";
  return out.str();
}

void ApplyFirstStartDefaults(Settings* settings, bool steam_deck) {
  if (!steam_deck) {
    return;
  }
  settings->display.render_scale = 1;
  settings->display.vsync = true;
}

bool ApplyOverrides(std::string_view spec, Settings* settings, std::string* error) {
  Settings result = *settings;
  std::string_view rest = spec;
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    const std::string item = Trim(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    if (item.empty()) {
      continue;
    }
    const size_t eq = item.find('=');
    const size_t dot = item.find('.');
    if (eq == std::string::npos || dot == std::string::npos || dot > eq) {
      *error = "'" + item + "': expected table.key=value";
      return false;
    }
    const std::string table = Trim(std::string_view(item).substr(0, dot));
    const std::string name = Trim(std::string_view(item).substr(dot + 1, eq - dot - 1));
    const std::string value = Trim(std::string_view(item).substr(eq + 1));
    if (table == kKeysTable || table == kButtonsTable) {  // a binding: the value as text (one key: commas separate items)
      const bool quoted = value.size() >= 2 && value.front() == '"' && value.back() == '"';
      const toml::value<std::string> text(quoted ? value.substr(1, value.size() - 2) : value);
      if (!ReadBinding(table, name, text, &result.bindings)) {
        *error = "'" + item + "': invalid binding";
        return false;
      }
      continue;
    }
    const auto key = std::find_if(Keys().begin(), Keys().end(),
                                  [&](const Key& k) { return table == k.table && name == k.name; });
    if (key == Keys().end()) {
      *error = "'" + table + "." + name + "': unknown setting";
      return false;
    }
    // Parse the value as TOML so it has the same types as in the file.
    // Text values may be given without quotes (display.mode=windowed).
    toml::table parsed;
    bool ok = true;
    try {
      parsed = toml::parse("v = " + value);
    } catch (const toml::parse_error&) {
      ok = false;
    }
    if (!ok && value.find_first_of("\"\\") == std::string::npos) {
      try {
        parsed = toml::parse("v = \"" + value + "\"");
        ok = true;
      } catch (const toml::parse_error&) {
      }
    }
    if (!ok) {
      *error = "'" + item + "': invalid value";
      return false;
    }
    if (!key->read(*parsed.get("v"), result)) {
      *error = "'" + item + "': invalid value";
      return false;
    }
  }
  if (!NormalizeButtons(&result.bindings.buttons)) {
    *error = "controller_buttons: two buttons act as one";
    return false;
  }
  *settings = result;
  return true;
}

bool WriteFileAtomically(const std::filesystem::path& path, std::string_view text, std::string* error) {
  std::error_code ec;
  const std::filesystem::path dir = path.parent_path().empty() ? "." : path.parent_path();
  std::filesystem::create_directories(dir, ec);
  // Keep the previous version: copy it to <path>.bak through its own temporary file and rename.
  if (std::filesystem::exists(path, ec)) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream old;
    old << in.rdbuf();
    const auto bak = std::filesystem::path(path.string() + ".bak");
    const auto bak_tmp = std::filesystem::path(path.string() + ".bak.tmp");
    if (!in || !WriteAll(bak_tmp, old.str(), error)) {
      std::filesystem::remove(bak_tmp, ec);
      if (error->empty()) *error = "cannot read " + path.string();
      return false;
    }
    std::filesystem::rename(bak_tmp, bak, ec);
    if (ec) {
      *error = "cannot keep " + bak.string() + ": " + ec.message();
      std::filesystem::remove(bak_tmp, ec);
      return false;
    }
  }
  const auto tmp = std::filesystem::path(path.string() + ".tmp");
  if (!WriteAll(tmp, text, error)) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    *error = "cannot replace " + path.string() + ": " + ec.message();
    std::filesystem::remove(tmp, ec);
    return false;
  }
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  SyncFd(dfd);  // the rename is durable once the directory is flushed (best effort)
  if (dfd >= 0) ::close(dfd);
  return true;
}

}  // namespace kuf::pc
