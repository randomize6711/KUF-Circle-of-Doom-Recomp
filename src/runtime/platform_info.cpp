#include "runtime/platform_info.h"

#include <fstream>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(kuf_steam_deck, "auto", "KUF",
                      "auto: the Steam Deck is recognised by its board (Valve Jupiter / Galileo); on / off: as if it "
                      "were or were not one (tests)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace kuf::platform {

namespace {

std::string ReadTrimmed(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) s.pop_back();
  return s;
}

std::string OsReleaseValue(const std::string& text, const std::string& key) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key + "=", 0) != 0) continue;
    std::string v = line.substr(key.size() + 1);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return v;
  }
  return {};
}

}  // namespace

Info Detect(const std::filesystem::path& root, const char* gamescope_display, const char* desktop) {
  Info info;
  std::string os = ReadTrimmed(root / "etc/os-release");
  if (os.empty()) os = ReadTrimmed(root / "usr/lib/os-release");
  info.os_name = OsReleaseValue(os, "PRETTY_NAME");
  info.steamos = OsReleaseValue(os, "ID") == "steamos";
  const std::string vendor = ReadTrimmed(root / "sys/class/dmi/id/board_vendor");
  const std::string board = ReadTrimmed(root / "sys/class/dmi/id/board_name");
  if (vendor == "Valve" && (board == "Jupiter" || board == "Galileo")) {
    info.steam_deck = true;
    info.deck_model = board == "Galileo" ? "OLED" : "LCD";
  }
  info.gamescope = (gamescope_display && *gamescope_display) || (desktop && std::string(desktop) == "gamescope");
  return info;
}

const Info& Current() {
  static const Info info = [] {
    Info i = Detect("/", std::getenv("GAMESCOPE_WAYLAND_DISPLAY"), std::getenv("XDG_CURRENT_DESKTOP"));
    const std::string force = REXCVAR_GET(kuf_steam_deck);
    if (force == "on" && !i.steam_deck) {
      i.steam_deck = true;
      i.deck_model = "forced";
    } else if (force == "off") {
      i.steam_deck = false;
      i.deck_model.clear();
    }
    REXLOG_INFO("[kuf-platform] {}", Summary(i));
    return i;
  }();
  return info;
}

std::string Summary(const Info& i) {
  std::string s = i.os_name.empty() ? "an unknown Linux" : i.os_name;
  if (i.steam_deck) s += ", Steam Deck (" + i.deck_model + ")";
  if (i.gamescope) s += ", under gamescope";
  return s;
}

}  // namespace kuf::platform
