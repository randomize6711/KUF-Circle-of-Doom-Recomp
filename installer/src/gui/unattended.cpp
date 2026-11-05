// The setup without its window (installer/README.md "Unattended"): the same checks as the pages (game folder,
// target folder, system, space) and the same core Install / Uninstall, driven by options. For scripted installs and
// the release tests (tools/package/appimage_test.sh); a player uses the window.
//
//   kuf-setup --install-to FOLDER --game-dir FOLDER [--no-menu] [--desktop]   install, or repair an installation
//   kuf-setup --remove FOLDER [--keep-shortcuts] [--remove-user-data]         uninstall
//   (an install takes --jobs N: the local build's parallel jobs; default: from this machine)
//   kuf-setup --check-system                                                  what the window's first page checks
// An install also checks the graphics driver (core/system_check.h) and refuses one that cannot run the game;
// --ignore-graphics-check installs anyway, saying so (tests in sandboxes without a graphics card).
// Exit: 0 done (--check-system: compatible), 1 failed, refused or incompatible (the reason on stderr), 2 usage.
#include "gui/unattended.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>

#include "core/game_data.h"
#include "core/install.h"
#include "core/system_check.h"
#include "core/util.h"

namespace kuf::setup {
namespace {

std::atomic<bool> g_cancel{false};
void OnCancelSignal(int) { g_cancel = true; }

int Fail(const std::string& why) {
  std::fprintf(stderr, "kuf-setup: %s\n", why.c_str());
  return 1;
}

fs::path Expand(const std::string& text) {
  if (!text.empty() && text[0] == '~') return HomeDir() / fs::path(text.substr(1)).relative_path();
  return text;
}

void Progress(Stage s, double f, const std::string& detail) {
  static int last_stage = -1, last_tenth = -1;
  const int tenth = int(f * 10);
  if (int(s) == last_stage && tenth == last_tenth) return;
  last_stage = int(s);
  last_tenth = tenth;
  std::printf("[%s] %3d%% %s\n", StageName(s), int(f * 100), detail.c_str());
  std::fflush(stdout);
}

}  // namespace

bool IsUnattended(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--install-to" || a == "--remove" || a == "--check-system") return true;
  }
  return false;
}

int RunUnattended(int argc, char** argv, const fs::path& exe) {
  fs::path install_to, remove, game, assets_arg, payload_arg;
  int jobs = 0;
  bool menu = true, desktop = false, keep_shortcuts = false, remove_data = false, check_only = false,
       ignore_graphics = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--install-to") install_to = Expand(value());
    else if (a == "--remove") remove = Expand(value());
    else if (a == "--game-dir") game = Expand(value());
    else if (a == "--assets") assets_arg = value();
    else if (a == "--payload") payload_arg = value();
    else if (a == "--no-menu") menu = false;
    else if (a == "--desktop") desktop = true;
    else if (a == "--keep-shortcuts") keep_shortcuts = true;
    else if (a == "--remove-user-data") remove_data = true;
    else if (a == "--check-system") check_only = true;
    else if (a == "--ignore-graphics-check") ignore_graphics = true;
    else if (a == "--jobs") jobs = std::atoi(value().c_str());
    else {
      std::fprintf(stderr, "kuf-setup: unknown option %s\n", a.c_str());
      return 2;
    }
  }

  if (!remove.empty()) {
    if (!ReadRecord(remove)) return Fail("no installation of the game in " + remove.string());
    RemoveOptions o;
    o.shortcuts = !keep_shortcuts;
    o.steam = !keep_shortcuts;
    o.user_data = remove_data;
    const Result r = Uninstall(remove, o, Progress);
    for (const auto& n : r.notes) std::printf("%s\n", n.c_str());
    return r.ok ? 0 : Fail(r.error);
  }

  // The setup's own files: as the window finds them (next to the program, or the AppImage's share/kuf-setup).
  const fs::path dir = exe.parent_path();
  fs::path assets, payload;
  std::error_code ec;
  for (const fs::path& c : {assets_arg, fs::path(Env("KUF_SETUP_ASSETS")), dir / "assets",
                            dir / "../share/kuf-setup/assets"}) {
    if (!c.empty() && fs::exists(c / "game_v1.tsv", ec)) {
      assets = fs::canonical(c, ec);
      break;
    }
  }
  for (const fs::path& c : {payload_arg, dir / "../share/kuf-setup/payload/package"}) {
    if (!c.empty() && fs::exists(c / "MANIFEST.sha256", ec)) {
      payload = fs::canonical(c, ec);
      break;
    }
  }
  if (assets.empty() || payload.empty()) return Fail("the setup's files or its game package are missing");
  std::string list, requires_text;
  if (!ReadFile(assets / "game_v1.tsv", &list)) return Fail("the setup's game-file list is missing");
  Plan plan;
  plan.manifest = ParseGameManifest(list, "Xbox 360, base version");
  // The system, as the window's first page: the libraries the game needs (requires.txt, from its binaries) and the
  // graphics driver.
  std::vector<std::string> problems;
  if (ReadFile(payload.parent_path() / "requires.txt", &requires_text)) {
    problems = CompatibilityProblems(ParseRequirements(requires_text), HostGlibc(), HostGlibcxx());
  }
  const VulkanReport vulkan = CheckVulkan();
  const auto graphics = VulkanProblems(vulkan);
  std::printf("system: %s; glibc %s, libstdc++ GLIBCXX %s\n", PlatformSummary(DetectPlatform()).c_str(),
              HostGlibc().c_str(), HostGlibcxx().empty() ? "unknown" : HostGlibcxx().c_str());
  if (!vulkan.error.empty()) std::printf("vulkan: %s\n", vulkan.error.c_str());
  for (const auto& d : vulkan.devices) {
    std::printf("vulkan: %s (%s), Vulkan %s: %s\n", d.name.c_str(), d.type.c_str(),
                VulkanVersionString(d.api_version).c_str(),
                d.type == "CPU (software)" ? "a software renderer (not counted: far too slow for the game)"
                : d.missing.empty()        ? "has everything the game needs"
                                           : "lacks what the game needs");
    for (const auto& m : d.missing) std::printf("vulkan:   missing %s\n", m.c_str());
  }
  for (const auto& p : problems) std::fprintf(stderr, "kuf-setup: %s\n", p.c_str());
  for (const auto& p : graphics) std::fprintf(stderr, "kuf-setup: %s\n", p.c_str());
  if (check_only) {
    const bool ok = problems.empty() && graphics.empty();
    std::printf("%s\n", ok ? "compatible: the game can be installed and run" : "NOT compatible (see above)");
    return ok ? 0 : 1;
  }
  if (!problems.empty()) return Fail("this system is too old for the game");
  if (!graphics.empty()) {
    if (!ignore_graphics) return Fail("the graphics driver cannot run the game (--ignore-graphics-check to install anyway)");
    std::fprintf(stderr, "kuf-setup: installing anyway (--ignore-graphics-check)\n");
  }

  if (game.empty()) return Fail("--game-dir: the folder with your game files (default.xex) is needed");
  plan.game_source = FindGameRoot(game);
  const GameCheck c = CheckGameFolder(plan.game_source, plan.manifest);
  if (!c.folder_ok) return Fail("the game folder cannot be read: " + plan.game_source.string());
  if (!c.executable_found) return Fail("no default.xex in " + plan.game_source.string());
  if (c.executable_encrypted) {
    return Fail("this default.xex is encrypted: this setup contains no console decryption keys and cannot decrypt it; "
                "it can only use an unencrypted default.xex (docs/INSTALL_GUIDE.md)");
  }
  if (!c.executable_known) return Fail("unsupported game version (only the original Xbox 360 base version)");
  if (!c.missing.empty() || !c.wrong_size.empty()) {
    return Fail(std::to_string(c.missing.size()) + " game files missing, " + std::to_string(c.wrong_size.size()) +
                " with the wrong size");
  }

  plan.root = install_to;
  const Target target = CheckTarget(plan.root);
  if (target == Target::kNotEmpty) return Fail(plan.root.string() + " contains other files: choose an empty folder");
  if (target == Target::kNotWritable) return Fail(plan.root.string() + " is not writable");
  if (target == Target::kInvalid) return Fail(plan.root.string() + " cannot be used (a full path, not your home)");
  plan.package = payload;
  if (fs::exists(payload.parent_path() / "allowed_signers", ec)) plan.signers = payload.parent_path() / "allowed_signers";
  plan.setup_binary = exe;
  plan.setup_assets = assets;
  plan.icon = assets / "icon-256.png";
  plan.build_jobs = jobs;
  plan.menu = menu;
  plan.desktop = desktop;
  if (target == Target::kNew && FreeBytes(plan.root) < SpaceNeeded(plan)) {
    return Fail("not enough free space: " + HumanBytes(SpaceNeeded(plan)) + " needed");
  }
  std::string v;
  ReadFile(payload / "VERSION", &v);
  while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
  std::printf("%s %s into %s (game files from %s)%s\n", target == Target::kExisting ? "repairing" : "installing",
              v.c_str(), plan.root.c_str(), plan.game_source.c_str(),
              plan.signers.empty() ? "; no release key in this setup: updates will be refused" : "");
  // SIGINT or SIGTERM cancels, as the window's Cancel does: the setup's own temporary folders are removed.
  std::signal(SIGINT, OnCancelSignal);
  std::signal(SIGTERM, OnCancelSignal);
  const Result r = Install(plan, Progress, g_cancel);
  for (const auto& n : r.notes) std::printf("%s\n", n.c_str());
  if (!r.ok) return Fail(r.error);
  std::printf("done: start with %s\n", (plan.root / "kuf-cod").c_str());
  return 0;
}

}  // namespace kuf::setup
