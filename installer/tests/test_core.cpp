// The setup's helpers, desktop entries and game-data checks (installer/src/core).

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <unistd.h>

#include "core/localbuild.h"
#include "core/game_data.h"
#include "core/install.h"
#include "core/system_check.h"
#include "core/util.h"
#include "sandbox.h"

using namespace kuf::setup;
using kuf::setup::test::Sandbox;

namespace {
// desktop-file-validate's verdict on an entry ("" when valid or the tool is missing).
std::string Validate(const fs::path& file) {
  if (std::system("command -v desktop-file-validate >/dev/null 2>&1") != 0) return {};
  std::string cmd = "desktop-file-validate " + ShellQuote(file.string()) + " 2>&1";
  std::string out;
  if (FILE* p = popen(cmd.c_str(), "r")) {
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
  }
  return out;
}
}  // namespace

TEST_CASE("shell and desktop-entry quoting", "[setup]") {
  CHECK(ShellQuote("/home/a b/it's") == "'/home/a b/it'\\''s'");
  CHECK(DesktopExecArg("/opt/game/kuf-cod") == "/opt/game/kuf-cod");
  // Spaces quote; ", `, $ and \ are escaped inside the quotes, then \ is doubled for the string level; % doubled.
  CHECK(DesktopExecArg("/home/a b/Games") == "\"/home/a b/Games\"");
  CHECK(DesktopExecArg("/x/$HOME \"q\" 100%") == "\"/x/\\\\$HOME \\\\\"q\\\\\" 100%%\"");
  CHECK(DesktopString("a\\b\nc") == "a\\\\b\\nc");
}

TEST_CASE("desktop entries are valid, with spaces and quotes in the path", "[setup]") {
  Sandbox s;
  const fs::path root = s.dir / "Games" / "Kingdom Under Fire - Circle of Doom \"test\" $x";
  for (const auto& [name, text] : {std::pair{"game.desktop", GameDesktopEntry(root)},
                                   std::pair{"uninstall.desktop", UninstallDesktopEntry(root)}}) {
    const fs::path f = s.dir / name;
    Sandbox::Write(f, text);
    CHECK(Validate(f) == "");
  }
  CHECK(GameDesktopEntry(root).find("Categories=Game;") != std::string::npos);
  // The launcher script quotes the paths for the shell.
  CHECK(LauncherScript(root).find("export KUF_GAME_DIR='") != std::string::npos);
  CHECK(LauncherScript("/a/it's").find("'/a/it'\\''s/game'") != std::string::npos);
}

TEST_CASE("the desktop folder follows user-dirs.dirs and is never created", "[setup]") {
  Sandbox s;
  CHECK_FALSE(DesktopDir());  // no ~/Desktop
  fs::create_directories(s.Home() / "Bureau");
  Sandbox::Write(s.Home() / ".config/user-dirs.dirs", "# x\nXDG_DESKTOP_DIR=\"$HOME/Bureau\"\n");
  REQUIRE(DesktopDir());
  CHECK(*DesktopDir() == s.Home() / "Bureau");
  Sandbox::Write(s.Home() / ".config/user-dirs.dirs", "XDG_DESKTOP_DIR=\"$HOME/Missing\"\n");
  CHECK_FALSE(DesktopDir());
  // The home folder as the desktop: no desktop (nothing is put into the home folder).
  Sandbox::Write(s.Home() / ".config/user-dirs.dirs", "XDG_DESKTOP_DIR=\"$HOME/\"\n");
  CHECK_FALSE(DesktopDir());
}

TEST_CASE("the reference list parses; damaged lists are refused", "[setup]") {
  const std::string sha(64, 'a');
  const auto m = ParseGameManifest("# c\n" + sha + "\t3\tdefault.xex\n" + sha + "\t5\tData/x.pak\n", "v1");
  REQUIRE(m.files.size() == 2);
  CHECK(m.total == 8);
  CHECK(ParseGameManifest(sha + "\t3\t../etc/passwd\n" + sha + "\t3\tdefault.xex\n", "v1").files.empty());
  CHECK(ParseGameManifest(sha + "\tx\tdefault.xex\n", "v1").files.empty());
  CHECK(ParseGameManifest(sha + "\t3\tother.bin\n", "v1").files.empty());  // no executable
  // The real list shipped with the setup: 137 files, default.xex v1.
  std::string text;
  REQUIRE(ReadFile(fs::path(KUF_SETUP_ASSETS) / "game_v1.tsv", &text));
  const auto real = ParseGameManifest(text, "v1");
  CHECK(real.files.size() == 137);
  REQUIRE(real.Find("default.xex"));
  CHECK(real.Find("default.xex")->sha256 == "c502f6617bf1ef1112474d6eb1de5bb6752625f0b7688691c1f8ce0c50c90627");
}

TEST_CASE("a game folder is checked: missing files, wrong sizes, the executable's version", "[setup]") {
  Sandbox s;
  const fs::path g = s.dir / "game";
  Sandbox::Write(g / "default.xex", Sandbox::Xex("IMAGE"));
  Sandbox::Write(g / "Data/x.pak", "12345");
  const auto m = ParseGameManifest(InspectXex(g / "default.xex").image_sha256 + "\t5\tdefault.xex\n" + std::string(64, 'b') +
                                       "\t5\tData/x.pak\n" + std::string(64, 'c') + "\t1\tData/y.pak\n",
                                   "test");
  REQUIRE(m.files.size() == 3);
  auto c = CheckGameFolder(g, m);
  CHECK(c.folder_ok);
  CHECK(c.executable_known);
  CHECK(c.missing == std::vector<std::string>{"Data/y.pak"});
  CHECK_FALSE(c.Ok());
  Sandbox::Write(g / "Data/y.pak", "toolong");
  c = CheckGameFolder(g, m);
  CHECK(c.wrong_size == std::vector<std::string>{"Data/y.pak"});
  Sandbox::Write(g / "Data/y.pak", "1");
  CHECK(CheckGameFolder(g, m).Ok());
  Sandbox::Write(g / "default.xex", Sandbox::Xex("OTHER"));  // another executable (version)
  CHECK_FALSE(CheckGameFolder(g, m).executable_known);
  Sandbox::Write(g / "default.xex", Sandbox::Xex("IMAGE", 1));  // encrypted: refused, and said so
  CHECK(CheckGameFolder(g, m).executable_encrypted);
  CHECK_FALSE(CheckGameFolder(g, m).Ok());
  // A folder one level above the game is accepted (its only subfolder with default.xex).
  CHECK(FindGameRoot(s.dir) == g);
  CHECK_FALSE(CheckGameFolder(s.dir / "nothing", m).folder_ok);
}

TEST_CASE("verified copies: hash, cancel, nothing left behind", "[setup]") {
  Sandbox s;
  Sandbox::Write(s.dir / "a", std::string(10 << 20, 'z'));
  uint64_t seen = 0;
  const auto sha = CopyHashed(s.dir / "a", s.dir / "b", [&](uint64_t n) { seen += n; }, nullptr);
  CHECK(sha == Sha256File(s.dir / "a"));
  CHECK(seen == (10u << 20));
  CHECK(CopyHashed(s.dir / "a", s.dir / "c", nullptr, [] { return true; }).empty());
  CHECK_FALSE(fs::exists(s.dir / "c"));
  CHECK_FALSE(fs::exists(s.dir / "c.part"));
}

TEST_CASE("the installation record round-trips", "[setup]") {
  Record r;
  r.version = "0.7.0";
  r.installed = "2026-10-31T00:00:00Z";
  r.shortcuts = {"/h/.local/share/applications/kuf-cod-recomp.desktop"};
  r.steam = {{"/h/.local/share/Steam", "12345"}};
  const auto back = ParseRecord(FormatRecord(r));
  REQUIRE(back);
  CHECK(back->version == "0.7.0");
  CHECK(back->shortcuts == r.shortcuts);
  CHECK(back->steam == r.steam);
  CHECK_FALSE(ParseRecord("version=1\n"));  // no format line: not the setup's
}

TEST_CASE("system requirements: versions compared numerically, this system read", "[setup]") {
  CHECK(CompareVersions("2.39", "2.44") < 0);
  CHECK(CompareVersions("2.44", "2.44") == 0);
  CHECK(CompareVersions("3.4.36", "3.4.9") > 0);  // numeric, not text
  const Requirements need = ParseRequirements("glibc 2.44\nglibcxx 3.4.36\n");
  CHECK(need.glibc == "2.44");
  CHECK(need.glibcxx == "3.4.36");
  CHECK(CompatibilityProblems(need, "2.44", "3.4.36").empty());
  const auto old = CompatibilityProblems(need, "2.39", "3.4.33");  // Ubuntu 24.04
  REQUIRE(old.size() == 2);
  CHECK(old[0].find("2.39") != std::string::npos);
  CHECK(CompatibilityProblems(need, "2.44", "").size() == 1);  // unknown: refused, not guessed
  // The portable package (M11): glibc 2.31, and its own libstdc++ for systems with an older one (SteamOS 3.5: 3.4.32).
  const Requirements portable = ParseRequirements("glibc 2.31\nglibcxx 3.4.33\nglibcxx-bundled 3.4.33\n");
  CHECK(portable.glibcxx_bundled == "3.4.33");
  CHECK(CompatibilityProblems(portable, "2.37", "3.4.32").empty());
  CHECK(CompatibilityProblems(portable, "2.31", "").empty());
  CHECK(CompatibilityProblems(portable, "2.28", "3.4.33").size() == 1);  // the C library is never bundled
  // A bundled copy older than what the binaries need does not count.
  CHECK(CompatibilityProblems(ParseRequirements("glibc 2.31\nglibcxx 3.4.34\nglibcxx-bundled 3.4.33\n"), "2.41",
                              "3.4.33").size() == 1);
  // This system (the build machine) is read: a glibc version, and a libstdc++ version (this test is C++).
  CHECK(CompareVersions(HostGlibc(), "2.17") > 0);
  CHECK(CompareVersions(HostGlibcxx(), "3.4.20") > 0);
}

TEST_CASE("system check (M11): SteamOS and the Steam Deck by os-release and the DMI board", "[setup]") {
  const fs::path root = fs::temp_directory_path() / ("kuf-setup-platform-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "etc");
  fs::create_directories(root / "sys/class/dmi/id");
  auto write = [&](const char* rel, const char* text) { std::ofstream(root / rel) << text; };
  write("etc/os-release", "NAME=\"SteamOS\"\nPRETTY_NAME=\"SteamOS\"\nID=steamos\n");
  write("sys/class/dmi/id/board_vendor", "Valve\n");
  write("sys/class/dmi/id/board_name", "Galileo\n");
  Platform p = DetectPlatform(root);
  CHECK(p.steamos);
  CHECK(p.steam_deck);
  CHECK(p.deck_model == "OLED");
  CHECK(PlatformSummary(p) == "SteamOS on a Steam Deck (OLED)");
  write("sys/class/dmi/id/board_vendor", "Micro-Star International Co., Ltd.\n");  // an AMD PC is not a Deck
  p = DetectPlatform(root);
  CHECK_FALSE(p.steam_deck);
  fs::remove_all(root);
}

TEST_CASE("system check (M11): Vulkan problems name what is missing; software renderers do not count", "[setup]") {
  VulkanReport r;
  r.surface = true;
  VulkanDevice deck{"AMD Custom GPU 0405 (RADV VANGOGH)", "integrated GPU", ((1u << 22) | (3u << 12) | 289u), 0x1002, {}};
  r.devices = {deck};
  CHECK(r.Usable());
  CHECK(VulkanProblems(r).empty());
  r.devices[0].missing = {"geometryShader", "fillModeNonSolid"};
  CHECK_FALSE(r.Usable());
  const auto lacks = VulkanProblems(r);
  REQUIRE(lacks.size() == 1);
  CHECK(lacks[0].find("geometryShader, fillModeNonSolid") != std::string::npos);
  CHECK(lacks[0].find("Vulkan 1.3.289") != std::string::npos);
  VulkanReport cpu;
  cpu.surface = true;
  cpu.devices = {{"llvmpipe (LLVM 15.0.7, 256 bits)", "CPU (software)", ((1u << 22) | (3u << 12)), 0x10005, {}}};
  CHECK_FALSE(cpu.Usable());
  CHECK(VulkanProblems(cpu)[0].find("software renderer") != std::string::npos);
  VulkanReport no_surface = r;
  no_surface.devices[0].missing.clear();
  no_surface.surface = false;
  CHECK_FALSE(no_surface.Usable());
  // The check runs in a child process; its answer arrives intact (here: the test switch for "no driver").
  setenv("KUF_SETUP_PRETEND_NO_VULKAN", "1", 1);
  const VulkanReport none = CheckVulkan();
  unsetenv("KUF_SETUP_PRETEND_NO_VULKAN");
  CHECK(none.error.find("no Vulkan driver") != std::string::npos);
  CHECK_FALSE(none.Usable());
}

TEST_CASE("default.xex is read without keys: its format, its memory image", "[setup]") {
  Sandbox s;
  const fs::path x = s.dir / "x.xex";
  Sandbox::Write(x, Sandbox::Xex(std::string(100, 'a')));
  const XexInfo plain = InspectXex(x);
  REQUIRE(plain.status == XexStatus::kOk);
  CHECK(plain.image_sha256.size() == 64);
  // Basic compression: the same image gives the same hash, whichever way the file holds it.
  Sandbox::Write(x, Sandbox::Xex(std::string(84, 'a') + std::string(16, '\0')));
  const std::string flat = InspectXex(x).image_sha256;
  Sandbox::Write(x, Sandbox::Xex(std::string(84, 'a'), 0, 1));
  CHECK(InspectXex(x).status == XexStatus::kOk);
  CHECK(InspectXex(x).image_sha256 == flat);
  Sandbox::Write(x, Sandbox::Xex("x", 1));
  CHECK(InspectXex(x).status == XexStatus::kEncrypted);
  CHECK(InspectXex(x).image_sha256.empty());
  Sandbox::Write(x, Sandbox::Xex("x", 0, 2));
  CHECK(InspectXex(x).status == XexStatus::kUnsupportedCompression);
  Sandbox::Write(x, "not an executable at all, long enough to hold a header");
  CHECK(InspectXex(x).status == XexStatus::kNotXex);
  Sandbox::Write(x, "XEX2");
  CHECK(InspectXex(x).status == XexStatus::kNotXex);
  CHECK(InspectXex(s.dir / "missing").status == XexStatus::kUnreadable);
}

TEST_CASE("build jobs follow the processors and the free memory", "[setup]") {
  CHECK(DefaultBuildJobs(8, uint64_t(32) << 30) == 8);
  CHECK(DefaultBuildJobs(8, uint64_t(4) << 30) == 4);  // 1 GiB each
  CHECK(DefaultBuildJobs(8, uint64_t(1) << 29) == 1);  // never none
  CHECK(DefaultBuildJobs(0, uint64_t(32) << 30) == 1);
}
