// The setup without its window (installer/README.md "Unattended"): the same checks as the pages (game folder,
// target folder, system, space) and the same core Install / Uninstall, driven by options. For scripted installs and
// the release tests (tools/package/appimage_test.sh); a player uses the window.
//
//   kuf-setup --install-to FOLDER --game-dir FOLDER [--no-menu] [--desktop]   install, or repair an installation
//   kuf-setup --remove FOLDER [--keep-shortcuts] [--remove-user-data]         uninstall
// Exit: 0 done, 1 failed or refused (the reason on stderr), 2 usage.
#include "gui/unattended.h"

#include <atomic>
#include <cstdio>
#include <string>

#include "core/game_data.h"
#include "core/install.h"
#include "core/util.h"

namespace kuf::setup {
namespace {

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
    if (a == "--install-to" || a == "--remove") return true;
  }
  return false;
}

int RunUnattended(int argc, char** argv, const fs::path& exe) {
  fs::path install_to, remove, game, assets_arg, payload_arg;
  bool menu = true, desktop = false, keep_shortcuts = false, remove_data = false;
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
  if (ReadFile(payload.parent_path() / "requires.txt", &requires_text)) {
    const auto problems = CompatibilityProblems(ParseRequirements(requires_text), HostGlibc(), HostGlibcxx());
    for (const auto& p : problems) std::fprintf(stderr, "kuf-setup: %s\n", p.c_str());
    if (!problems.empty()) return Fail("this system is too old for the game");
  }

  if (game.empty()) return Fail("--game-dir: the folder with your game files (default.xex) is needed");
  plan.game_source = FindGameRoot(game);
  const GameCheck c = CheckGameFolder(plan.game_source, plan.manifest);
  if (!c.folder_ok) return Fail("the game folder cannot be read: " + plan.game_source.string());
  if (!c.executable_found) return Fail("no default.xex in " + plan.game_source.string());
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
  std::atomic<bool> cancel{false};
  const Result r = Install(plan, Progress, cancel);
  for (const auto& n : r.notes) std::printf("%s\n", n.c_str());
  if (!r.ok) return Fail(r.error);
  std::printf("done: start with %s\n", (plan.root / "kuf-cod").c_str());
  return 0;
}

}  // namespace kuf::setup
