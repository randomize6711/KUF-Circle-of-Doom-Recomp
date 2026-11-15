// The portable edition's logic (core/portable.h): finding the game files, the first and later launches, moving the
// folder, a damaged or interrupted preparation, disc images, saves, a read-only program folder, Verify Game Files.
// The local build is the fake one of the install tests (a code generator and a compiler that write small files).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

#include "core/platform.h"
#include "core/portable.h"
#include "core/sha256.h"
#include "core/xex_decrypt.h"
#include "sandbox.h"
#include "xex_builder.h"
#include "xiso_builder.h"

using namespace kuf::setup;
using kuf::setup::test::Sandbox;
using kuf::setup::test::Xiso;
using kuf::setup::test::EncryptableXex;
using kuf::setup::test::KeyFromHex;
using kuf::setup::test::kImage80;
using kuf::setup::test::TestKey;

namespace {

constexpr uint32_t kTitle = 0x424507D1, kMedia = 0x7092B5EC, kVersion = 0x00000001;
std::atomic<bool> g_no_cancel{false};

struct Portable {
  Sandbox s;
  fs::path root;
  PortableLayout layout;
  GameManifest manifest;
  std::string plain, retail;
  explicit Portable(const std::string& name = "KUF-Circle-of-Doom-Portable") {
    root = s.dir / name;
    layout = DefaultPortableLayout(root);
    const fs::path rt = root / "Runtime";
    Sandbox::Write(rt / "bin/rexgluerd", "#!/bin/sh\nmkdir -p generated/default\necho > generated/default/kuf_cod_pch.h\n"
                                         "echo 'int a;' > generated/default/a.cpp\n");
    Sandbox::Write(rt / "bin/librexruntimerd.so", "runtime library");
    Sandbox::Write(rt / "builder/toolchain/llvm",
                   "#!/bin/sh\n[ -n \"$KUF_TEST_BUILD_FAILS\" ] && { echo 'compiler stops here'; exit 1; }\n"
                   "while [ $# -gt 0 ]; do [ \"$1\" = -o ] && out=\"$2\"; shift; done\n"
                   "printf '#!/bin/sh\\necho game\\n' > \"$out\"\n");
    Sandbox::Write(rt / "builder/recipe.txt", "generation_id " + std::string(64, 'a') +
                                                  "\nsources 1\ncompile -c\nlink -pthread\nlink_libraries -lm\n");
    Sandbox::Write(rt / "builder/kuf_cod_manifest.toml", "[project]\n");
    Sandbox::Write(rt / "builder/config/v1/codegen.toml", "\n");
    Sandbox::Write(rt / "VERSION", "0.9.0-test\n");
    fs::permissions(rt / "bin/rexgluerd", fs::perms(0755));
    fs::permissions(rt / "builder/toolchain/llvm", fs::perms(0755));
    plain = Sandbox::XexIdentified("IMAGE", 0, kTitle, kMedia, kVersion);
    retail = Sandbox::XexIdentified("SCRAMBLED", 1, kTitle, kMedia, kVersion);
    manifest = ParseGameManifest("@title 424507D1 7092B5EC 00000001\n@retail-xex " + InspectXexData(retail).file_sha256 +
                                     " 1\n" + InspectXexData(plain).image_sha256 + "\t1\tdefault.xex\n" +
                                     Sha256Hex(std::string(3000, 's')) + "\t3000\tData/Sound/a.xwb\n",
                                 "test");
    REQUIRE(!manifest.files.empty());
    fs::create_directories(root / "GameFiles");
  }
  void PlainFolder(const std::string& name = "my game") {
    Sandbox::Write(root / "GameFiles" / name / "default.xex", plain);
    Sandbox::Write(root / "GameFiles" / name / "Data/Sound/a.xwb", std::string(3000, 's'));
  }
  bool Prepare(const PortableSource& src, std::string* error) {
    return PreparePortable(layout, src, manifest, nullptr, g_no_cancel, error, 1);
  }
};

}  // namespace

TEST_CASE("portable: no game files, one folder, several sources best first, a retail dump paired", "[portable]") {
  Portable p;
  CHECK(FindPortableSources(p.root / "GameFiles", p.manifest).empty());
  Sandbox::Write(p.root / "GameFiles/Put your game files here.txt", "...");
  Sandbox::Write(p.root / "GameFiles/.hidden/default.xex", p.plain);
  CHECK(FindPortableSources(p.root / "GameFiles", p.manifest).empty());
  // A retail dump (encrypted executable, $SystemUpdate) and, beside it, an unencrypted default.xex: paired.
  Sandbox::Write(p.root / "GameFiles/retail dump/Default.xex", p.retail);
  Sandbox::Write(p.root / "GameFiles/retail dump/Data/Sound/a.xwb", std::string(3000, 's'));
  Sandbox::Write(p.root / "GameFiles/retail dump/$SystemUpdate/su.bin", "x");
  Sandbox::Write(p.root / "GameFiles/unencrypted/default.xex", p.plain);
  // An incomplete folder.
  Sandbox::Write(p.root / "GameFiles/broken/default.xex", p.plain);
  auto found = FindPortableSources(p.root / "GameFiles", p.manifest);
  REQUIRE(found.size() == 3);  // retail dump, unencrypted (incomplete: only the executable), broken
  CHECK(found[0].check.Ok());
  CHECK(found[0].path.filename() == "retail dump");
  CHECK(found[0].executable.filename() == "default.xex");
  CHECK(found[0].label.find("with the unencrypted") != std::string::npos);
  CHECK_FALSE(found[1].check.Ok());
  CHECK_FALSE(found[2].check.Ok());
}

TEST_CASE("portable: first launch prepares, later launches do not; saves are kept; the folder can move", "[portable]") {
  Portable p("Portable KUF");
  p.PlainFolder();
  Sandbox::Write(p.root / "Data/saves/existing-save.bin", "my progress");
  auto found = FindPortableSources(p.root / "GameFiles", p.manifest);
  REQUIRE(found.size() == 1);
  REQUIRE(found[0].check.Ok());
  PortableState state;
  std::string why, error;
  CHECK_FALSE(PortableReady(p.layout, found[0], state, &why));
  CHECK(why == "The game has not been prepared yet.");
  REQUIRE(p.Prepare(found[0], &error));
  REQUIRE(ReadPortableState(p.layout.StateFile(), &state));
  CHECK(state.source == "GameFiles/my game");
  CHECK(state.game_dir == "GameFiles/my game");  // a plain folder is read where it is
  CHECK(state.package_version == "0.9.0-test");
  CHECK(PortableReady(p.layout, found[0], state, &why));
  CHECK(fs::exists(p.root / "Runtime/bin/kuf_cod"));
  std::string save;
  CHECK(ReadFile(p.root / "Data/saves/existing-save.bin", &save));
  CHECK(save == "my progress");
  for (const char* d : {"Data/user", "Data/saves", "Data/cache", "Data/logs"}) CHECK(fs::is_directory(p.root / d));
  // The launch: every path inside the folder.
  LaunchCommand c = PortableLaunchCommand(p.layout, state, {"--fullscreen"});
  CHECK(c.argv[0] == PathUtf8(p.root / "Runtime/bin/kuf_cod"));
  CHECK(c.argv.back() == "--fullscreen");
  for (const std::string& a : c.argv) {
    if (a.rfind("--", 0) == 0 && a.find('=') != std::string::npos && a.find('/') != std::string::npos) {
      CHECK(a.find(PathUtf8(p.root)) != std::string::npos);
    }
  }
  // Moved to a folder with spaces and non-ASCII characters: still ready, the paths follow.
  const fs::path moved = p.s.dir / PathFromUtf8("Spiele \xC3\xA4\xC3\xB6 \xE7\xA5\x9E/KUF Portable");
  fs::create_directories(moved.parent_path());
  fs::rename(p.root, moved);
  const PortableLayout ml = DefaultPortableLayout(moved);
  found = FindPortableSources(moved / "GameFiles", p.manifest);
  REQUIRE(found.size() == 1);
  PortableState ms;
  REQUIRE(ReadPortableState(ml.StateFile(), &ms));
  CHECK(PortableReady(ml, found[0], ms, &why));
  c = PortableLaunchCommand(ml, ms);
  CHECK(c.argv[0] == PathUtf8(moved / "Runtime/bin/kuf_cod"));
  CHECK(c.argv[2] == "--game_data_root=" + PathUtf8(moved / "GameFiles/my game"));
}

TEST_CASE("portable: a damaged program, a new package version, a failed preparation", "[portable]") {
  Portable p;
  p.PlainFolder();
  auto found = FindPortableSources(p.root / "GameFiles", p.manifest);
  std::string error, why;
  REQUIRE(p.Prepare(found[0], &error));
  PortableState state;
  REQUIRE(ReadPortableState(p.layout.StateFile(), &state));
  const std::string good_program = state.program_sha256;
  // The program damaged (a corrupted preparation): not ready, and preparing again repairs it.
  Sandbox::Write(p.root / "Runtime/bin/kuf_cod", "damaged");
  CHECK_FALSE(PortableReady(p.layout, found[0], state, &why));
  CHECK(why == "The prepared program is missing or damaged.");
  REQUIRE(p.Prepare(found[0], &error));
  REQUIRE(ReadPortableState(p.layout.StateFile(), &state));
  CHECK(PortableReady(p.layout, found[0], state, &why));
  // A new package (another generation id): prepared again before it is played.
  Sandbox::Write(p.root / "Runtime/builder/recipe.txt", "generation_id " + std::string(64, 'b') +
                                                            "\nsources 1\ncompile -c\nlink -pthread\nlink_libraries -lm\n");
  CHECK_FALSE(PortableReady(p.layout, found[0], state, &why));
  CHECK(why == "This version of the program needs a new preparation.");
  // A failed build: an error, the old program and record stay.
  p.s.SetEnv("KUF_TEST_BUILD_FAILS", "1");
  CHECK_FALSE(p.Prepare(found[0], &error));
  CHECK(error.find("compiler stops here") != std::string::npos);
  PortableState after;
  REQUIRE(ReadPortableState(p.layout.StateFile(), &after));
  CHECK(after.generation == std::string(64, 'a'));
  CHECK(Sha256File(p.root / "Runtime/bin/kuf_cod") == after.program_sha256);
  CHECK(after.program_sha256 == good_program);
  CHECK_FALSE(fs::exists(p.root / "Runtime/bin/kuf_cod.part"));
}

TEST_CASE("portable: a disc image in GameFiles is unpacked into Data/game once, checked; the image is only read",
          "[portable]") {
  Portable p;
  Xiso x;
  x.files = {{"default.xex", p.plain}, {"Data/Sound/a.xwb", std::string(3000, 's')}, {"$SystemUpdate/su.bin", "x"}};
  x.Build(p.root / "GameFiles/My Game.ISO");
  const std::string before = Sha256File(p.root / "GameFiles/My Game.ISO");
  auto found = FindPortableSources(p.root / "GameFiles", p.manifest);
  REQUIRE(found.size() == 1);
  REQUIRE(found[0].check.Ok());
  CHECK(found[0].label.find("disc image") != std::string::npos);
  std::string error, why;
  REQUIRE(p.Prepare(found[0], &error));
  PortableState state;
  REQUIRE(ReadPortableState(p.layout.StateFile(), &state));
  CHECK(state.game_dir == "data:game");
  CHECK(fs::exists(p.root / "Data/game/Data/Sound/a.xwb"));
  CHECK_FALSE(fs::exists(p.root / "Data/game/$SystemUpdate"));
  CHECK_FALSE(fs::exists(p.root / "Data/game.part"));
  CHECK(Sha256File(p.root / "GameFiles/My Game.ISO") == before);
  CHECK(PortableReady(p.layout, found[0], state, &why));
  // Verify Game Files: a changed file is named.
  std::vector<std::string> bad;
  CHECK(VerifyPortableGameFiles(p.layout, state, p.manifest, &bad, nullptr, g_no_cancel));
  for (const std::string& b : bad) UNSCOPED_INFO("bad: " << b);
  CHECK(bad.empty());
  bad.clear();
  Sandbox::Write(p.root / "Data/game/Data/Sound/a.xwb", std::string(3000, 't'));
  CHECK(VerifyPortableGameFiles(p.layout, state, p.manifest, &bad, nullptr, g_no_cancel));
  REQUIRE(bad.size() == 1);
  CHECK(bad[0] == "Data/Sound/a.xwb");
}

TEST_CASE("portable: a retail dump with an unencrypted default.xex; a read-only program folder", "[portable]") {
  Portable p;
  Sandbox::Write(p.root / "GameFiles/dump/default.xex", p.retail);
  Sandbox::Write(p.root / "GameFiles/dump/Data/Sound/a.xwb", std::string(3000, 's'));
  Sandbox::Write(p.root / "GameFiles/default.xex.unencrypted/default.xex", p.plain);
  // The program folder cannot be written (a portable folder on read-only media for the runtime part).
  fs::permissions(p.root / "Runtime/bin", fs::perms(0555));
  auto found = FindPortableSources(p.root / "GameFiles", p.manifest);
  REQUIRE(!found.empty());
  REQUIRE(found[0].check.Ok());
  std::string error, why;
  const bool ok = p.Prepare(found[0], &error);
  fs::permissions(p.root / "Runtime/bin", fs::perms(0755));
  REQUIRE(ok);
  PortableState state;
  REQUIRE(ReadPortableState(p.layout.StateFile(), &state));
  CHECK(state.game_dir == "data:game");  // the dump's files with the unencrypted executable
  CHECK(InspectXex(p.root / "Data/game/default.xex").image_sha256 == InspectXexData(p.plain).image_sha256);
  CHECK(fs::exists(p.root / "Data/bin/librexruntimerd.so"));  // the runtime beside the program
  fs::permissions(p.root / "Runtime/bin", fs::perms(0555));
  CHECK(p.layout.Program() == p.root / "Data/bin/kuf_cod");
  CHECK(PortableReady(p.layout, found[0], state, &why));
  fs::permissions(p.root / "Runtime/bin", fs::perms(0755));
}

TEST_CASE("portable: a retail dump's encrypted default.xex is decrypted into Data/game; the dump is only read",
          "[portable][retail]") {
  if (!kXexDecryptionBuilt) {
    SUCCEED("no XEX decryption in this build: skipped");
    return;
  }
  const AesKey master = KeyFromHex("f00dfacecafebeef0123456789abcdef");  // a test key standing in for the retail one
  TestKey key(master);
  Portable p;
  const std::string plain = EncryptableXex(kImage80, nullptr), enc = EncryptableXex(kImage80, &master);
  p.manifest = ParseGameManifest("@title 424507D1 7092B5EC 00000001\n@retail-xex " + InspectXexData(enc).file_sha256 +
                                     " 1\n" + InspectXexData(plain).image_sha256 + "\t1\tdefault.xex\n" +
                                     Sha256Hex(std::string(3000, 's')) + "\t3000\tData/Sound/a.xwb\n",
                                 "test");
  REQUIRE(!p.manifest.files.empty());
  Sandbox::Write(p.root / "GameFiles/dump/default.xex", enc);
  Sandbox::Write(p.root / "GameFiles/dump/Data/Sound/a.xwb", std::string(3000, 's'));
  auto found = FindPortableSources(p.root / "GameFiles", p.manifest);
  REQUIRE(!found.empty());
  REQUIRE(found[0].check.Ok());
  CHECK(found[0].check.decrypt);
  CHECK(found[0].executable.empty());  // no separate executable needed
  std::string error, why;
  REQUIRE(p.Prepare(found[0], &error));
  PortableState state;
  REQUIRE(ReadPortableState(p.layout.StateFile(), &state));
  CHECK(state.game_dir == "data:game");
  CHECK(state.image_sha256 == "file:" + InspectXexData(enc).file_sha256);  // the version prepared, recorded
  CHECK(InspectXex(p.root / "Data/game/default.xex").status == XexStatus::kOk);
  CHECK(InspectXex(p.root / "Data/game/default.xex").image_sha256 == InspectXexData(plain).image_sha256);
  CHECK(Sha256File(p.root / "GameFiles/dump/default.xex") == Sha256Hex(enc));  // the player's file unchanged
  CHECK(PortableReady(p.layout, found[0], state, &why));
  // A second launch: nothing copied again.
  const auto before = fs::last_write_time(p.root / "Data/game/default.xex");
  REQUIRE(p.Prepare(found[0], &error));
  CHECK(fs::last_write_time(p.root / "Data/game/default.xex") == before);
  // A record of another executable (another dump of the version): not ready.
  PortableState other = state;
  other.image_sha256 = "file:" + std::string(64, '0');
  CHECK_FALSE(PortableReady(p.layout, found[0], other, &why));
}

TEST_CASE("portable: a cached object list with Windows line ends still links (the first Windows run)", "[portable]") {
  Portable p;
  p.PlainFolder();
  auto found = FindPortableSources(p.root / "GameFiles", p.manifest);
  REQUIRE(!found.empty());
  std::string error;
  REQUIRE(p.Prepare(found[0], &error));
  int lists = 0;
  for (const auto& e : fs::recursive_directory_iterator(p.layout.BuildCache())) {
    if (e.path().filename() != "objects.txt") continue;
    std::string text, crlf;
    REQUIRE(ReadFile(e.path(), &text));
    for (char c : text) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    REQUIRE(AtomicWrite(e.path(), crlf));
    ++lists;
  }
  REQUIRE(lists > 0);
  fs::remove(p.layout.Program());
  CHECK(p.Prepare(found[0], &error));
  CHECK(error.empty());
  CHECK(fs::exists(p.layout.Program()));
}

TEST_CASE("portable: a build cache that lost objects is built again from nothing, once (M17E)", "[portable]") {
  Portable p;
  p.PlainFolder();
  auto found = FindPortableSources(p.root / "GameFiles", p.manifest);
  REQUIRE(!found.empty());
  std::string error;
  REQUIRE(p.Prepare(found[0], &error));
  // The cache marked complete, its objects gone (a disk that lost files, a damaged copy of the folder): before
  // M17E every later preparation failed at the link, the same way, until the player deleted Data/build-cache.
  int removed = 0;
  for (const auto& e : fs::recursive_directory_iterator(p.layout.BuildCache())) {
    if (e.path().extension() == ".o") ++removed;
  }
  REQUIRE(removed > 0);
  for (const auto& e : fs::recursive_directory_iterator(p.layout.BuildCache())) {
    if (e.path().filename() == "obj") {
      fs::remove_all(e.path());
      break;
    }
  }
  fs::remove(p.layout.Program());
  CHECK(p.Prepare(found[0], &error));
  CHECK(error.empty());
  CHECK(fs::exists(p.layout.Program()));
  int rebuilt = 0;
  for (const auto& e : fs::recursive_directory_iterator(p.layout.BuildCache())) {
    if (e.path().extension() == ".o") ++rebuilt;
  }
  CHECK(rebuilt == removed);
  // The same damage when the rebuild itself fails: a real error that says both attempts failed (no loop).
  for (const auto& e : fs::recursive_directory_iterator(p.layout.BuildCache())) {
    if (e.path().filename() == "obj") {
      fs::remove_all(e.path());
      break;
    }
  }
  fs::remove(p.layout.Program());
  p.s.SetEnv("KUF_TEST_BUILD_FAILS", "1");
  CHECK_FALSE(p.Prepare(found[0], &error));
  CHECK(error.find("also after building it again from nothing") != std::string::npos);
  CHECK(error.find("missing from the cache") != std::string::npos);
}

TEST_CASE("portable: a preparation cancelled while compiling resumes; nothing half-written is used (M17E)",
          "[portable]") {
  Portable p;
  p.PlainFolder();
  auto found = FindPortableSources(p.root / "GameFiles", p.manifest);
  REQUIRE(!found.empty());
  std::atomic<bool> cancel{false};
  std::string error;
  const PortableProgress stop_when_compiling = [&](PortablePhase phase, double, const std::string&) {
    if (phase == PortablePhase::kCompile) cancel = true;
  };
  CHECK_FALSE(PreparePortable(p.layout, found[0], p.manifest, stop_when_compiling, cancel, &error, 1));
  CHECK_FALSE(fs::exists(p.layout.Program()));
  CHECK_FALSE(fs::exists(p.layout.StateFile()));  // no record of a prepared game
  PortableState state;
  std::string why;
  CHECK_FALSE(PortableReady(p.layout, found[0], state, &why));
  // Started again: it completes, and the next launch is direct.
  REQUIRE(p.Prepare(found[0], &error));
  REQUIRE(ReadPortableState(p.layout.StateFile(), &state));
  CHECK(PortableReady(p.layout, found[0], state, &why));
  for (const auto& e : fs::recursive_directory_iterator(p.root)) {
    CHECK(e.path().extension() != ".part");
  }
}
