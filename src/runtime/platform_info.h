// The machine the game runs on (M11, docs/STEAM_DECK.md): SteamOS, the Steam Deck (its DMI board, never a guess from
// the GPU) and gamescope (Steam's Game Mode compositor). Read once; logged at start for bug reports.
#pragma once

#include <filesystem>
#include <string>

namespace kuf::platform {

struct Info {
  std::string os_name;     // os-release PRETTY_NAME
  bool steamos = false;    // os-release ID=steamos
  bool steam_deck = false; // DMI board Valve "Jupiter" (LCD) or "Galileo" (OLED)
  std::string deck_model;  // "LCD", "OLED" or ""
  bool gamescope = false;  // running under gamescope (Game Mode, or a nested gamescope)
};

// From files under `root` ("/" except in tests) and the environment given (gamescope's variables).
Info Detect(const std::filesystem::path& root, const char* gamescope_display, const char* desktop);
// This process's machine: Detect("/", $GAMESCOPE_WAYLAND_DISPLAY, $XDG_CURRENT_DESKTOP); --kuf_steam_deck=on/off
// overrides the Steam Deck answer (tests). Computed once.
const Info& Current();
std::string Summary(const Info& info);

}  // namespace kuf::platform
