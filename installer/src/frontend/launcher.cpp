// The portable edition's launcher (M16C/F, docs/PORTABLE.md): "Play KUF" on Linux and "Play KUF.exe" on Windows, one
// program for both (SDL3 and Dear ImGui), the logic in core/portable. It finds the game files in GameFiles/ (or a
// folder, disc image or default.xex dropped on the window or on the launcher), checks them, prepares the native game
// once (shows the progress, Cancel is safe) and starts it with everything kept inside the folder. Nothing is changed
// just because something was dropped: preparing and playing wait for the player's button.
//
//   Play KUF [--play] [PATH]     --play: start the game right away when it is prepared; PATH: game files to use
//   Play KUF --self-test DIR     no window: writes what it finds and decides to DIR/self-test.txt (for tests)
//   Play KUF --prepare           no window: prepares the first usable source (progress on stdout), exit 0 when done
//   Setup.exe --install-to DIR [--no-shortcuts] GAME   no window: installs GAME into DIR (the setup's tests)
//   Play KUF --print-launch      no window: the game's command line and environment, one item per line (for tests)
// Tests of the window: KUF_LAUNCHER_SHOT=FILE.bmp draws the first screen into FILE (SDL's offscreen driver works),
// KUF_LAUNCHER_SIZE=WxH sets the window size (1280x800: a Steam Deck).
// Unattended tests (docs/WINDOWS_TEST.md, tools/windows/acceptance): KUF_NO_DIALOG=1 prints what a message box would
// say (uninstall, rollback) instead of waiting for a click; KUF_SETUP_DATA_DIR=DIR keeps an installed copy's data in
// DIR instead of the user's local application data, so a test installation never touches a real one's saves.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // GetModuleFileNameW
#endif

#if !defined(_WIN32)
#include <signal.h>
#endif

#include "core/game_data.h"
#include "core/update/updater.h"
#include "core/platform.h"
#include "core/portable.h"
#include "core/xex_decrypt.h"
#include "core/util.h"

namespace setup = kuf::setup;
namespace fs = std::filesystem;

namespace {

enum class Screen { kNoGameFiles, kChoose, kDetected, kPreparing, kVerifying, kSettings, kMessage, kReadOnly,
                    kSetupDrop, kSetupConfirm, kSetupInstalling, kSetupDone };

#if defined(_WIN32)
constexpr const char* kLauncherName = "Play KUF.exe";
#else
constexpr const char* kLauncherName = "Play KUF";
#endif
constexpr const char* kGameName = "Kingdom Under Fire - Circle of Doom";

struct App {
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  setup::PortableLayout layout;
  setup::GameManifest manifest;
  std::vector<setup::PortableSource> sources;
  int chosen = 0;
  fs::path picked;  // game files picked or dropped outside GameFiles (this run only)
  setup::PortableState state;
  bool ready = false;
  std::string not_ready;
  Screen screen = Screen::kNoGameFiles;
  std::string message_title, message_text;
  Screen after_message = Screen::kDetected;
  bool play_when_ready = false;
  bool quit = false;
  // Work in the background (preparation, verification).
  std::thread worker;
  std::atomic<bool> busy{false}, cancel{false};
  std::mutex mutex;
  setup::PortablePhase phase = setup::PortablePhase::kCopy;
  double fraction = 0;
  std::string detail, result_error;
  bool result_ok = false;
  std::vector<std::string> verify_bad;
  std::vector<std::string> pending_drops;  // from the dialog callbacks and drop events
  int picking = 0;                         // what the open dialog chooses: 0 game files, 1 destination
  // The setup (M16B, Windows): this program with the game package appended (or --setup with Runtime/ beside it).
  bool setup_mode = false;
  fs::path self;                           // this program's file
  uint64_t payload_offset = 0, payload_size = 0, manifest_offset = 0, manifest_size = 0;
  std::string destination;
  bool start_menu = true, desktop = false;
  std::string install_step;
};

// The setup's payload: a .tar.gz of Runtime/ appended to the program, then the reference list (game_v1.tsv), then 40
// bytes: "KUFSETUP" and the payload's and the list's offset and size (little endian). make_windows_setup.sh writes it.
bool ReadPayloadTrailer(App* a) {
  setup::InFile f(a->self);
  std::error_code ec;
  const uint64_t n = fs::file_size(a->self, ec);
  unsigned char t[40];
  if (!f.ok() || ec || n < 40 || !f.ReadAt(n - 40, t, 40) || std::string_view(reinterpret_cast<char*>(t), 8) != "KUFSETUP") {
    return false;
  }
  uint64_t v[4] = {};
  for (int k = 0; k < 4; ++k) {
    for (int i = 7; i >= 0; --i) v[k] = v[k] << 8 | t[8 + k * 8 + i];
  }
  a->payload_offset = v[0];
  a->payload_size = v[1];
  a->manifest_offset = v[2];
  a->manifest_size = v[3];
  return v[0] + v[1] == v[2] && v[2] + v[3] + 40 == n && v[3] < (1u << 20);
}

bool ReadEmbeddedManifest(App* a, std::string* text) {
  if (!a->manifest_size) return false;
  setup::InFile f(a->self);
  text->assign(a->manifest_size, '\0');
  return f.ok() && f.ReadAt(a->manifest_offset, text->data(), text->size());
}

// Where an installed copy (not portable) keeps its data: the user's local application data (KUF_SETUP_DATA_DIR for
// tests: an absolute folder).
fs::path InstalledDataDir() {
  if (const char* d = SDL_getenv("KUF_SETUP_DATA_DIR"); d && *d && setup::PathFromUtf8(d).is_absolute()) {
    return setup::PathFromUtf8(d);
  }
#if defined(_WIN32)
  return setup::GetKnownFolder(setup::KnownFolder::kLocalAppData) / "kuf-cod-recomp" / "data";
#else
  return setup::XdgDataHome() / "kuf-cod-recomp" / "installed-data";
#endif
}

// A message box, or with KUF_NO_DIALOG set (unattended tests) the same text on stdout.
void Notify(bool ok, const std::string& text) {
  const char* quiet = SDL_getenv("KUF_NO_DIALOG");
  if (quiet && *quiet) {
    std::printf("%s: %s\n", ok ? "done" : "error", text.c_str());
    std::fflush(stdout);
    return;
  }
  SDL_ShowSimpleMessageBox(ok ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR, "Kingdom Under Fire: Circle of Doom",
                           text.c_str(), nullptr);
}

#if defined(_WIN32)
// The entry in Settings > Apps (and Control Panel's Programs and Features): per user (HKCU), no administrator.
constexpr const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\KUF-Circle-of-Doom-Recomp";

bool RegisterUninstallEntry(const fs::path& dest, const fs::path& exe) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return false;
  }
  std::string version;
  setup::ReadFile(dest / "Runtime" / "VERSION", &version);
  while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) version.pop_back();
  const auto text = [&](const wchar_t* name, const std::wstring& v) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                          DWORD((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  };
  const auto number = [&](const wchar_t* name, DWORD v) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)) == ERROR_SUCCESS;
  };
  uint64_t bytes = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dest, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) bytes += it->file_size(ec);
  }
  const std::wstring quoted = L"\"" + exe.wstring() + L"\"";
  bool ok = text(L"DisplayName", L"Kingdom Under Fire: Circle of Doom (native port)") &&
            text(L"DisplayVersion", setup::PathFromUtf8(version).wstring()) &&
            text(L"Publisher", L"kuf-cod-recomp") && text(L"InstallLocation", dest.wstring()) &&
            text(L"DisplayIcon", exe.wstring() + L",0") && text(L"UninstallString", quoted + L" --uninstall") &&
            number(L"NoModify", 1) && number(L"NoRepair", 1) && number(L"EstimatedSize", DWORD(bytes >> 10));
  RegCloseKey(key);
  return ok;
}

void RemoveUninstallEntry() { RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey); }
#endif

fs::path DefaultDestination() {
#if defined(_WIN32)
  const fs::path programs = setup::GetKnownFolder(setup::KnownFolder::kPrograms);
  return (programs.empty() ? setup::HomeDir() : programs) / kGameName;
#else
  return setup::HomeDir() / "Games" / kGameName;
#endif
}

constexpr ImVec4 kGold{0.86f, 0.70f, 0.38f, 1}, kText{0.93f, 0.89f, 0.82f, 1}, kMuted{0.70f, 0.64f, 0.56f, 1},
    kBad{0.95f, 0.45f, 0.38f, 1}, kGood{0.55f, 0.85f, 0.50f, 1};

fs::path Utf8Path(const char* s) { return setup::PathFromUtf8(s ? s : ""); }

void Theme(float scale) {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark(&st);
  st.WindowRounding = 0;
  st.FrameRounding = 6;
  st.FramePadding = ImVec2(14, 10);
  st.ItemSpacing = ImVec2(12, 12);
  st.WindowPadding = ImVec2(36, 28);
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.06f, 0.05f, 1);
  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_Button] = ImVec4(0.30f, 0.17f, 0.10f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.26f, 0.13f, 1);
  c[ImGuiCol_ButtonActive] = ImVec4(0.58f, 0.34f, 0.15f, 1);
  c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.12f, 0.09f, 1);
  c[ImGuiCol_PlotHistogram] = kGold;
  c[ImGuiCol_Header] = ImVec4(0.35f, 0.20f, 0.11f, 1);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.26f, 0.13f, 1);
  c[ImGuiCol_Separator] = ImVec4(0.55f, 0.42f, 0.22f, 1);
  st.ScaleAllSizes(scale);
}

// A readable font of the system (Segoe UI on Windows; common sans fonts on Linux, SteamOS included), else ImGui's own.
void LoadFont(float size) {
  ImGuiIO& io = ImGui::GetIO();
  const std::vector<const char*> candidates = {
#if defined(_WIN32)
      "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
#else
      "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/cantarell/Cantarell-Regular.otf",
#endif
  };
  for (const char* f : candidates) {
    std::error_code ec;
    if (fs::exists(f, ec) && io.Fonts->AddFontFromFileTTF(f, size)) return;
  }
  ImFontConfig cfg;
  cfg.SizePixels = size;
  io.Fonts->AddFontDefault(&cfg);
}

bool BigButton(const char* label, float width = 0) {
  return ImGui::Button(label, ImVec2(width, ImGui::GetFontSize() * 2.4f));
}

void Rescan(App* a) {
  a->sources = setup::FindPortableSources(a->layout.GameFiles(), a->manifest);
  if (!a->picked.empty()) {
    // Game files picked or dropped from elsewhere come first.
    setup::PortableSource s;
    s.path = setup::GameSource::Open(a->picked).kind() == setup::GameSource::Kind::kIso
                 ? a->picked
                 : setup::FindGameRoot(a->picked);
    s.check = setup::CheckGame(setup::GameSource::Open(s.path), a->manifest);
    s.label = setup::PathUtf8(s.path);
    a->sources.insert(a->sources.begin(), s);
  }
  setup::PortableState st;
  a->state = setup::ReadPortableState(a->layout.StateFile(), &st) ? st : setup::PortableState{};
  a->chosen = 0;
  // The source prepared last, when it is still there, is the one shown.
  for (size_t i = 0; i < a->sources.size(); ++i) {
    std::string why;
    if (setup::PortableReady(a->layout, a->sources[i], a->state, &why)) a->chosen = int(i);
  }
  const int usable = int(std::count_if(a->sources.begin(), a->sources.end(),
                                       [](const setup::PortableSource& s) { return s.check.Ok(); }));
  if (a->setup_mode) {  // the setup's screens are chosen by the setup
    a->chosen = 0;
    return;
  }
  if (a->sources.empty()) {
    a->screen = Screen::kNoGameFiles;
  } else if (usable > 1 && a->picked.empty()) {
    a->screen = Screen::kChoose;
  } else {
    a->screen = Screen::kDetected;
  }
  if (!a->sources.empty()) {
    a->ready = setup::PortableReady(a->layout, a->sources[a->chosen], a->state, &a->not_ready);
  }
}

void ShowMessage(App* a, std::string title, std::string text, Screen after) {
  a->message_title = std::move(title);
  a->message_text = std::move(text);
  a->after_message = after;
  a->screen = Screen::kMessage;
}

void Launch(App* a) {
  setup::PortableState st;
  if (!setup::ReadPortableState(a->layout.StateFile(), &st)) return;
  const setup::LaunchCommand c = setup::PortableLaunchCommand(a->layout, st);
  std::error_code ec;
  fs::create_directories(a->layout.Logs(), ec);
  if (!setup::StartDetached(c.argv, a->layout.root, a->layout.Logs() / "launcher.log", c.env)) {
    ShowMessage(a, "The game could not be started", "The details are in " +
                    setup::PathUtf8(a->layout.Logs() / "launcher.log") + ".", Screen::kDetected);
    return;
  }
  a->quit = true;
}

void StartPrepare(App* a, bool then_play) {
  if (a->busy) return;
  if (a->worker.joinable()) a->worker.join();
  a->busy = true;
  a->cancel = false;
  a->result_ok = false;
  a->result_error.clear();
  a->play_when_ready = then_play;
  a->screen = Screen::kPreparing;
  const setup::PortableSource source = a->sources[a->chosen];
  a->worker = std::thread([a, source] {
    std::string error;
    const bool ok = setup::PreparePortable(
        a->layout, source, a->manifest,
        [a](setup::PortablePhase p, double f, const std::string& d) {
          std::lock_guard<std::mutex> lock(a->mutex);
          a->phase = p;
          a->fraction = f;
          a->detail = d;
        },
        a->cancel, &error);
    std::lock_guard<std::mutex> lock(a->mutex);
    a->result_ok = ok;
    a->result_error = error;
    a->busy = false;
  });
}

void StartVerify(App* a) {
  if (a->busy) return;
  if (a->worker.joinable()) a->worker.join();
  a->busy = true;
  a->cancel = false;
  a->verify_bad.clear();
  a->screen = Screen::kVerifying;
  a->worker = std::thread([a] {
    std::vector<std::string> bad;
    const bool done = setup::VerifyPortableGameFiles(
        a->layout, a->state, a->manifest, &bad,
        [a](double f) {
          std::lock_guard<std::mutex> lock(a->mutex);
          a->fraction = f;
        },
        a->cancel);
    std::lock_guard<std::mutex> lock(a->mutex);
    a->verify_bad = bad;
    a->result_ok = done;
    a->busy = false;
  });
}

const char* PhaseName(setup::PortablePhase p);

// Runs a program to its end (the setup's tar), for the install worker.
int RunAndWait(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log) {
  setup::ChildProcess c;
  if (!setup::StartProcess(argv, cwd, log, {}, &c)) return -1;
  int code = 0;
  bool oom = false;
  while (!setup::PollProcess(&c, &code, &oom)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return code;
}

fs::path TarProgram() {
#if defined(_WIN32)
  // Windows 10 (1803 and newer) and 11 carry bsdtar.
  const char* root = SDL_getenv("SystemRoot");
  return fs::path(root ? root : "C:\\Windows") / "System32" / "tar.exe";
#else
  return "/usr/bin/tar";
#endif
}

// The installation (setup mode): the program and its runtime into the destination, the player's game files copied
// and checked into its GameFiles/Game, the native game prepared, shortcuts. An installed copy is a portable folder
// whose data lives in the user's local application data (installed.txt says so).
bool InstallTo(App* a, const fs::path& dest, std::string* error) {
  std::error_code ec;
  const auto step = [&](const std::string& what, double f) {
    std::lock_guard<std::mutex> lock(a->mutex);
    a->install_step = what;
    a->fraction = f;
    a->detail.clear();
  };
  fs::create_directories(dest, ec);
  if (!setup::CanCreateIn(dest)) {
    *error = "The folder " + setup::PathUtf8(dest) + " cannot be written. Choose another one.";
    return false;
  }
  // 1. The program.
  step("Installing the program", 0);
  fs::remove_all(dest / "Runtime.part", ec);
  if (a->payload_size) {
    const fs::path tgz = dest / "setup-payload.tar.gz";
    if (setup::CopyRangeHashed(a->self, a->payload_offset, a->payload_size, tgz, nullptr,
                               [a] { return a->cancel.load(); }).empty()) {
      *error = "Could not unpack the setup into " + setup::PathUtf8(dest) + ".";
      return false;
    }
    fs::create_directories(dest / "Runtime.part", ec);
    const int rc = RunAndWait({setup::PathUtf8(TarProgram()), "-xzf", setup::PathUtf8(tgz), "-C",
                               setup::PathUtf8(dest / "Runtime.part")}, dest, dest / "setup.log");
    fs::remove(tgz, ec);
    if (rc != 0) {
      *error = "Unpacking the program failed (" + setup::PathUtf8(dest / "setup.log") + ").";
      return false;
    }
    fs::rename(dest / "Runtime.part" / "Runtime", dest / "Runtime.new", ec);
    fs::remove_all(dest / "Runtime.part", ec);
  } else {
    fs::copy(a->layout.Runtime(), dest / "Runtime.new", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  }
  if (ec || !fs::exists(dest / "Runtime.new" / "bin")) {
    *error = "Could not install the program into " + setup::PathUtf8(dest) + ".";
    return false;
  }
  // A repair keeps the program already prepared (Runtime/bin/kuf_cod*) when the new package is the same generation.
  fs::remove_all(dest / "Runtime.old", ec);
  if (fs::exists(dest / "Runtime", ec)) fs::rename(dest / "Runtime", dest / "Runtime.old", ec);
  fs::rename(dest / "Runtime.new", dest / "Runtime", ec);
  fs::remove_all(dest / "Runtime.old", ec);
  const uint64_t self_size = a->payload_size ? a->payload_offset : fs::file_size(a->self, ec);
  if (setup::CopyRangeHashed(a->self, 0, self_size, dest / kLauncherName, nullptr, nullptr).empty()) {
    *error = "Could not install the launcher.";
    return false;
  }
  fs::permissions(dest / kLauncherName, fs::perms(0755), ec);
  // 2. The game files, checked.
  step("Copying and checking your game files", 0.05);
  const setup::PortableSource source = a->sources[a->chosen];
  if (!setup::CopyGameFiles(source, a->manifest, dest / "GameFiles" / "Game", [a](double f, const std::string& d) {
        std::lock_guard<std::mutex> lock(a->mutex);
        a->fraction = f;
        a->detail = d;
      }, a->cancel, error)) {
    return false;
  }
  // 3. The native game.
  const setup::PortableLayout l{dest, InstalledDataDir()};
  setup::AtomicWrite(dest / "installed.txt", "data=local application data\n");
  setup::PortableSource installed;
  installed.path = dest / "GameFiles" / "Game";
  installed.check = setup::CheckGame(setup::GameSource::Folder(installed.path), a->manifest);
  if (!setup::PreparePortable(l, installed, a->manifest, [a](setup::PortablePhase p, double f, const std::string& d) {
        std::lock_guard<std::mutex> lock(a->mutex);
        a->install_step = PhaseName(p);
        a->fraction = f;
        a->detail = d;
      }, a->cancel, error)) {
    return false;
  }
  // 4. Shortcuts (Windows), recorded for the uninstall.
  std::string record = "data=local application data\n";
  const fs::path exe = dest / kLauncherName;
  if (a->start_menu) {
    const fs::path menu = setup::GetKnownFolder(setup::KnownFolder::kStartMenuPrograms);
    if (!menu.empty()) {
      const fs::path play = menu / (std::string(kGameName) + ".lnk");
      const fs::path remove = menu / ("Uninstall " + std::string(kGameName) + ".lnk");
      if (setup::CreateShortcut(play, exe, "", dest, "Kingdom Under Fire: Circle of Doom")) {
        record += "shortcut=" + setup::PathUtf8(play) + "\n";
      }
      if (setup::CreateShortcut(remove, exe, "--uninstall", dest, "Uninstall Kingdom Under Fire: Circle of Doom")) {
        record += "shortcut=" + setup::PathUtf8(remove) + "\n";
      }
    }
  }
  if (a->desktop) {
    const fs::path desk = setup::GetKnownFolder(setup::KnownFolder::kDesktop);
    const fs::path link = desk / (std::string(kGameName) + ".lnk");
    if (!desk.empty() && setup::CreateShortcut(link, exe, "", dest, "Kingdom Under Fire: Circle of Doom")) {
      record += "shortcut=" + setup::PathUtf8(link) + "\n";
    }
  }
#if defined(_WIN32)
  if (a->start_menu && RegisterUninstallEntry(dest, exe)) record += "apps_entry=1\n";  // not with --no-shortcuts
#endif
  setup::AtomicWrite(dest / "installed.txt", record);
  return true;
}

void StartInstall(App* a) {
  if (a->busy) return;
  if (a->worker.joinable()) a->worker.join();
  a->busy = true;
  a->cancel = false;
  a->screen = Screen::kSetupInstalling;
  a->worker = std::thread([a] {
    std::string error;
    const bool ok = InstallTo(a, Utf8Path(a->destination.c_str()), &error);
    std::lock_guard<std::mutex> lock(a->mutex);
    a->result_ok = ok;
    a->result_error = error;
    a->busy = false;
  });
}

// Uninstalling an installed copy: its folder and shortcuts, and on request its data. A program cannot delete its own
// file on Windows, so a copy of it in the temporary folder does the work (--uninstall-from).
int Uninstall(const fs::path& dest, bool with_data) {
  std::string record;
  setup::ReadFile(dest / "installed.txt", &record);
  std::istringstream in(record);
  std::string line;
  std::error_code ec;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("shortcut=", 0) == 0) fs::remove(setup::PathFromUtf8(line.substr(9)), ec);
#if defined(_WIN32)
    if (line == "apps_entry=1") RemoveUninstallEntry();
#endif
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));  // the installed launcher has ended
  for (int attempt = 0; attempt < 10; ++attempt) {
    fs::remove_all(dest, ec);
    if (!fs::exists(dest, ec)) break;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (with_data) fs::remove_all(InstalledDataDir(), ec);
  const bool ok = !fs::exists(dest, ec);
  Notify(ok, ok ? (with_data ? "The game, its saves and its settings were removed."
                             : "The game was removed. Your saves and settings were kept.")
                : "Some files could not be removed: " + setup::PathUtf8(dest));
  return ok ? 0 : 1;
}

int AskUninstall(const fs::path& self, const fs::path& dest) {
  const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
                                              {0, 2, "Remove, also saves"},
                                              {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Remove (keep saves)"}};
  const std::string text = "Remove Kingdom Under Fire: Circle of Doom from " + setup::PathUtf8(dest) +
                           "?\nYour own game dump is not touched. Saves and settings are kept unless you choose to "
                           "remove them too.";
  const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, nullptr, "Uninstall", text.c_str(), 3, buttons, nullptr};
  int choice = 0;
  if (!SDL_ShowMessageBox(&box, &choice) || choice == 0) return 0;
  std::error_code ec;
  const fs::path temp = fs::temp_directory_path(ec) / ("kuf-uninstall-" + std::to_string(SDL_GetTicks()));
  fs::create_directories(temp, ec);
  const fs::path copy = temp / self.filename();
  fs::copy_file(self, copy, ec);
  std::vector<std::string> argv = {setup::PathUtf8(copy), "--uninstall-from", setup::PathUtf8(dest)};
  if (choice == 2) argv.push_back("--with-data");
  return setup::StartDetached(argv, temp, temp / "uninstall.log", {}) ? 0 : 1;
}

void SDLCALL OnPicked(void* userdata, const char* const* files, int) {
  App* a = static_cast<App*>(userdata);
  if (files && files[0]) {
    std::lock_guard<std::mutex> lock(a->mutex);
    a->pending_drops.push_back(files[0]);
  }
}

void OpenFolder(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::string url = "file://" + setup::PathUtf8(dir.generic_u8string());
#if defined(_WIN32)
  url = "file:///" + setup::PathUtf8(dir.generic_u8string());
#endif
  SDL_OpenURL(url.c_str());
}

const char* PhaseName(setup::PortablePhase p) {
  switch (p) {
    case setup::PortablePhase::kCopy: return "Checking and copying the game files";
    case setup::PortablePhase::kGenerate: return "Reading the game's code";
    case setup::PortablePhase::kCompile: return "Compiling the native game";
    case setup::PortablePhase::kLink: return "Linking the game";
  }
  return "";
}

void Heading(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kGold);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::PopStyleColor();
  ImGui::Separator();
}

void Wrapped(const std::string& text, ImVec4 color = kText) {
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::PopStyleColor();
}

void DropHint() {
  Wrapped("You can also drag your game files onto this window: a game folder, an Xbox 360 disc image (.iso) or "
          "default.xex. Nothing is changed until you choose Play.", kMuted);
}

void Frame(App* a) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::Begin("launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  const float w = ImGui::GetContentRegionAvail().x;
  switch (a->screen) {
    case Screen::kNoGameFiles: {
      Heading("Drag your game files here");
      Wrapped(setup::RetailDecryptionAvailable()
                  ? "No game files were found. Copy your own game files into the GameFiles folder of this portable "
                    "edition: the folder that contains default.xex, or an Xbox 360 disc image (.iso). A retail copy "
                    "works as it is: its encrypted default.xex is decrypted during the preparation."
                  : "No game files were found. Copy your own game files into the GameFiles folder of this portable "
                    "edition: the folder that contains default.xex, an Xbox 360 disc image (.iso), or a retail dump "
                    "together with an unencrypted default.xex of the same version.");
      Wrapped("GameFiles: " + setup::PathUtf8(a->layout.GameFiles()), kMuted);
      DropHint();
      if (BigButton("Open the GameFiles folder")) OpenFolder(a->layout.GameFiles());
      ImGui::SameLine();
      if (BigButton("Scan again")) Rescan(a);
      ImGui::SameLine();
      if (BigButton("Choose a game folder...")) SDL_ShowOpenFolderDialog(OnPicked, a, a->window, nullptr, false);
      ImGui::SameLine();
      if (BigButton("Choose a disc image or default.xex...")) {
        static const SDL_DialogFileFilter filters[] = {{"Disc image or default.xex", "iso;ISO;xex;XEX"}};
        SDL_ShowOpenFileDialog(OnPicked, a, a->window, filters, 1, nullptr, false);
      }
      break;
    }
    case Screen::kChoose: {
      Heading("Several game sources were found");
      Wrapped("Choose the game files to use:");
      for (size_t i = 0; i < a->sources.size(); ++i) {
        const setup::PortableSource& s = a->sources[i];
        ImGui::BeginDisabled(!s.check.Ok());
        if (ImGui::RadioButton((s.label + "##" + std::to_string(i)).c_str(), a->chosen == int(i))) a->chosen = int(i);
        ImGui::EndDisabled();
        if (!s.check.Ok()) Wrapped("    " + setup::GameCheckProblem(s.check, a->manifest), kBad);
      }
      if (BigButton("Continue", 220)) {
        a->ready = setup::PortableReady(a->layout, a->sources[a->chosen], a->state, &a->not_ready);
        a->screen = Screen::kDetected;
      }
      break;
    }
    case Screen::kDetected: {
      const setup::PortableSource& s = a->sources[a->chosen];
      Heading(s.check.Ok() ? "Game files detected" : "These game files cannot be used");
      Wrapped(s.label);
      if (s.check.Ok()) {
        const setup::XexInfo& x = s.check.separate ? s.check.separate_xex : s.check.dump_xex;
        Wrapped("Kingdom Under Fire: Circle of Doom, title " + std::string("424507D1") + ", version " +
                    setup::XexVersionString(x.version) + " - the supported release.", kGood);
        Wrapped(a->ready ? "Prepared: Play starts the game." : a->not_ready + " Play prepares the native game "
                    "first (a few minutes, once).", a->ready ? kText : kMuted);
      } else {
        Wrapped(setup::GameCheckProblem(s.check, a->manifest), kBad);
      }
      DropHint();
      ImGui::BeginDisabled(!s.check.Ok());
      if (BigButton("Play", 220)) {
        if (a->ready) {
          Launch(a);
        } else {
          StartPrepare(a, true);
        }
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (BigButton("Settings", 160)) a->screen = Screen::kSettings;
      ImGui::SameLine();
      ImGui::BeginDisabled(a->state.game_dir.empty() || !a->ready);
      if (BigButton("Verify Game Files", 230)) StartVerify(a);
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (BigButton("Change Game Folder", 240)) SDL_ShowOpenFolderDialog(OnPicked, a, a->window, nullptr, false);
      if (a->sources.size() > 1) {
        ImGui::SameLine();
        if (BigButton("Other game files...", 220)) a->screen = Screen::kChoose;
      }
      break;
    }
    case Screen::kPreparing:
    case Screen::kVerifying: {
      const bool verifying = a->screen == Screen::kVerifying;
      Heading(verifying ? "Verifying the game files" : "Preparing the native game");
      double f;
      std::string phase, detail;
      {
        std::lock_guard<std::mutex> lock(a->mutex);
        f = a->fraction;
        phase = verifying ? "Comparing every file with the supported release" : PhaseName(a->phase);
        detail = a->detail;
      }
      Wrapped(phase);
      ImGui::ProgressBar(float(f), ImVec2(w, ImGui::GetFontSize() * 1.6f));
      Wrapped(detail, kMuted);
      Wrapped(verifying ? "Your files are only read." : "Your game files are only read. Cancel is safe: what is "
              "finished is kept for the next attempt.", kMuted);
      if (BigButton("Cancel", 180)) a->cancel = true;
      if (!a->busy) {
        if (a->worker.joinable()) a->worker.join();
        if (verifying) {
          if (a->cancel) {
            Rescan(a);
          } else if (a->verify_bad.empty()) {
            ShowMessage(a, "The game files are intact", "Every file matches the supported release.", Screen::kDetected);
          } else {
            std::string list;
            for (size_t i = 0; i < a->verify_bad.size() && i < 12; ++i) list += "\n  " + a->verify_bad[i];
            ShowMessage(a, "Some game files differ",
                        std::to_string(a->verify_bad.size()) + " files are missing or differ from the supported "
                        "release:" + list + "\nCopy them again from your dump, then prepare again.", Screen::kDetected);
          }
        } else if (a->result_ok) {
          Rescan(a);
          if (a->play_when_ready) Launch(a);
        } else {
          Rescan(a);
          ShowMessage(a, a->cancel ? "Cancelled" : "The game could not be prepared",
                      a->result_error + "\nYour game files were not changed.", Screen::kDetected);
        }
      }
      break;
    }
    case Screen::kSettings: {
      Heading("Settings");
      Wrapped("Everything this edition writes stays in its folder:");
      Wrapped("Saves: " + setup::PathUtf8(a->layout.Saves()), kMuted);
      Wrapped("Settings: " + setup::PathUtf8(a->layout.User()), kMuted);
      Wrapped("Logs: " + setup::PathUtf8(a->layout.Logs()), kMuted);
      if (BigButton("Open the saves folder")) OpenFolder(a->layout.Saves());
      ImGui::SameLine();
      if (BigButton("Open the logs folder")) OpenFolder(a->layout.Logs());
      Wrapped("The game's own options (display, controls, frame rate) are in the game: PC Settings in its menus.",
              kMuted);
      if (BigButton("Back", 160)) a->screen = Screen::kDetected;
      break;
    }
    case Screen::kReadOnly: {
      Heading("This folder cannot be written");
      Wrapped("The portable edition keeps its saves, settings and prepared game in its own folder, but " +
              setup::PathUtf8(a->layout.root) + " cannot be written (read-only media or no permission). Move the "
              "folder somewhere writable, or choose a writable folder for its data.");
      if (BigButton("Choose a writable folder...")) SDL_ShowOpenFolderDialog(OnPicked, a, a->window, nullptr, false);
      ImGui::SameLine();
      if (BigButton("Quit", 140)) a->quit = true;
      break;
    }
    case Screen::kSetupDrop: {
      Heading("Drag your game files here");
      Wrapped(std::string("Kingdom Under Fire: Circle of Doom - native Windows port, setup. Drop your own game "
                          "files onto this window: an Xbox 360 disc image (.iso), the extracted game folder, or its "
                          "default.xex. ") +
              (setup::RetailDecryptionAvailable()
                   ? "A retail copy works as it is: its encrypted default.xex is decrypted during the installation."
                   : "A retail copy also needs an unencrypted default.xex of the same version (this setup does not "
                     "decrypt)."));
      Wrapped("Nothing is installed or changed until you choose Install.", kMuted);
      if (BigButton("Choose a game folder...")) {
        a->picking = 0;
        SDL_ShowOpenFolderDialog(OnPicked, a, a->window, nullptr, false);
      }
      ImGui::SameLine();
      if (BigButton("Choose a disc image or default.xex...")) {
        a->picking = 0;
        static const SDL_DialogFileFilter filters[] = {{"Disc image or default.xex", "iso;ISO;xex;XEX"}};
        SDL_ShowOpenFileDialog(OnPicked, a, a->window, filters, 1, nullptr, false);
      }
      break;
    }
    case Screen::kSetupConfirm: {
      const setup::PortableSource& src = a->sources[a->chosen];
      Heading(src.check.Ok() ? "Game files detected" : "These game files cannot be used");
      Wrapped(src.label);
      if (src.check.Ok()) {
        Wrapped("Kingdom Under Fire: Circle of Doom, the supported release (version " +
                    setup::XexVersionString((src.check.separate ? src.check.separate_xex : src.check.dump_xex).version) +
                    ").", kGood);
      } else {
        Wrapped(setup::GameCheckProblem(src.check, a->manifest), kBad);
        Wrapped("Drop other game files onto this window, or choose them again.", kMuted);
      }
      if (src.check.Ok()) {
        ImGui::Spacing();
        Wrapped("Install to:");
        static char buf[1024];
        std::snprintf(buf, sizeof(buf), "%s", a->destination.c_str());
        ImGui::SetNextItemWidth(w - 220);
        if (ImGui::InputText("##dest", buf, sizeof(buf))) a->destination = buf;
        ImGui::SameLine();
        if (BigButton("Choose...", 200)) {
          a->picking = 1;
          SDL_ShowOpenFolderDialog(OnPicked, a, a->window, nullptr, false);
        }
        ImGui::Checkbox("Start menu shortcut", &a->start_menu);
        ImGui::SameLine();
        ImGui::Checkbox("Desktop shortcut", &a->desktop);
        Wrapped("About " + setup::HumanBytes(src.check.bytes + (uint64_t(700) << 20)) + " in that folder; saves and "
                "settings in your user folder (" + setup::PathUtf8(InstalledDataDir()) + "). No administrator "
                "rights are needed.", kMuted);
        if (BigButton("Install", 220)) StartInstall(a);
      }
      ImGui::SameLine();
      if (BigButton("Back", 160)) a->screen = Screen::kSetupDrop;
      break;
    }
    case Screen::kSetupInstalling: {
      Heading("Installing");
      double f;
      std::string step, detail;
      {
        std::lock_guard<std::mutex> lock(a->mutex);
        f = a->fraction;
        step = a->install_step;
        detail = a->detail;
      }
      Wrapped(step);
      ImGui::ProgressBar(float(f), ImVec2(w, ImGui::GetFontSize() * 1.6f));
      Wrapped(detail, kMuted);
      Wrapped("Your game files are only read. Cancel is safe.", kMuted);
      if (BigButton("Cancel", 180)) a->cancel = true;
      if (!a->busy) {
        if (a->worker.joinable()) a->worker.join();
        if (a->result_ok) {
          a->screen = Screen::kSetupDone;
        } else {
          ShowMessage(a, a->cancel ? "Cancelled" : "The installation did not finish",
                      a->result_error + "\nYour game files were not changed.", Screen::kSetupConfirm);
        }
      }
      break;
    }
    case Screen::kSetupDone: {
      Heading("Installed");
      Wrapped("Kingdom Under Fire: Circle of Doom is installed in " + a->destination + ".");
      Wrapped("Start it from the Start menu, or with Play now. To remove it, use \"Uninstall Kingdom Under Fire - "
              "Circle of Doom\" in the Start menu.", kMuted);
      if (BigButton("Play now", 220)) {
        const fs::path exe = Utf8Path(a->destination.c_str()) / kLauncherName;
        setup::StartDetached({setup::PathUtf8(exe), "--play"}, Utf8Path(a->destination.c_str()),
                             InstalledDataDir() / "logs" / "launcher.log", {});
        a->quit = true;
      }
      ImGui::SameLine();
      if (BigButton("Finish", 180)) a->quit = true;
      break;
    }
    case Screen::kMessage: {
      Heading(a->message_title.c_str());
      Wrapped(a->message_text);
      if (BigButton("OK", 160)) a->screen = a->after_message;
      break;
    }
  }
  ImGui::End();
}

// Where this portable copy keeps its data when its own folder cannot be written: remembered per folder.
fs::path RememberedDataDir(const fs::path& root) {
  std::string text;
  setup::ReadFile(setup::UserConfigDir() / "portable-data.txt", &text);
  const std::string key = setup::PathUtf8(root) + "\t";
  size_t at = text.find(key);
  if (at == std::string::npos) return {};
  const size_t end = text.find('\n', at);
  return setup::PathFromUtf8(text.substr(at + key.size(), end == std::string::npos ? std::string::npos : end - at - key.size()));
}
void RememberDataDir(const fs::path& root, const fs::path& data) {
  std::string text;
  const fs::path file = setup::UserConfigDir() / "portable-data.txt";
  setup::ReadFile(file, &text);
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  setup::AtomicWrite(file, text + setup::PathUtf8(root) + "\t" + setup::PathUtf8(data) + "\n");
}

// What a dropped file or folder was taken as, in launcher.log (support, and the Windows acceptance test's drop check).
void LogDrop(App* a, const std::string& utf8) {
  std::error_code ec;
  const fs::path logs = a->setup_mode ? InstalledDataDir() / "logs" : a->layout.Logs();
  fs::create_directories(logs, ec);
  std::string what = std::to_string(a->sources.size()) + " source(s)";
  if (!a->sources.empty()) {
    const setup::PortableSource& s = a->sources[std::min<size_t>(a->chosen, a->sources.size() - 1)];
    what += ", chosen " + s.label + (s.check.Ok() ? " (usable)" : " (not usable)");
  }
  std::ofstream(logs / "launcher.log", std::ios::app | std::ios::binary)
      << "drop: " << utf8 << ": " << what << (a->setup_mode ? ", setup" : ", portable") << "\n";
}

void HandlePicked(App* a, const std::string& utf8) {
  const fs::path p = Utf8Path(utf8.c_str());
  if (a->picking == 1) {  // the setup's destination: a folder for the game inside the chosen one
    a->picking = 0;
    a->destination = setup::PathUtf8(p / kGameName);
    return;
  }
  if (a->setup_mode) {
    if (a->screen == Screen::kSetupDrop || a->screen == Screen::kSetupConfirm) {
      a->picked = p;
      Rescan(a);
      a->screen = Screen::kSetupConfirm;
    }
    return;
  }
  if (a->screen == Screen::kReadOnly) {
    if (setup::CanCreateIn(p)) {
      a->layout.data = p / "KUF Portable Data";
      RememberDataDir(a->layout.root, a->layout.data);
      Rescan(a);
    }
    return;
  }
  a->picked = p;  // only noted: nothing is prepared or written until Play
  Rescan(a);
}

}  // namespace

// Waits (at most a minute) until the process `pid` ended: the game that started this launcher for its restart.
void WaitForExit(long long pid) {
  if (pid <= 0) return;
#if defined(_WIN32)
  if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid))) {
    WaitForSingleObject(h, 60000);
    CloseHandle(h);
  }
#else
  for (int k = 0; k < 600 && kill(pid_t(pid), 0) == 0; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
}

int main(int argc, char** argv) {
  const char* base = SDL_GetBasePath();
  App app;
  App* a = &app;
  a->layout = setup::DefaultPortableLayout(Utf8Path(base));
#if defined(_WIN32)
  wchar_t self_w[32768];
  const DWORD self_n = GetModuleFileNameW(nullptr, self_w, 32768);
  a->self = fs::path(std::wstring(self_w, self_n));
#else
  std::error_code self_ec;
  a->self = fs::read_symlink("/proc/self/exe", self_ec);
#endif
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--uninstall-from" && i + 1 < argc) {
      const bool with_data = i + 2 < argc && std::string(argv[i + 2]) == "--with-data";
      return Uninstall(Utf8Path(argv[i + 1]), with_data);
    }
    if (arg == "--uninstall") return AskUninstall(a->self, a->layout.root);
    if (arg == "--setup") a->setup_mode = true;
    if (arg == "--after-restart" && i + 1 < argc) WaitForExit(std::atoll(argv[i + 1]));
    if (arg == "--rollback-update") {  // back to the version before the last update (the launcher, outside Runtime/,
                                       // can move Runtime/; kuf-update.exe inside it cannot on Windows)
      std::string now, why;
      const bool ok = setup::update::Rollback({a->layout.root}, &now, &why);
      Notify(ok, ok ? "Back to version " + now + "." : "No change: " + why);
      return ok ? 0 : 1;
    }
  }
  if (ReadPayloadTrailer(a)) a->setup_mode = true;
  std::error_code mark_ec;
  if (!a->setup_mode && fs::exists(a->layout.root / "installed.txt", mark_ec)) a->layout.data = InstalledDataDir();
  // An update the game's Check for Updates staged (M17G): put in place before anything reads Runtime/, while the game
  // is not running. The launcher already running is the version before (it renames itself to .previous).
  if (!a->setup_mode) {
    std::string applied, why;
    const bool ok = setup::update::ApplyStagedUpdate({a->layout.root}, &applied, &why);
    if (!applied.empty() || !ok) {
      fs::create_directories(a->layout.Logs(), mark_ec);
      std::ofstream(a->layout.Logs() / "launcher.log", std::ios::app | std::ios::binary)
          << (ok ? "update: now " + applied : "update: " + why) << "\n";
    }
  }
  std::string text;
  if (!setup::ReadFile(a->layout.Runtime() / "assets" / "game_v1.tsv", &text) &&
      !setup::ReadFile(a->layout.root / "game_v1.tsv", &text) && !ReadEmbeddedManifest(a, &text)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Kingdom Under Fire: Circle of Doom",
                             "This portable folder is incomplete (Runtime/assets/game_v1.tsv is missing). Extract the "
                             "whole archive again.", nullptr);
    return 1;
  }
  a->manifest = setup::ParseGameManifest(text, "v1");
  bool play = false, prepare = false, print_launch = false, no_shortcuts = false;
  std::string install_to;
  fs::path self_test;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--play") play = true;
    else if (arg == "--prepare") prepare = true;
    else if (arg == "--print-launch") print_launch = true;
    else if (arg == "--install-to" && i + 1 < argc) install_to = argv[++i];
    else if (arg == "--no-shortcuts") no_shortcuts = true;
    else if (arg == "--setup") continue;
    else if (arg == "--self-test" && i + 1 < argc) self_test = Utf8Path(argv[++i]);
    else if (arg == "--after-restart" && i + 1 < argc) {  // the game restarted for an update: straight back in
      ++i;
      play = true;
    }
    else a->picked = Utf8Path(argv[i]);  // dropped onto the launcher, or given
  }
  const bool writable = setup::CanCreateIn(a->layout.data.parent_path()) &&
                        (!fs::exists(a->layout.data) || setup::CanCreateIn(a->layout.data));
  if (!writable) {
    const fs::path remembered = RememberedDataDir(a->layout.root);
    if (!remembered.empty()) a->layout.data = remembered;
  }
  Rescan(a);
  if (!writable && !a->setup_mode && RememberedDataDir(a->layout.root).empty()) a->screen = Screen::kReadOnly;
  if (a->setup_mode && !install_to.empty()) {  // no window (tests)
    if (a->sources.empty() || !a->sources[0].check.Ok()) {
      std::fprintf(stderr, "setup: the game files cannot be used\n");
      return 1;
    }
    a->start_menu = a->desktop = !no_shortcuts;
    a->desktop = false;
    std::string error;
    const bool ok = InstallTo(a, Utf8Path(install_to.c_str()), &error);
    std::printf("%s\n", ok ? "installed" : error.c_str());
    return ok ? 0 : 1;
  }
  if (a->setup_mode) {
    a->destination = setup::PathUtf8(DefaultDestination());
    a->screen = a->picked.empty() ? Screen::kSetupDrop : Screen::kSetupConfirm;
  }

  if (!self_test.empty()) {  // no window: what the launcher sees, for the automated tests
    std::ofstream out(self_test / "self-test.txt");
    out << "root=" << setup::PathUtf8(a->layout.root) << "\nsources=" << a->sources.size() << "\n";
    for (const setup::PortableSource& s : a->sources) out << "source=" << s.label << (s.check.Ok() ? " ok" : " bad") << "\n";
    out << "ready=" << (a->ready ? 1 : 0) << "\nnot_ready=" << a->not_ready << "\n";
    return 0;
  }
  if (prepare) {
    if (a->sources.empty() || !a->sources[a->chosen].check.Ok()) {
      std::fprintf(stderr, "Play KUF: no usable game files in %s\n", setup::PathUtf8(a->layout.GameFiles()).c_str());
      return 1;
    }
    std::atomic<bool> no_cancel{false};
    std::string error;
    int last = -1;
    const bool ok = setup::PreparePortable(a->layout, a->sources[a->chosen], a->manifest,
        [&](setup::PortablePhase p, double f, const std::string&) {
          const int pct = int(f * 100);
          if (pct / 10 != last / 10 || pct == 100) std::printf("[%s] %d\n", PhaseName(p), pct);
          last = pct;
          std::fflush(stdout);
        }, no_cancel, &error);
    if (!ok) std::fprintf(stderr, "Play KUF: %s\n", error.c_str());
    return ok ? 0 : 1;
  }
  if (print_launch) {
    if (!a->ready) {
      std::fprintf(stderr, "Play KUF: not prepared: %s\n", a->not_ready.c_str());
      return 1;
    }
    const setup::LaunchCommand c = setup::PortableLaunchCommand(a->layout, a->state);
    for (const std::string& e : c.env) std::printf("env %s\n", e.c_str());
    for (const std::string& x : c.argv) std::printf("arg %s\n", x.c_str());
    return 0;
  }
  if (play && a->screen == Screen::kDetected && a->ready) {
    Launch(a);
    return 0;
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    std::fprintf(stderr, "Play KUF: %s\n", SDL_GetError());
    return 1;
  }
  const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()) > 0
                          ? SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()) : 1.0f;
  int width = int(1100 * scale), height = int(640 * scale);
  const char* size = SDL_getenv("KUF_LAUNCHER_SIZE");
  if (size) std::sscanf(size, "%dx%d", &width, &height);
  const char* shot = SDL_getenv("KUF_LAUNCHER_SHOT");
  a->window = SDL_CreateWindow("Kingdom Under Fire: Circle of Doom", width, height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  a->renderer = a->window ? SDL_CreateRenderer(a->window, nullptr) : nullptr;
  if (!a->renderer) {
    std::fprintf(stderr, "Play KUF: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(a->renderer, 1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  LoadFont(20.0f * scale);
  Theme(scale);
  ImGui_ImplSDL3_InitForSDLRenderer(a->window, a->renderer);
  ImGui_ImplSDLRenderer3_Init(a->renderer);

  int frames = 0;
  while (!a->quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      if (e.type == SDL_EVENT_QUIT && !a->busy) a->quit = true;
      if (e.type == SDL_EVENT_DROP_FILE && e.drop.data && !a->busy) {
        std::lock_guard<std::mutex> lock(a->mutex);
        a->pending_drops.push_back(e.drop.data);
      }
    }
    std::vector<std::string> drops;
    {
      std::lock_guard<std::mutex> lock(a->mutex);
      drops.swap(a->pending_drops);
    }
    for (const std::string& d : drops) {
      HandlePicked(a, d);
      LogDrop(a, d);
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    Frame(a);
    ImGui::Render();
    SDL_SetRenderDrawColor(a->renderer, 23, 15, 12, 255);
    SDL_RenderClear(a->renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), a->renderer);
    if (shot && ++frames == 4) {  // a few frames: the layout settles
      if (SDL_Surface* pixels = SDL_RenderReadPixels(a->renderer, nullptr)) {
        SDL_SaveBMP(pixels, shot);
        SDL_DestroySurface(pixels);
      }
      a->quit = true;
    }
    SDL_RenderPresent(a->renderer);
  }
  a->cancel = true;
  if (a->worker.joinable()) a->worker.join();
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(a->renderer);
  SDL_DestroyWindow(a->window);
  SDL_Quit();
  return 0;
}
