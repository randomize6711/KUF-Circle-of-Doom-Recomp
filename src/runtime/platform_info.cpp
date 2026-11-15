#include "runtime/platform_info.h"

#include <fstream>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#include "runtime/win32.h"
#endif

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

#if defined(_WIN32)
// "Windows 11 (build 26100)": RtlGetVersion, which reports the real version (GetVersionEx is capped by the manifest).
std::string WindowsName() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof(v);
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  const auto get = ntdll ? reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
  if (!get || get(&v) != 0) return "Windows";
  const char* name = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "Windows 11"
                     : v.dwMajorVersion == 10                          ? "Windows 10"
                                                                        : "Windows";
  return std::string(name) + " (build " + std::to_string(v.dwBuildNumber) + ")";
}
#endif

Info Detect(const std::filesystem::path& root, const char* gamescope_display, const char* desktop) {
  Info info;
#if defined(_WIN32)
  (void)root;
  (void)gamescope_display;
  (void)desktop;
  info.os_name = WindowsName();
#else
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
#endif
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
