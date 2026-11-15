// Retail dumps without decryption (docs/RETAIL_IMPORT_PLAN.md Stage A): executable identification, the reference
// list's '@' lines, folders matched ignoring case, a separate unencrypted executable, Xbox 360 disc images (XDVDFS)
// written by the test, and damaged images refused. The real retail folder is only read, and only when
// KUF_TEST_RETAIL_DIR names it (KUF_TEST_PLAIN_XEX: an unencrypted default.xex of the same version).

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>
#include <string>

#include "core/sha256.h"
#include "core/game_data.h"
#include "core/xex_decrypt.h"
#include "core/xiso.h"
#include "sandbox.h"
#include "xiso_builder.h"
#include "xex_builder.h"

using namespace kuf::setup;
using kuf::setup::test::Sandbox;
using kuf::setup::test::Xiso;
using kuf::setup::test::Le16;
using kuf::setup::test::Le32;
using kuf::setup::test::Patch;
using namespace kuf::setup::test;

namespace {

constexpr uint32_t kTitle = 0x424507D1, kMedia = 0x7092B5EC, kVersion = 1;

GameManifest TestManifest(const std::string& plain_image_sha, const std::string& retail_sha,
                          const std::map<std::string, std::string>& data) {
  std::string list = "@title 424507D1 7092B5EC 00000001\n@retail-xex " + retail_sha + " 1\n";
  list += plain_image_sha + "\t1\tdefault.xex\n";
  for (const auto& [path, content] : data) {
    list += Sha256Hex(content) + "\t" + std::to_string(content.size()) + "\t" + path + "\n";
  }
  return ParseGameManifest(list, "test");
}

}  // namespace

TEST_CASE("the reference list's '@' lines: title, retail executable; damaged ones refused", "[retail]") {
  const std::string sha(64, 'a');
  auto m = ParseGameManifest("@title 424507D1 7092B5EC 00000001\n@retail-xex " + sha + " 42\n@later x\n" + sha +
                                 "\t3\tdefault.xex\n",
                             "v1");
  REQUIRE(m.files.size() == 1);
  CHECK(m.title_id == kTitle);
  CHECK(m.media_id == kMedia);
  CHECK(m.version == kVersion);
  CHECK(m.retail_xex_sha256 == sha);
  CHECK(m.retail_xex_size == 42);
  CHECK(ParseGameManifest("@title 424507D1 xyz 1\n" + sha + "\t3\tdefault.xex\n", "v1").files.empty());
  CHECK(ParseGameManifest("@retail-xex abc 1\n" + sha + "\t3\tdefault.xex\n", "v1").files.empty());
  std::string text;
  REQUIRE(ReadFile(fs::path(KUF_SETUP_ASSETS) / "game_v1.tsv", &text));
  const auto real = ParseGameManifest(text, "v1");
  CHECK(real.files.size() == 137);
  CHECK(real.title_id == kTitle);
  CHECK(real.media_id == kMedia);
  CHECK(real.version == kVersion);
  CHECK(real.retail_xex_sha256 == "3ec406ccec65729acd61a2f891833704a28fbfe486746bf500d4aeaa872529ff");
  CHECK(real.retail_xex_size == 11808768);
}

TEST_CASE("executables are identified without a key: game, version, title update, retail, encryption", "[retail]") {
  const std::string plain = Sandbox::XexIdentified("IMAGE", 0, kTitle, kMedia, kVersion);
  const std::string retail = Sandbox::XexIdentified("SCRAMBLED", 1, kTitle, kMedia, kVersion);
  const XexInfo p = InspectXexData(plain), r = InspectXexData(retail);
  REQUIRE(p.status == XexStatus::kOk);
  CHECK(p.has_execution_info);
  CHECK(p.title_id == kTitle);
  CHECK(p.media_id == kMedia);
  CHECK(p.version == kVersion);
  CHECK(p.disc_number == 1);
  CHECK(p.disc_count == 1);
  CHECK(r.status == XexStatus::kEncrypted);
  CHECK(r.encryption == 1);
  CHECK(r.image_sha256.empty());
  CHECK(r.file_sha256.size() == 64);
  const GameManifest m = TestManifest(p.image_sha256, r.file_sha256, {});
  REQUIRE(!m.files.empty());
  CHECK(JudgeExecutable(p, m) == Executable::kSupported);
  CHECK(ExecutableMessage(Executable::kSupported, p, m).empty());
  CHECK(JudgeExecutable(r, m) == Executable::kRetailEncrypted);
  CHECK(NeedsSeparateExecutable(Executable::kRetailEncrypted));
  CHECK(JudgeExecutable(InspectXexData(Sandbox::XexIdentified("OTHER", 1, kTitle, kMedia, kVersion)), m) ==
        Executable::kEncrypted);
  CHECK(JudgeExecutable(InspectXexData(Sandbox::XexIdentified("OTHER", 0, kTitle, kMedia, kVersion)), m) ==
        Executable::kModified);
  const XexInfo game = InspectXexData(Sandbox::XexIdentified("IMAGE", 0, 0x4D5307E6, kMedia, kVersion));
  CHECK(JudgeExecutable(game, m) == Executable::kOtherGame);
  CHECK(ExecutableMessage(Executable::kOtherGame, game, m).find("4D5307E6") != std::string::npos);
  const XexInfo later = InspectXexData(Sandbox::XexIdentified("IMAGE", 1, kTitle, kMedia, 0x00000102));
  CHECK(JudgeExecutable(later, m) == Executable::kOtherVersion);
  CHECK(ExecutableMessage(Executable::kOtherVersion, later, m).find("0.0.1.2") != std::string::npos);
  CHECK(JudgeExecutable(InspectXexData(Sandbox::XexIdentified("IMAGE", 0, kTitle, 0x11111111, kVersion)), m) ==
        Executable::kOtherVersion);  // another release (media)
  CHECK(JudgeExecutable(InspectXexData(Sandbox::XexIdentified("IMAGE", 1, kTitle, kMedia, kVersion, 0x21)), m) ==
        Executable::kTitleUpdate);
  CHECK(JudgeExecutable(InspectXexData(Sandbox::Xex("IMAGE", 0, 2)), m) == Executable::kUnsupportedCompression);
  CHECK(JudgeExecutable(InspectXexData("MZ not an xex"), m) == Executable::kNotXex);
  CHECK(JudgeExecutable(XexInfo{}, m) == Executable::kUnreadable);
  CHECK(XexVersionString(0x20A0F1F3) == "2.0.41201.243");
  // Header fields that point past the end are not followed.
  std::string cut = plain.substr(0, 60);
  const XexInfo c = InspectXexData(cut);
  CHECK_FALSE(c.has_execution_info);
  CHECK(c.status == XexStatus::kNotXex);
  std::string bad = plain;
  bad[28] = '\x7F';  // the format header's offset far outside the file
  CHECK(InspectXexData(bad).status == XexStatus::kNotXex);
  CHECK(InspectXexData(std::string(kMaxXexBytes + 1, 'X')).status == XexStatus::kUnreadable);
}

TEST_CASE("a retail folder: recognised, matched ignoring case, usable with a separate executable", "[retail]") {
  Sandbox s;
  const fs::path g = s.dir / "dump";
  const std::string plain = Sandbox::XexIdentified("IMAGE", 0, kTitle, kMedia, kVersion);
  const std::string retail = Sandbox::XexIdentified("SCRAMBLED", 1, kTitle, kMedia, kVersion);
  Sandbox::Write(g / "Default.xex", retail);
  Sandbox::Write(g / "data/X.pak", "12345");
  Sandbox::Write(g / "$SystemUpdate/su.bin", "dashboard");
  Sandbox::Write(s.dir / "elsewhere/plain/default.xex", plain);
  const GameManifest m = TestManifest(InspectXexData(plain).image_sha256, InspectXexData(retail).file_sha256,
                                      {{"Data/x.pak", "12345"}});
  REQUIRE(!m.files.empty());
  CHECK(FindGameRoot(s.dir) == g);  // its only subfolder with a default.xex, in any case
  CHECK(FindGameRoot(g / "Default.xex") == g);  // default.xex picked directly: its folder
  CHECK(GameSource::Open(g / "Default.xex").kind() == GameSource::Kind::kFolder);
  CHECK(CheckGame(GameSource::Open(g / "Default.xex"), m).dump_verdict == Executable::kRetailEncrypted);
  CHECK(FindGameRoot(g / "data/X.pak") == g / "data/X.pak");  // another file stays a file (a disc image)
  auto c = CheckGame(GameSource::Folder(g), m);
  CHECK(c.folder_ok);
  CHECK(c.retail_layout);
  CHECK(c.missing.empty());
  CHECK(c.dump_verdict == Executable::kRetailEncrypted);
  CHECK(c.executable_encrypted);
  CHECK_FALSE(c.Ok());
  CHECK(GameCheckProblem(c, m).find("recognised") != std::string::npos);
  c = CheckGame(GameSource::Folder(g), m, s.dir / "elsewhere/plain/default.xex");
  CHECK(c.separate);
  CHECK(c.verdict == Executable::kSupported);
  CHECK_FALSE(c.executable_encrypted);
  CHECK(c.Ok());
  CHECK(GameCheckProblem(c, m).empty());
  Sandbox::Write(s.dir / "other.xex", Sandbox::XexIdentified("IMAGE", 0, kTitle, kMedia, 0x102));
  c = CheckGame(GameSource::Folder(g), m, s.dir / "other.xex");
  CHECK_FALSE(c.Ok());
  CHECK(GameCheckProblem(c, m).rfind("The separate default.xex: This is another version", 0) == 0);
  c = CheckGame(GameSource::Folder(g), m, s.dir / "absent.xex");
  CHECK(GameCheckProblem(c, m) == "The separate default.xex was not found.");
  // A plain dump of the supported version needs nothing more.
  Sandbox::Write(g / "Default.xex", plain);
  CHECK(CheckGame(GameSource::Folder(g), m).Ok());
}

TEST_CASE("Xbox 360 disc images: listed, checked and copied at every partition offset", "[retail]") {
  const std::string plain = Sandbox::XexIdentified("IMAGE", 0, kTitle, kMedia, kVersion);
  const std::string retail = Sandbox::XexIdentified("SCRAMBLED", 1, kTitle, kMedia, kVersion);
  const GameManifest m = TestManifest(InspectXexData(plain).image_sha256, InspectXexData(retail).file_sha256,
                                      {{"Data/x.pak", "12345"}, {"Data/Sound/y.xwb", std::string(5000, 'y')}});
  for (uint64_t partition : kXisoPartitionOffsets) {
    Sandbox s;
    Xiso x;
    x.partition = partition;
    x.files = {{"default.xex", retail},
               {"Data/X.PAK", "12345"},
               {"Data/Sound/y.xwb", std::string(5000, 'y')},
               {"Data/Sound/z.xwb", ""},
               {"$SystemUpdate/su20076000_00000000", "dashboard"},
               {"media/a", "a"},
               {"media/b", "bb"},
               {"media/c", "ccc"}};
    const fs::path iso = s.dir / "game.iso";
    x.Build(iso);
    CAPTURE(partition);
    CHECK(IsXisoFile(iso));
    const IsoImage image = ReadXiso(iso);
    REQUIRE(image.status == IsoStatus::kOk);
    CHECK(image.partition == partition);
    CHECK(image.files.size() == x.files.size());
    const GameSource src = GameSource::Open(iso);
    CHECK(src.kind() == GameSource::Kind::kIso);
    std::string got;
    REQUIRE(src.Read(src.Find("Data/Sound/y.xwb"), &got, 1 << 20));
    CHECK(got == std::string(5000, 'y'));
    CHECK(src.Find("data/x.pak").path == "Data/X.PAK");
    auto c = CheckGame(src, m);
    CHECK(c.kind == GameSource::Kind::kIso);
    CHECK(c.retail_layout);
    CHECK(c.missing.empty());
    CHECK(c.wrong_size.empty());
    CHECK(c.dump_verdict == Executable::kRetailEncrypted);
    Sandbox::Write(s.dir / "plain.xex", plain);
    CHECK(CheckGame(src, m, s.dir / "plain.xex").Ok());
    // A copy out of the image: the file's bytes, hashed; cancel leaves nothing.
    fs::create_directories(s.dir / "out");
    const std::string sha = src.Copy(src.Find("Data/Sound/y.xwb"), s.dir / "out/y.xwb", nullptr, nullptr);
    CHECK(sha == Sha256File(s.dir / "out/y.xwb"));
    CHECK(fs::file_size(s.dir / "out/y.xwb") == 5000);
    CHECK(src.Copy(src.Find("Data/Sound/z.xwb"), s.dir / "out/z.xwb", nullptr, nullptr).size() == 64);
    CHECK(src.Copy(src.Find("Data/Sound/y.xwb"), s.dir / "out/c.xwb", nullptr, [] { return true; }).empty());
    CHECK_FALSE(fs::exists(s.dir / "out/c.xwb"));
    CHECK_FALSE(fs::exists(s.dir / "out/c.xwb.part"));
  }
}

TEST_CASE("damaged or hostile disc images are refused, never followed out of range", "[retail]") {
  Sandbox s;
  const fs::path iso = s.dir / "game.iso";
  auto fresh = [&](std::map<std::string, std::string> files) {
    Xiso x;
    x.files = std::move(files);
    x.Build(iso);
    return x;
  };
  Sandbox::Write(s.dir / "plain.bin", std::string(200000, 'p'));
  CHECK(ReadXiso(s.dir / "plain.bin").status == IsoStatus::kNotXiso);
  CHECK_FALSE(IsXisoFile(s.dir / "plain.bin"));
  CHECK(ReadXiso(s.dir / "absent.iso").status == IsoStatus::kUnreadable);
  CHECK(GameSource::Open(s.dir / "plain.bin").error().find("not an Xbox 360 disc image") != std::string::npos);

  fresh({{"a", "x"}});
  Patch(iso, 32 * kXisoSector + 0x7EC, "MICROSOFT*XBOX*MEDIX");  // the second magic
  CHECK(ReadXiso(iso).status == IsoStatus::kNotXiso);

  fresh({{"a", "x"}});
  Patch(iso, 32 * kXisoSector + 20, Le32(0x00FFFFFF));  // the root table far past the end
  CHECK(ReadXiso(iso).status == IsoStatus::kMalformed);

  Xiso x = fresh({{"a", "x"}});
  Patch(iso, x.entry_at["a"] + 8, Le32(0x7FFFFFFF));  // a file larger than the image
  CHECK(ReadXiso(iso).status == IsoStatus::kMalformed);

  x = fresh({{"A/f", "x"}});
  Patch(iso, x.entry_at["A"] + 4, Le32(x.root_sector));  // a folder that is the root again
  Patch(iso, x.entry_at["A"] + 8, Le32(x.root_size));
  const IsoImage loop = ReadXiso(iso);
  CHECK(loop.status == IsoStatus::kMalformed);
  CHECK(loop.error.find("loop") != std::string::npos);

  x = fresh({{"a", "x"}, {"b", "y"}});
  Patch(iso, x.entry_at["a"] + 2, Le16(0));  // the chain's link back to the entry itself
  Patch(iso, x.entry_at["a"], Le16(0));
  CHECK(ReadXiso(iso).status == IsoStatus::kOk);  // offset 0 is "no child": the second file is just not reached
  Patch(iso, x.entry_at["a"] + 2, Le16(uint32_t((x.entry_at["b"] - x.entry_at["a"]) / 4)));
  Patch(iso, x.entry_at["b"], Le16(uint32_t((x.entry_at["b"] - x.entry_at["a"]) / 4)));  // b -> b
  CHECK(ReadXiso(iso).error.find("loop") != std::string::npos);

  x = fresh({{"ab", "x"}});
  Patch(iso, x.entry_at["ab"] + 14, "/b");  // a name with a separator
  CHECK(ReadXiso(iso).error == "an invalid file name");
  Patch(iso, x.entry_at["ab"] + 14, "..");
  CHECK(ReadXiso(iso).error == "an invalid file name");

  x = fresh({{"ab", "x"}});
  Patch(iso, x.entry_at["ab"] + 13, std::string(1, char(200)));  // a name past its table
  CHECK(ReadXiso(iso).status == IsoStatus::kMalformed);

  x = fresh({{"ab", "x"}, {"cd", "y"}});
  Patch(iso, x.entry_at["cd"] + 14, "AB");  // the same name twice, ignoring case
  CHECK(ReadXiso(iso).error.find("twice") != std::string::npos);

  std::string deep;
  for (int i = 0; i <= kXisoMaxDepth + 1; ++i) deep += "d/";
  fresh({{deep + "f", "x"}});
  CHECK(ReadXiso(iso).error == "folders nested too deeply");

  fresh({{"e/", ""}, {"f", "x"}});  // an empty folder: fine
  const IsoImage ok = ReadXiso(iso);
  CHECK(ok.status == IsoStatus::kOk);
}

TEST_CASE("the real retail folder (read only, when KUF_TEST_RETAIL_DIR is set)", "[retail][real]") {
  const std::string dir = Env("KUF_TEST_RETAIL_DIR");
  if (dir.empty()) {
    SUCCEED("KUF_TEST_RETAIL_DIR not set: skipped");
    return;
  }
  std::string text;
  REQUIRE(ReadFile(fs::path(KUF_SETUP_ASSETS) / "game_v1.tsv", &text));
  const GameManifest m = ParseGameManifest(text, "v1");
  auto c = CheckGame(GameSource::Open(dir), m);
  CHECK(c.folder_ok);
  CHECK(c.retail_layout);
  CHECK(c.missing.empty());
  CHECK(c.wrong_size.empty());
  // With the retail key in this build the encrypted executable is decrypted (and accepted); without it, recognised.
  CHECK(c.dump_verdict == (RetailDecryptionAvailable() ? Executable::kSupported : Executable::kRetailEncrypted));
  CHECK(c.decrypt == RetailDecryptionAvailable());
  CHECK(c.dump_xex.title_id == kTitle);
  const GameCheck direct = CheckGame(GameSource::Open(fs::path(dir) / "default.xex"), m);  // the executable picked
  CHECK(direct.folder_ok);
  CHECK(direct.dump_verdict == c.dump_verdict);
  const std::string plain = Env("KUF_TEST_PLAIN_XEX");
  if (!plain.empty()) {
    c = CheckGame(GameSource::Open(dir), m, plain);
    CHECK(c.verdict == Executable::kSupported);
    CHECK(c.Ok());
  }
}

// ---- decryption (core/xex_decrypt.h) ----------------------------------------------------------------------------

namespace {

}  // namespace

TEST_CASE("AES-128 matches FIPS-197 and SP 800-38A", "[retail][aes]") {
  const AesKey k = KeyFromHex("000102030405060708090a0b0c0d0e0f");
  std::string b = Unhex("00112233445566778899aabbccddeeff");
  AesEncryptBlock(k, reinterpret_cast<uint8_t*>(b.data()));
  CHECK(Hex(reinterpret_cast<uint8_t*>(b.data()), 16) == "69c4e0d86a7b0430d8cdb78070b4c55a");
  AesDecryptBlock(k, reinterpret_cast<uint8_t*>(b.data()));
  CHECK(Hex(reinterpret_cast<uint8_t*>(b.data()), 16) == "00112233445566778899aabbccddeeff");
  const AesKey k2 = KeyFromHex("2b7e151628aed2a6abf7158809cf4f3c");
  std::string c = Unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2");
  uint8_t iv[16];
  for (int i = 0; i < 16; ++i) iv[i] = uint8_t(i);
  AesCbcDecrypt(k2, iv, reinterpret_cast<uint8_t*>(c.data()), c.size());
  CHECK(Hex(reinterpret_cast<uint8_t*>(c.data()), 32) ==
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
}

TEST_CASE("an encrypted XEX decrypts to the unencrypted one; a wrong key or damage is caught", "[retail][aes]") {
  if (!kXexDecryptionBuilt) {
    std::string enc = EncryptableXex(kImage80, nullptr);
    CHECK(DecryptXexWithKey(&enc, AesKey{}) == XexDecrypt::kNoKey);  // the routine is not in this build
    CHECK_FALSE(RetailDecryptionAvailable());
    SUCCEED("no XEX decryption in this build (KUF_SETUP_DECRYPTION_CODE_FOR_TESTS tests it): skipped");
    return;
  }
  const AesKey master = KeyFromHex("f00dfacecafebeef0123456789abcdef");
  const std::string plain = EncryptableXex(kImage80, nullptr);
  std::string enc = EncryptableXex(kImage80, &master);
  REQUIRE(InspectXexData(enc).status == XexStatus::kEncrypted);
  REQUIRE(InspectXexData(plain).status == XexStatus::kOk);
  CHECK(enc.find("secret") == std::string::npos);
  std::string d = enc;
  REQUIRE(DecryptXexWithKey(&d, master) == XexDecrypt::kOk);
  CHECK(d.size() == plain.size());
  CHECK(d.substr(d.size() - 80) == kImage80);  // the payload; the header keeps its wrapped file key
  CHECK(InspectXexData(d).image_sha256 == InspectXexData(plain).image_sha256);
  d = enc;
  REQUIRE(DecryptXexWithKey(&d, KeyFromHex("00000000000000000000000000000001")) == XexDecrypt::kOk);
  CHECK(InspectXexData(d).image_sha256 != InspectXexData(plain).image_sha256);  // the image check rejects it
  std::string p = plain;
  CHECK(DecryptXexWithKey(&p, master) == XexDecrypt::kNotEncrypted);
  std::string cut = enc.substr(0, 100);
  CHECK(DecryptXexWithKey(&cut, master) == XexDecrypt::kMalformed);
  CHECK(cut.size() == 100);  // unchanged
  // Through the setup's check, with the test key standing in for the retail one.
  TestKey key(master);
  REQUIRE(RetailDecryptionAvailable());
  Sandbox s;
  Sandbox::Write(s.dir / "dump/default.xex", enc);
  Sandbox::Write(s.dir / "dump/Data/x.pak", "12345");
  const GameManifest m = TestManifest(InspectXexData(plain).image_sha256, InspectXexData(enc).file_sha256,
                                      {{"Data/x.pak", "12345"}});
  auto c = CheckGame(GameSource::Folder(s.dir / "dump"), m);
  CHECK(c.decrypt);
  CHECK(c.dump_verdict == Executable::kSupported);
  CHECK(c.Ok());
  std::string other = EncryptableXex(std::string(80, 'Z'), &master);  // the right title, another image
  Sandbox::Write(s.dir / "dump/default.xex", other);
  c = CheckGame(GameSource::Folder(s.dir / "dump"), m);
  CHECK_FALSE(c.decrypt);
  CHECK(c.dump_verdict == Executable::kDecryptedMismatch);
  CHECK(GameCheckProblem(c, m).find("was decrypted, but") != std::string::npos);
  Sandbox::Write(s.dir / "dump/default.xex", EncryptableXex(kImage80, &master, 0x4D5307E6));  // another game
  CHECK(CheckGame(GameSource::Folder(s.dir / "dump"), m).dump_verdict == Executable::kOtherGame);
}

TEST_CASE("the real retail default.xex decrypts to the supported image (read only, KUF_TEST_RETAIL_DIR)",
          "[retail][real]") {
  const std::string dir = Env("KUF_TEST_RETAIL_DIR");
  if (dir.empty() || !RetailDecryptionAvailable()) {
    SUCCEED("KUF_TEST_RETAIL_DIR not set or no retail key in this build: skipped");
    return;
  }
  std::string text;
  REQUIRE(ReadFile(fs::path(KUF_SETUP_ASSETS) / "game_v1.tsv", &text));
  const GameManifest m = ParseGameManifest(text, "v1");
  std::string data;
  REQUIRE(ReadFile(fs::path(dir) / "default.xex", &data));
  REQUIRE(DecryptRetailXex(&data) == XexDecrypt::kOk);
  CHECK(InspectXexData(data).image_sha256 == m.Find("default.xex")->sha256);
  const std::string plain = Env("KUF_TEST_PLAIN_XEX");
  if (!plain.empty()) CHECK(InspectXexData(data).file_sha256 == Sha256File(plain));
  const GameCheck c = CheckGame(GameSource::Open(dir), m);
  CHECK(c.decrypt);
  CHECK(c.Ok());
}
