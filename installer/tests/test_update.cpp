// The Windows updater's verification (M17G): SHA-512, Ed25519 and SSH signatures, which on Linux ssh-keygen does
// for tools/updater/kuf_update.py. Known vectors (FIPS 180-4, RFC 8032) and signatures made by ssh-keygen here.
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/update/ed25519.h"
#include "core/update/sha512.h"
#include "core/update/sshsig.h"
#include "core/platform.h"
#include "core/sha256.h"
#include "core/util.h"
#include "sandbox.h"

using namespace kuf::setup;
using namespace kuf::setup::update;
using kuf::setup::test::Sandbox;
namespace fs = std::filesystem;

namespace {

std::string Hex(const uint8_t* d, size_t n) {
  static const char* k = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += k[d[i] >> 4];
    s += k[d[i] & 15];
  }
  return s;
}

std::vector<uint8_t> Bytes(const std::string& hex) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) v.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
  return v;
}

}  // namespace

TEST_CASE("SHA-512: the standard's vectors and every padding boundary", "[update]") {
  auto hex = [](const std::string& s) {
    const auto d = Sha512Of(s);
    return Hex(d.data(), d.size());
  };
  CHECK(hex("") == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f6"
                   "3b931bd47417a81a538327af927da3e");
  CHECK(hex("abc") == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
                      "454d4423643ce80e2a9ac94fa54ca49f");
  CHECK(hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrst"
            "nopqrstu") == "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5"
                           "433ac7d329eeb6dd26545e96e55b874be909");
  CHECK(hex(std::string(1000, 'a')) == "67ba5535a46e3f86dbfbed8cbbaf0125c76ed549ff8b0b9e03e0c88cf90fa634fa7b12b47d7"
                                       "7b694de488ace8d9a65967dc96df599727d3292a8d9d447709c97");
  // Incremental updates give the same digest as one.
  Sha512 s;
  const std::string text(333, 'x');
  for (char c : text) s.Update(&c, 1);
  const auto a = s.Final(), b = Sha512Of(text);
  CHECK(a == b);
}

TEST_CASE("Ed25519: RFC 8032 vectors verify; any change is refused", "[update]") {
  struct V {
    const char *pk, *msg, *sig;
  } v[] = {
      {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
       "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe"
       "24655141438e7a100b"},
      {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
       "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302a"
       "eeb00d291612bb0c00"},
      {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
       "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed2"
       "8dc027beceea1ec40a"},
  };
  for (const V& t : v) {
    auto pk = Bytes(t.pk), m = Bytes(t.msg), s = Bytes(t.sig);
    CHECK(Ed25519Verify(s.data(), m.data(), m.size(), pk.data()));
    for (size_t i : {size_t(0), size_t(31), size_t(32), size_t(63)}) {
      s[i] ^= 1;
      CHECK_FALSE(Ed25519Verify(s.data(), m.data(), m.size(), pk.data()));
      s[i] ^= 1;
    }
    m.push_back(0);
    CHECK_FALSE(Ed25519Verify(s.data(), m.data(), m.size(), pk.data()));
    m.pop_back();
    pk[0] ^= 1;
    CHECK_FALSE(Ed25519Verify(s.data(), m.data(), m.size(), pk.data()));
    pk[0] ^= 1;
    // A non-canonical S (S + L) is refused (RFC 8032 5.1.7).
    std::vector<uint8_t> big = s;
    const uint8_t l[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                           0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};
    int carry = 0;
    for (int i = 0; i < 32; ++i) {
      const int x = big[32 + i] + l[i] + carry;
      big[32 + i] = uint8_t(x);
      carry = x >> 8;
    }
    CHECK_FALSE(Ed25519Verify(big.data(), m.data(), m.size(), pk.data()));
  }
}

#if !defined(_WIN32)
TEST_CASE("SSH signatures: ssh-keygen's are verified as ssh-keygen -Y verify does", "[update]") {
  if (std::system("ssh-keygen -? > /dev/null 2>&1; command -v ssh-keygen > /dev/null") != 0) {
    SKIP("ssh-keygen not installed");
  }
  Sandbox sb;
  const fs::path d = sb.dir;
  const std::string q = "'" + PathUtf8(d) + "'";
  REQUIRE(std::system(("cd " + q + " && ssh-keygen -q -t ed25519 -N '' -C t -f key < /dev/null > /dev/null 2>&1 && "
                       "ssh-keygen -q -t ed25519 -N '' -C o -f other < /dev/null > /dev/null 2>&1").c_str()) == 0);
  std::string pub;
  REQUIRE(ReadFile(d / "key.pub", &pub));
  pub = pub.substr(0, pub.rfind(' '));
  const std::string allowed = "# release keys\nrelease@kuf-cod-recomp namespaces=\"kuf-cod-recomp-release,"
                              "kuf-cod-recomp-manifest\" " + pub + "\n";
  std::string package(70000, '\0');
  for (size_t i = 0; i < package.size(); ++i) package[i] = char((i * 2654435761u) >> 13);
  for (const char* f : {"a", "b", "c"}) REQUIRE(AtomicWrite(d / f, package));
  REQUIRE(std::system(("cd " + q + " && ssh-keygen -q -Y sign -f key -n kuf-cod-recomp-release a < /dev/null > /dev/null 2>&1"
                       " && ssh-keygen -q -Y sign -f key -n kuf-cod-recomp-manifest -O hashalg=sha256 b < /dev/null "
                       "> /dev/null 2>&1 && ssh-keygen -q -Y sign -f other -n kuf-cod-recomp-release c < /dev/null "
                       "> /dev/null 2>&1").c_str()) == 0);
  std::string a, b, c, why;
  REQUIRE(ReadFile(d / "a.sig", &a));
  REQUIRE(ReadFile(d / "b.sig", &b));
  REQUIRE(ReadFile(d / "c.sig", &c));
  const std::string id = "release@kuf-cod-recomp";
  CHECK(VerifySshSignature(allowed, id, "kuf-cod-recomp-release", package, a, &why));
  CHECK(VerifySshSignature(allowed, id, "kuf-cod-recomp-manifest", package, b, &why));  // SHA-256 hash
  CHECK_FALSE(VerifySshSignature(allowed, id, "kuf-cod-recomp-manifest", package, a, &why));  // other namespace
  CHECK(why.find("signed for 'kuf-cod-recomp-release'") != std::string::npos);
  CHECK_FALSE(VerifySshSignature(allowed, "someone@else", "kuf-cod-recomp-release", package, a, &why));
  CHECK_FALSE(VerifySshSignature(allowed, id, "kuf-cod-recomp-release", package, c, &why));  // a key not listed
  std::string changed = package;
  changed[12345] ^= 4;
  CHECK_FALSE(VerifySshSignature(allowed, id, "kuf-cod-recomp-release", changed, a, &why));
  CHECK(why == "the signature does not match the file");
  // A key allowed only for the manifest namespace does not sign packages.
  const std::string manifest_only = "release@kuf-cod-recomp namespaces=\"kuf-cod-recomp-manifest\" " + pub + "\n";
  CHECK_FALSE(VerifySshSignature(manifest_only, id, "kuf-cod-recomp-release", package, a, &why));
  // Windows line ends in allowed_signers (a file that went through a Windows editor) still read.
  std::string crlf;
  for (char ch : allowed) crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
  CHECK(VerifySshSignature(crlf, id, "kuf-cod-recomp-release", package, a, &why));
}
#endif

TEST_CASE("base64 decoding: valid text, padding, refusals", "[update]") {
  std::string out;
  CHECK(DecodeBase64("aGVsbG8gd29ybGQ=", &out));
  CHECK(out == "hello world");
  CHECK(DecodeBase64("aGVs\nbG8=", &out));
  CHECK(out == "hello");
  CHECK_FALSE(DecodeBase64("aGVs*bG8=", &out));
  CHECK_FALSE(DecodeBase64("aGVsbG8=A", &out));
}

#include "core/update/json.h"
#include "core/update/zip.h"

TEST_CASE("JSON: GitHub's release list and the update manifest read; malformed text refused", "[update]") {
  const auto v = ParseJson(R"([{"tag_name": "v1.2.3", "draft": false, "prerelease": true,
      "assets": [{"name": "a.zip", "size": 123}], "body": "line\nnext \u00dc \ud83d\ude00"}])");
  REQUIRE(v);
  REQUIRE(v->type == Json::kArray);
  const Json& r = v->items.at(0);
  CHECK(r.String("tag_name") == "v1.2.3");
  CHECK(r.Bool("prerelease"));
  CHECK_FALSE(r.Bool("draft"));
  CHECK(r.String("body") == "line\nnext \xC3\x9C \xF0\x9F\x98\x80");
  CHECK(r.Get("assets")->items.at(0).Integer("size") == 123u);
  for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "\"\\x\"", "01", "1.", "[1] x", "\"\\ud800\"", "{\"a\" 1}"}) {
    CHECK_FALSE(ParseJson(bad));
  }
  std::string deep(100, '[');
  deep += std::string(100, ']');
  CHECK_FALSE(ParseJson(deep));  // depth bounded
  auto num = ParseJson(R"({"a": 1.5, "b": -3, "c": 7})");
  REQUIRE(num);
  CHECK_FALSE(num->Integer("a"));
  CHECK_FALSE(num->Integer("b"));
  CHECK(num->Integer("c") == 7u);
  CHECK(JsonQuote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
}

#if !defined(_WIN32)
TEST_CASE("ZIP: Python's archives extract byte for byte; hostile and damaged ones are refused", "[update]") {
  if (std::system("python3 -c 'import zipfile' > /dev/null 2>&1") != 0) SKIP("python3 not installed");
  Sandbox sb;
  const fs::path d = sb.dir;
  const std::string q = "'" + PathUtf8(d) + "'";
  // Contents: empty, small, highly repetitive, random, large; stored and deflated.
  REQUIRE(std::system(("cd " + q + " && python3 -I -c \"\nimport os, random, zipfile\nrandom.seed(7)\n"
      "files = {'top/empty.txt': b'', 'top/small.txt': b'hello', 'top/rep.bin': b'abcabc' * 50000,\n"
      "         'top/sub/random.bin': bytes(random.getrandbits(8) for _ in range(300000)),\n"
      "         'top/sub/text.txt': ''.join(random.choice('ab cd\\n') for _ in range(200000)).encode()}\n"
      "for name, method in (('good.zip', zipfile.ZIP_DEFLATED), ('stored.zip', zipfile.ZIP_STORED)):\n"
      "    with zipfile.ZipFile(name, 'w', method) as z:\n"
      "        z.writestr('top/', b'')\n"
      "        for n, c in files.items(): z.writestr(n, c)\n"
      "os.makedirs('ref/top/sub', exist_ok=True)\n"
      "for n, c in files.items(): open('ref/' + n, 'wb').write(c)\n"
      "for bad, n in (('dotdot.zip', '../evil'), ('abs.zip', '/etc/evil'), ('drive.zip', 'C:/evil'),\n"
      "               ('backslash.zip', 'top\\\\\\\\evil'), ('mid.zip', 'top/../../evil')):\n"
      "    with zipfile.ZipFile(bad, 'w') as z: z.writestr(n, b'x')\n"
      "data = bytearray(open('good.zip', 'rb').read())\n"
      "i = data.find(b'abcabc')\n"
      "with zipfile.ZipFile('crc.zip', 'w', zipfile.ZIP_STORED) as z: z.writestr('top/a.txt', b'A' * 1000)\n"
      "c = bytearray(open('crc.zip', 'rb').read()); c[c.find(b'AAAA')] ^= 1; open('crc.zip', 'wb').write(c)\n"
      "open('trunc.zip', 'wb').write(open('good.zip', 'rb').read()[:5000])\n"
      "\"").c_str()) == 0);
  for (const char* name : {"good.zip", "stored.zip"}) {
    std::vector<ZipEntry> entries;
    std::string why;
    REQUIRE(ReadZipDirectory(d / name, &entries, &why));
    CHECK(entries.size() == 6);
    const fs::path out = d / (std::string("out-") + name);
    REQUIRE(ExtractZip(d / name, entries, [](const std::string& n) { return n; }, out, &why));
    for (const char* f : {"top/empty.txt", "top/small.txt", "top/rep.bin", "top/sub/random.bin", "top/sub/text.txt"}) {
      std::string got, want;
      REQUIRE(ReadFile(out / f, &got));
      REQUIRE(ReadFile(d / "ref" / f, &want));
      CHECK(got == want);
    }
  }
  // Only what the mapping names is written, where it says.
  std::vector<ZipEntry> entries;
  std::string why;
  REQUIRE(ReadZipDirectory(d / "good.zip", &entries, &why));
  REQUIRE(ExtractZip(d / "good.zip", entries,
                     [](const std::string& n) { return n == "top/small.txt" ? std::string("new/name.txt") : ""; },
                     d / "picked", &why));
  CHECK(fs::exists(d / "picked/new/name.txt"));
  CHECK_FALSE(fs::exists(d / "picked/top"));
  for (const char* bad : {"dotdot.zip", "abs.zip", "drive.zip", "backslash.zip", "mid.zip"}) {
    CHECK_FALSE(ReadZipDirectory(d / bad, &entries, &why));
    CHECK(why.find("unsafe name") != std::string::npos);
  }
  CHECK_FALSE(fs::exists(d / "evil"));
  REQUIRE(ReadZipDirectory(d / "crc.zip", &entries, &why));
  CHECK_FALSE(ExtractZip(d / "crc.zip", entries, [](const std::string& n) { return n; }, d / "crc-out", &why));
  CHECK(why.find("checksum") != std::string::npos);
  CHECK_FALSE(fs::exists(d / "crc-out/top/a.txt"));  // nothing unverified is written
  CHECK_FALSE(ReadZipDirectory(d / "trunc.zip", &entries, &why));
}
#endif

TEST_CASE("inflate: stored and fixed blocks by hand; damage and over-long output refused", "[update]") {
  std::string out;
  const uint8_t stored[] = {0x01, 0x05, 0x00, 0xfa, 0xff, 'h', 'e', 'l', 'l', 'o'};
  CHECK(Inflate(stored, sizeof(stored), &out, 100));
  CHECK(out == "hello");
  CHECK_FALSE(Inflate(stored, sizeof(stored), &out, 3));  // the limit
  const uint8_t bad_len[] = {0x01, 0x05, 0x00, 0xfb, 0xff, 'h', 'e', 'l', 'l', 'o'};
  CHECK_FALSE(Inflate(bad_len, sizeof(bad_len), &out, 100));
  const uint8_t fixed_hello[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00};  // zlib's raw deflate of "hello"
  CHECK(Inflate(fixed_hello, sizeof(fixed_hello), &out, 100));
  CHECK(out == "hello");
  CHECK_FALSE(Inflate(fixed_hello, 3, &out, 100));  // truncated
  const uint8_t reserved_type[] = {0x07};
  CHECK_FALSE(Inflate(reserved_type, 1, &out, 100));
  CHECK(Crc32("123456789", 9) == 0xCBF43926u);
}

#include "core/update/updater.h"

#if !defined(_WIN32)
namespace {

// A program folder of version `v` (Play KUF, Runtime/ with VERSION, update.conf, allowed_signers, a recipe) and a
// signed release of `next` in `releases/`, made with a throwaway key (ssh-keygen) and Python's zipfile.
struct UpdateFixture {
  Sandbox sb;
  fs::path root, releases, key;
  std::string signers;
  bool ok = false;
  UpdateFixture() {
    if (std::system("command -v ssh-keygen > /dev/null && python3 -c 'import zipfile' > /dev/null 2>&1") != 0) return;
    root = sb.dir / "KUF Portable Ü";
    releases = sb.dir / "releases";
    key = sb.dir / "key";
    fs::create_directories(releases);
    if (std::system(("ssh-keygen -q -t ed25519 -N '' -C t -f '" + PathUtf8(key) + "' < /dev/null > /dev/null 2>&1")
                        .c_str()) != 0) {
      return;
    }
    std::string pub;
    ReadFile(fs::path(PathUtf8(key) + ".pub"), &pub);
    signers = "release@kuf-cod-recomp namespaces=\"kuf-cod-recomp-release,kuf-cod-recomp-manifest\" " +
              pub.substr(0, pub.rfind(' ')) + "\n";
    Program(root, "0.9.0", std::string(64, 'a'));
    Sandbox::Write(root / "Runtime/bin/kuf_cod", "the prepared game");
    Sandbox::Write(root / "Data/saves/save.bin", "my progress");
    Sandbox::Write(root / "GameFiles/default.xex", "game");
    ok = true;
  }
  void Program(const fs::path& r, const std::string& v, const std::string& generation) {
    Sandbox::Write(r / "Play KUF", "launcher " + v);
    Sandbox::Write(r / "Runtime/VERSION", v + "\n");
    Sandbox::Write(r / "Runtime/update.conf",
                   "repo = owner/kuf\nplatform = linux-x86_64-portable\nasset_prefix = kuf-cod-recomp\n");
    Sandbox::Write(r / "Runtime/allowed_signers", signers);
    Sandbox::Write(r / "Runtime/builder/recipe.txt", "generation_id " + generation + "\n");
    Sandbox::Write(r / "Runtime/bin/rexgluerd", "generator " + v);
  }
  // Builds the release of `v`; `mutate` may change the files before they are signed or after.
  void Release(const std::string& v, const std::string& generation, const std::string& tweak = "") {
    const fs::path tree = sb.dir / ("tree-" + v);
    Program(tree / "KUF-Circle-of-Doom-Portable", v, generation);
    Sandbox::Write(tree / "KUF-Circle-of-Doom-Portable/GameFiles/readme.txt", "put your game here");
    const std::string base = "kuf-cod-recomp-" + v + "-linux-x86_64-portable";
    const std::string zip = PathUtf8(releases / (base + ".zip"));
    REQUIRE(std::system(("cd '" + PathUtf8(tree) + "' && python3 -I -c \"import os, sys, zipfile\n"
                         "with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:\n"
                         "    for d, _, fs in os.walk('KUF-Circle-of-Doom-Portable'):\n"
                         "        for f in sorted(fs): z.write(os.path.join(d, f))\n\" '" + zip + "'").c_str()) == 0);
    std::string data;
    REQUIRE(ReadFile(PathFromUtf8(zip), &data));
    const std::string sha = tweak == "wrong-sha" ? std::string(64, '0') : Sha256Hex(data);
    const std::string manifest = "{\"schema\": 1, \"product\": \"kuf-cod-recomp\", \"version\": \"" + v +
                                 "\", \"platform\": \"linux-x86_64-portable\", \"published\": \"2026-11-10T00:00:00Z\","
                                 " \"summary\": \"Fixes.\", \"notes\": \"Line 1\\nLine 2\", \"package\": {\"name\": \"" +
                                 base + ".zip\", \"size\": " + std::to_string(data.size()) + ", \"sha256\": \"" + sha +
                                 "\"}}";
    REQUIRE(AtomicWrite(releases / (base + ".update.json"), manifest));
    const std::string ns_manifest = tweak == "manifest-wrong-namespace" ? "kuf-cod-recomp-release" : "kuf-cod-recomp-manifest";
    const std::string sign = "ssh-keygen -q -Y sign -f '" + PathUtf8(key) + "' -n ";
    REQUIRE(std::system((sign + "kuf-cod-recomp-release '" + zip + "' < /dev/null > /dev/null 2>&1 && " + sign +
                         ns_manifest + " '" + PathUtf8(releases / (base + ".update.json")) +
                         "' < /dev/null > /dev/null 2>&1").c_str()) == 0);
    if (tweak == "tampered") {  // a byte of the package changed after signing (and the manifest's hash too)
      data[data.size() / 2] ^= 1;
      REQUIRE(AtomicWrite(PathFromUtf8(zip), data));
    }
    std::string list = "[";
    for (const auto& e : fs::directory_iterator(releases)) {
      const std::string n = PathUtf8(e.path().filename());
      if (n.size() > 4 && n.substr(n.size() - 4) == ".zip") {
        const std::string ver = n.substr(15, n.find("-linux") - 15);
        if (list.size() > 1) list += ",";
        list += "{\"tag_name\": \"v" + ver + "\", \"html_url\": \"https://github.com/owner/kuf/releases/tag/v" + ver +
                "\", \"assets\": [";
        for (const char* ext : {".zip", ".zip.sig", ".update.json", ".update.json.sig"}) {
          list += std::string(ext == std::string(".zip") ? "" : ",") + "{\"name\": \"kuf-cod-recomp-" + ver +
                  "-linux-x86_64-portable" + ext + "\"}";
        }
        list += "]}";
      }
    }
    REQUIRE(AtomicWrite(releases / "releases.json", list + "]"));
  }
};

}  // namespace

TEST_CASE("updater: check, update staged, applied at the next start, rolled back; saves untouched", "[update]") {
  UpdateFixture f;
  if (!f.ok) SKIP("ssh-keygen or python3 not installed");
  f.Release("0.9.1", std::string(64, 'a'));  // the same generation: the prepared game is carried over
  const ProgramFolder folder{f.root};
  auto source = DirectorySource(f.releases);
  std::atomic<bool> no{false};
  CheckResult c = Check(folder, *source, {});
  REQUIRE(c.error.empty());
  CHECK(c.exit == Exit::kDone);
  CHECK(c.update);
  CHECK(c.current == "0.9.0");
  CHECK(c.latest == "0.9.1");
  CHECK(c.summary == "Fixes.");
  CHECK(c.notes == "Line 1\nLine 2");
  CHECK(c.page == "https://github.com/owner/kuf/releases/tag/v0.9.1");
  std::vector<std::string> stages;
  uint64_t last = 0;
  CheckResult u = Update(folder, *source, {}, [&](uint64_t done, uint64_t) { last = done; },
                         [&](const std::string& s) { stages.push_back(s); }, no);
  REQUIRE(u.error.empty());
  CHECK(u.exit == Exit::kDone);
  CHECK(last > 0);
  CHECK(stages == std::vector<std::string>{"verifying", "installing"});
  // Staged only: the running version is untouched until the next start.
  CHECK(folder.CurrentVersion() == "0.9.0");
  CHECK(folder.PendingVersion() == "0.9.1");
  std::string applied, why;
  REQUIRE(ApplyStagedUpdate(folder, &applied, &why));
  CHECK(applied == "0.9.1");
  CHECK(folder.CurrentVersion() == "0.9.1");
  CHECK(folder.PreviousVersion() == "0.9.0");
  std::string text;
  REQUIRE(ReadFile(f.root / "Play KUF", &text));
  CHECK(text == "launcher 0.9.1");
  REQUIRE(ReadFile(f.root / "Runtime/bin/kuf_cod", &text));
  CHECK(text == "the prepared game");  // carried over: same generation id, no recompiling
  REQUIRE(ReadFile(f.root / "Data/saves/save.bin", &text));
  CHECK(text == "my progress");
  CHECK(fs::exists(f.root / "GameFiles/default.xex"));
  CHECK_FALSE(fs::exists(f.root / "GameFiles/readme.txt"));  // only Play KUF and Runtime/ are taken
  CHECK(ApplyStagedUpdate(folder, &applied, &why));  // nothing staged: nothing happens
  CHECK(applied.empty());
  // Up to date now.
  CHECK(Check(folder, *source, {}).exit == Exit::kUpToDate);
  // Back to 0.9.0, and forward again.
  std::string now;
  REQUIRE(Rollback(folder, &now, &why));
  CHECK(now == "0.9.0");
  CHECK(folder.CurrentVersion() == "0.9.0");
  REQUIRE(ReadFile(f.root / "Play KUF", &text));
  CHECK(text == "launcher 0.9.0");
  REQUIRE(Rollback(folder, &now, &why));
  CHECK(folder.CurrentVersion() == "0.9.1");
}

TEST_CASE("updater: a new generation is prepared again; refusals change nothing", "[update]") {
  UpdateFixture f;
  if (!f.ok) SKIP("ssh-keygen or python3 not installed");
  const ProgramFolder folder{f.root};
  std::atomic<bool> no{false};
  const auto update = [&](const std::string& v, const std::string& tweak, Exit want, const std::string& what) {
    f.Release(v, std::string(64, 'b'), tweak);
    auto source = DirectorySource(f.releases);
    CheckResult r = Update(folder, *source, {}, nullptr, nullptr, no);
    CHECK(r.exit == want);
    CHECK(r.error.find(what) != std::string::npos);
    CHECK(folder.CurrentVersion() == "0.9.0");
    CHECK(folder.PendingVersion().empty());
    CHECK_FALSE(fs::exists(folder.Next()));
    fs::remove_all(f.releases);
    fs::create_directories(f.releases);
  };
  update("0.9.2", "tampered", Exit::kRefused, "not the manifest's size and SHA-256");
  update("0.9.2", "wrong-sha", Exit::kRefused, "not the manifest's size and SHA-256");
  update("0.9.2", "manifest-wrong-namespace", Exit::kRefused, "update manifest of 0.9.2");
  // An older release is not offered (no downgrade).
  f.Release("0.8.9", std::string(64, 'b'));
  {
    auto source = DirectorySource(f.releases);
    CHECK(Update(folder, *source, {}, nullptr, nullptr, no).exit == Exit::kUpToDate);
    CHECK(folder.PendingVersion().empty());
  }
  // Another key's release: refused.
  fs::remove_all(f.releases);
  fs::create_directories(f.releases);
  f.Release("0.9.3", std::string(64, 'b'));
  const std::string good = f.signers;
  Sandbox::Write(f.root / "Runtime/allowed_signers",
                 "release@kuf-cod-recomp ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n");
  {
    auto source = DirectorySource(f.releases);
    CheckResult r = Update(folder, *source, {}, nullptr, nullptr, no);
    CHECK(r.exit == Exit::kRefused);
    CHECK(folder.PendingVersion().empty());
  }
  Sandbox::Write(f.root / "Runtime/allowed_signers", good);
  // A new generation: the update applies, and the prepared game is not carried over (prepared again).
  {
    auto source = DirectorySource(f.releases);
    REQUIRE(Update(folder, *source, {}, nullptr, nullptr, no).exit == Exit::kDone);
    std::string applied, why;
    REQUIRE(ApplyStagedUpdate(folder, &applied, &why));
    CHECK(applied == "0.9.3");
    CHECK_FALSE(fs::exists(f.root / "Runtime/bin/kuf_cod"));
  }
}
#endif

TEST_CASE("versions: semver precedence as the Linux updater's", "[update]") {
  const auto v = [](const char* s) { return *ParseVersion(s); };
  CHECK(CompareVersions(v("1.0.0"), v("1.0.0")) == 0);
  CHECK(CompareVersions(v("1.0.0-rc.1"), v("1.0.0")) < 0);
  CHECK(CompareVersions(v("1.0.0-rc.2"), v("1.0.0-rc.10")) < 0);
  CHECK(CompareVersions(v("1.0.0-alpha"), v("1.0.0-alpha.1")) < 0);
  CHECK(CompareVersions(v("1.0.0-1"), v("1.0.0-alpha")) < 0);
  CHECK(CompareVersions(v("0.9.0-dev.m16"), v("0.9.0-dev.m17")) < 0);
  CHECK(CompareVersions(v("0.10.0"), v("0.9.9")) > 0);
  for (const char* bad : {"", "1", "1.2", "v1.2.3", "1.2.3-", "1.2.3+build", "1.2.3-a..b", "01.x.3"}) {
    CHECK_FALSE(ParseVersion(bad));
  }
}

TEST_CASE("updater: the channel follows update.conf, else the installed version", "[update]") {
  UpdateConfig c;
  CHECK(FollowsPrereleases(c, "0.9.0-rc.1", false));   // no channel line: a pre-release follows the pre-releases
  CHECK_FALSE(FollowsPrereleases(c, "1.0.0", false));  // and a release the releases
  CHECK(FollowsPrereleases(c, "1.0.0", true));         // --prerelease
  c.channel = "stable";
  CHECK_FALSE(FollowsPrereleases(c, "0.9.0-rc.1", false));
  c.channel = "prerelease";
  CHECK(FollowsPrereleases(c, "1.0.0", false));
}
