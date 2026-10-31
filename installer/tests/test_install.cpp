// Installing, repairing and uninstalling into disposable folders (installer/src/core/install.cpp): a small fake
// package and fake game files, a temporary home (menu, desktop, Steam, saves all inside it).

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/install.h"
#include "sandbox.h"

using namespace kuf::setup;
using kuf::setup::test::Sandbox;

namespace {

bool NotRunning() { return false; }

struct Fixture {
  Sandbox s;
  Plan plan;
  Fixture(const std::string& install_name = "Kingdom Under Fire - Circle of Doom") {
    steam::SetRunningCheckForTests(NotRunning);
    // The package: as package_linux.sh makes it (VERSION, MANIFEST.sha256 of every other file).
    const fs::path pkg = s.dir / "pkg";
    Sandbox::Write(pkg / "bin/kuf_cod", "#!/bin/sh\necho game\n");
    Sandbox::Write(pkg / "kuf-cod.sh", "#!/bin/sh\n");
    Sandbox::Write(pkg / "kuf_update.py", "print()\n");
    Sandbox::Write(pkg / "VERSION", "0.7.0\n");
    fs::permissions(pkg / "bin/kuf_cod", fs::perms(0755));
    std::string manifest;
    for (const char* f : {"VERSION", "bin/kuf_cod", "kuf-cod.sh", "kuf_update.py"}) {
      manifest += Sha256File(pkg / f) + "  " + f + "\n";
    }
    Sandbox::Write(pkg / "MANIFEST.sha256", manifest);
    // The player's game files (read only).
    const fs::path game = s.dir / "my game";
    Sandbox::Write(game / "default.xex", "XEX-V1");
    Sandbox::Write(game / "Data/Sound/a.xwb", std::string(3 << 20, 's'));
    Sandbox::Write(game / "$SystemUpdate/su.bin", "not needed");
    fs::permissions(game / "default.xex", fs::perms(0444));
    plan.manifest = ParseGameManifest(Sha256File(game / "default.xex") + "\t6\tdefault.xex\n" +
                                          Sha256File(game / "Data/Sound/a.xwb") + "\t" + std::to_string(3 << 20) +
                                          "\tData/Sound/a.xwb\n",
                                      "test");
    plan.root = s.dir / "Games" / install_name;
    plan.package = pkg;
    plan.game_source = game;
    Sandbox::Write(s.dir / "icon.png", "PNG");
    plan.icon = s.dir / "icon.png";
    Sandbox::Write(s.dir / "setup/kuf-setup", "#!/bin/sh\n");
    Sandbox::Write(s.dir / "setup/assets/theme.css", "*{}");
    plan.setup_binary = s.dir / "setup/kuf-setup";
    plan.setup_assets = s.dir / "setup/assets";
    plan.menu = true;
  }
  ~Fixture() { steam::SetRunningCheckForTests(nullptr); }
};

std::atomic<bool> g_no_cancel{false};

}  // namespace

TEST_CASE("a new installation: the updater's layout, the game data, the menu entry", "[install]") {
  Fixture f;
  std::vector<Stage> stages;
  const Result r = Install(f.plan, [&](Stage s, double, const std::string&) {
    if (stages.empty() || stages.back() != s) stages.push_back(s);
  }, g_no_cancel);
  REQUIRE(r.ok);
  const fs::path root = f.plan.root;
  CHECK(fs::read_symlink(root / "current") == "versions/0.7.0");
  // A normal folder (the staging folder it was built in is private while it is built).
  CHECK((fs::status(root).permissions() & fs::perms(0755)) == fs::perms(0755));
  CHECK(fs::exists(root / "versions/0.7.0/bin/kuf_cod"));
  CHECK((fs::status(root / "versions/0.7.0/bin/kuf_cod").permissions() & fs::perms::owner_exec) != fs::perms::none);
  CHECK(fs::exists(root / "game/default.xex"));
  CHECK_FALSE(fs::exists(root / "game/$SystemUpdate"));
  CHECK(fs::exists(root / "kuf-cod"));
  CHECK(fs::exists(root / "kuf-update"));
  CHECK(fs::exists(root / "update.conf"));
  CHECK(fs::exists(root / "share/setup/kuf-setup"));
  CHECK(fs::exists(root / "share/setup/assets/theme.css"));
  const fs::path entry = f.s.Home() / ".local/share/applications/kuf-cod-recomp.desktop";
  REQUIRE(fs::exists(entry));
  const auto record = ReadRecord(root);
  REQUIRE(record);
  CHECK(record->version == "0.7.0");
  CHECK(record->shortcuts.size() == 2);  // the game and its uninstall entry
  CHECK(stages == std::vector<Stage>{kValidate, kPrepare, kRuntime, kGameData, kShortcuts, kFinalize});
  // No staging folder is left next to it.
  for (const auto& e : fs::directory_iterator(root.parent_path())) {
    CHECK(e.path().filename().string().find(".kuf-staging-") == std::string::npos);
  }
  // The player's folder is unchanged.
  CHECK(fs::exists(f.plan.game_source / "$SystemUpdate/su.bin"));
}

TEST_CASE("desktop shortcut and Steam shortcut, recorded and removed again", "[install]") {
  Fixture f;
  fs::create_directories(f.s.Home() / "Desktop");
  const fs::path steam_root = f.s.Home() / ".local/share/Steam";
  fs::create_directories(steam_root / "userdata/1001/config");
  f.plan.desktop = true;
  f.plan.steam_profile = steam::FindProfiles(steam::FindInstalls(f.s.Home()).at(0)).at(0);
  const Result r = Install(f.plan, nullptr, g_no_cancel);
  REQUIRE(r.ok);
  CHECK(fs::exists(f.s.Home() / "Desktop/kuf-cod-recomp.desktop"));
  CHECK(steam::HasShortcut(*f.plan.steam_profile, f.plan.root / "kuf-cod"));
  // Saves exist (the game made them): kept by the uninstaller by default.
  Sandbox::Write(f.s.Home() / ".local/share/kuf-cod-recomp/data/saves/save1", "save");
  Sandbox::Write(f.plan.root / "my notes.txt", "mine");  // a file the player added
  const Result u = Uninstall(f.plan.root, RemoveOptions{}, nullptr);
  REQUIRE(u.ok);
  CHECK_FALSE(fs::exists(f.s.Home() / "Desktop/kuf-cod-recomp.desktop"));
  CHECK_FALSE(fs::exists(f.s.Home() / ".local/share/applications/kuf-cod-recomp.desktop"));
  CHECK_FALSE(steam::HasShortcut(*f.plan.steam_profile, f.plan.root / "kuf-cod"));
  CHECK(fs::exists(f.s.Home() / ".local/share/kuf-cod-recomp/data/saves/save1"));
  CHECK(fs::exists(f.plan.root / "my notes.txt"));  // kept, so the folder too
  CHECK_FALSE(fs::exists(f.plan.root / "game"));
  CHECK_FALSE(fs::exists(f.plan.root / "versions"));
  CHECK_FALSE(fs::exists(f.plan.root / kRecordFile));
}

TEST_CASE("uninstall removes the folder when nothing else is in it; saves only when asked", "[install]") {
  Fixture f;
  REQUIRE(Install(f.plan, nullptr, g_no_cancel).ok);
  Sandbox::Write(f.s.Home() / ".local/share/kuf-cod-recomp/data/saves/save1", "save");
  RemoveOptions all;
  all.user_data = true;
  REQUIRE(Uninstall(f.plan.root, all, nullptr).ok);
  CHECK_FALSE(fs::exists(f.plan.root));
  CHECK(fs::exists(f.plan.root.parent_path()));  // only the installation folder itself
  CHECK_FALSE(fs::exists(f.s.Home() / ".local/share/kuf-cod-recomp/data"));
  // A folder without the setup's record is never removed.
  fs::create_directories(f.s.dir / "other");
  Sandbox::Write(f.s.dir / "other/file", "x");
  CHECK_FALSE(Uninstall(f.s.dir / "other", RemoveOptions{}, nullptr).ok);
  CHECK(fs::exists(f.s.dir / "other/file"));
}

TEST_CASE("failures change nothing: a damaged game file, a non-empty folder, cancelling", "[install]") {
  Fixture f;
  // A game file with the right size but other contents (another version or damaged).
  Sandbox::Write(f.plan.game_source / "Data/Sound/a.xwb", std::string(3 << 20, 't'));
  Result r = Install(f.plan, nullptr, g_no_cancel);
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("does not match") != std::string::npos);
  CHECK_FALSE(fs::exists(f.plan.root));
  for (const auto& e : fs::directory_iterator(f.plan.root.parent_path())) {
    CHECK(e.path().filename().string().find(".kuf-staging-") == std::string::npos);
  }
  // A folder with other files in it is refused.
  Fixture g;
  Sandbox::Write(g.plan.root / "someone else's.txt", "x");
  CHECK(CheckTarget(g.plan.root) == Target::kNotEmpty);
  CHECK_FALSE(Install(g.plan, nullptr, g_no_cancel).ok);
  CHECK(fs::exists(g.plan.root / "someone else's.txt"));
  // Cancelled during the copy: nothing left.
  Fixture h;
  std::atomic<bool> cancel{false};
  r = Install(h.plan, [&](Stage s, double, const std::string&) {
    if (s == kGameData) cancel = true;
  }, cancel);
  CHECK_FALSE(r.ok);
  CHECK_FALSE(fs::exists(h.plan.root));
  // The home folder and / are never targets.
  CHECK(CheckTarget(h.s.Home()) == Target::kInvalid);
  CHECK(CheckTarget("/") == Target::kInvalid);
  CHECK(CheckTarget("relative/path") == Target::kInvalid);
}

TEST_CASE("repair and reinstall: an existing installation is detected and fixed in place", "[install]") {
  Fixture f;
  REQUIRE(Install(f.plan, nullptr, g_no_cancel).ok);
  CHECK(CheckTarget(f.plan.root) == Target::kExisting);
  // Damage: a game file changed, the program deleted; the player's update.conf edited (kept).
  Sandbox::Write(f.plan.root / "game/Data/Sound/a.xwb", "broken");
  fs::remove(f.plan.root / "versions/0.7.0/bin/kuf_cod");
  Sandbox::Write(f.plan.root / "update.conf", "repo = me/fork\n");
  Sandbox::Write(f.s.Home() / ".local/share/kuf-cod-recomp/data/saves/save1", "save");
  const Result r = Install(f.plan, nullptr, g_no_cancel);
  REQUIRE(r.ok);
  CHECK(Sha256File(f.plan.root / "game/Data/Sound/a.xwb") == Sha256File(f.plan.game_source / "Data/Sound/a.xwb"));
  CHECK(fs::exists(f.plan.root / "versions/0.7.0/bin/kuf_cod"));
  std::string conf;
  REQUIRE(ReadFile(f.plan.root / "update.conf", &conf));
  CHECK(conf == "repo = me/fork\n");
  CHECK(fs::exists(f.s.Home() / ".local/share/kuf-cod-recomp/data/saves/save1"));
  // The menu entry is not duplicated.
  CHECK(ReadRecord(f.plan.root)->shortcuts.size() == 2);
}

TEST_CASE("an installation path with spaces, quotes and $ works end to end", "[install]") {
  Fixture f("KUF \"Circle\" of $Doom's");
  REQUIRE(Install(f.plan, nullptr, g_no_cancel).ok);
  std::string script;
  REQUIRE(ReadFile(f.plan.root / "kuf-cod", &script));
  // bash reads the launcher's exported path back exactly.
  const std::string cmd = "bash -c " + ShellQuote("source <(sed -n '/^export/p' " +
                                                  ShellQuote((f.plan.root / "kuf-cod").string()) +
                                                  ") && printf %s \"$KUF_GAME_DIR\"");
  std::string out;
  if (FILE* p = popen(cmd.c_str(), "r")) {
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
  }
  CHECK(out == (f.plan.root / "game").string());
}
