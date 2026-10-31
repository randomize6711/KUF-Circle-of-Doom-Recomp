// Transactional content packages (patches/rexglue-sdk/0005, rex/system/xam/content_transaction.h): the
// host-side guarantee behind save/load reliability. A package directory is only ever the previous committed
// version or the new one; interrupted sessions never apply their changes; damage is detected on open.
// Runs on temporary directories only.

#include <rex/system/xam/content_transaction.h>

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace txn = rex::system::xam::content_txn;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::string tmpl = (fs::temp_directory_path() / "kuf-txn-XXXXXX").string();
    REQUIRE(mkdtemp(tmpl.data()) != nullptr);
    path = tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void Write(const fs::path& file, const std::string& text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}
std::string Read(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}
// Guest-visible files of a directory (runtime ".rex-" files excluded) and their contents.
std::map<std::string, std::string> Files(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (e.is_regular_file() && !e.path().filename().string().starts_with(".rex-")) {
      out[fs::relative(e.path(), dir).generic_string()] = Read(e.path());
    }
  }
  return out;
}
std::vector<std::string> Entries(const fs::path& dir) {
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path().filename().string());
  std::sort(out.begin(), out.end());
  return out;
}

// A committed package "Save" in root with the given files.
fs::path Committed(const fs::path& root, const std::map<std::string, std::string>& files) {
  const fs::path pkg = root / "Save";
  std::string error;
  REQUIRE(txn::Begin(pkg, fs::exists(pkg) ? txn::BeginMode::kReplace : txn::BeginMode::kCreate, &error));
  for (const auto& [name, text] : files) Write(txn::StagingPath(pkg) / name, text);
  auto r = txn::Commit(pkg);
  REQUIRE(r.ok);
  return pkg;
}

}  // namespace

TEST_CASE("sha256 known vectors", "[save][txn]") {
  CHECK(txn::Sha256Hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const std::string abc = "abc";
  CHECK(txn::Sha256Hex(std::span(reinterpret_cast<const uint8_t*>(abc.data()), abc.size())) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string two =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 56 bytes: padding spills a block
  CHECK(txn::Sha256Hex(std::span(reinterpret_cast<const uint8_t*>(two.data()), two.size())) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("create: the package appears only on commit, with a manifest", "[save][txn]") {
  TempDir t;
  const fs::path pkg = t.path / "Save";
  std::string error;
  REQUIRE(txn::Begin(pkg, txn::BeginMode::kCreate, &error));
  Write(txn::StagingPath(pkg) / "KD_1350", "character");
  CHECK_FALSE(fs::exists(pkg));  // not visible before commit
  auto r = txn::Commit(pkg);
  REQUIRE(r.ok);
  CHECK(r.changed);
  CHECK(r.files == 1);
  CHECK(r.bytes == 9);
  CHECK(Files(pkg) == std::map<std::string, std::string>{{"KD_1350", "character"}});
  CHECK(fs::exists(pkg / ".rex-manifest"));
  CHECK_FALSE(fs::exists(txn::StagingPath(pkg)));
  CHECK(txn::Verify(pkg, nullptr) == txn::Integrity::kOk);
}

TEST_CASE("open: changes stay in staging until commit, then replace the package", "[save][txn]") {
  TempDir t;
  const fs::path pkg = Committed(t.path, {{"KD_1350", "v1"}, {"GlobalSetting", "g"}});
  std::string error;
  REQUIRE(txn::Begin(pkg, txn::BeginMode::kOpen, &error));
  CHECK(Files(txn::StagingPath(pkg)) == Files(pkg));
  CHECK_FALSE(fs::exists(txn::StagingPath(pkg) / ".rex-manifest"));
  Write(txn::StagingPath(pkg) / "KD_1350", "v2-longer");
  Write(txn::StagingPath(pkg) / "LH_1271", "second character");
  CHECK(Read(pkg / "KD_1350") == "v1");
  REQUIRE(txn::Commit(pkg).ok);
  CHECK(Files(pkg) == std::map<std::string, std::string>{
                          {"GlobalSetting", "g"}, {"KD_1350", "v2-longer"}, {"LH_1271", "second character"}});
  CHECK(txn::Verify(pkg, nullptr) == txn::Integrity::kOk);
  CHECK(Entries(t.path) == std::vector<std::string>{"Save"});  // no leftovers
}

TEST_CASE("replace (CREATE_ALWAYS): the old package survives until the new one is committed", "[save][txn]") {
  TempDir t;
  const fs::path pkg = Committed(t.path, {{"KD_1350", "old"}, {"GlobalSetting", "g"}});
  std::string error;
  REQUIRE(txn::Begin(pkg, txn::BeginMode::kReplace, &error));
  CHECK(Files(txn::StagingPath(pkg)).empty());   // the guest sees an empty package, as with CREATE_ALWAYS
  CHECK(Read(pkg / "KD_1350") == "old");         // ... while the old one is still on disk
  Write(txn::StagingPath(pkg) / "GlobalSetting", "g2");
  REQUIRE(txn::Commit(pkg).ok);
  CHECK(Files(pkg) == std::map<std::string, std::string>{{"GlobalSetting", "g2"}});
}

TEST_CASE("flush (snapshot): commits and keeps staging mounted", "[save][txn]") {
  TempDir t;
  const fs::path pkg = Committed(t.path, {{"KD_1350", "v1"}});
  std::string error;
  REQUIRE(txn::Begin(pkg, txn::BeginMode::kOpen, &error));
  Write(txn::StagingPath(pkg) / "KD_1350", "v2");
  auto r = txn::Snapshot(pkg);
  REQUIRE(r.ok);
  CHECK(r.changed);
  CHECK(Read(pkg / "KD_1350") == "v2");
  CHECK(fs::exists(txn::StagingPath(pkg) / "KD_1350"));
  auto again = txn::Snapshot(pkg);  // the game flushes twice in a row
  REQUIRE(again.ok);
  CHECK_FALSE(again.changed);
  Write(txn::StagingPath(pkg) / "KD_1350", "v3");
  REQUIRE(txn::Snapshot(pkg).ok);
  CHECK(Read(pkg / "KD_1350") == "v3");
  CHECK(txn::Verify(pkg, nullptr) == txn::Integrity::kOk);
}

TEST_CASE("an unchanged commit leaves the package directory untouched", "[save][txn]") {
  TempDir t;
  const fs::path pkg = Committed(t.path, {{"KD_1350", "same"}});
  struct stat before {};
  REQUIRE(stat(pkg.c_str(), &before) == 0);
  std::string error;
  REQUIRE(txn::Begin(pkg, txn::BeginMode::kOpen, &error));
  auto r = txn::Commit(pkg);
  REQUIRE(r.ok);
  CHECK_FALSE(r.changed);
  struct stat after {};
  REQUIRE(stat(pkg.c_str(), &after) == 0);
  CHECK(before.st_ino == after.st_ino);
}

TEST_CASE("recovery: uncommitted changes of an interrupted session are never applied", "[save][txn]") {
  TempDir t;
  const fs::path pkg = Committed(t.path, {{"KD_1350", "valid save"}});
  std::string error;
  REQUIRE(txn::Begin(pkg, txn::BeginMode::kOpen, &error));
  Write(txn::StagingPath(pkg) / "KD_1350", "half-writ");  // the process dies here
  auto notes = txn::Recover(t.path);
  REQUIRE(notes.size() == 1);
  CHECK(notes[0].warning);
  CHECK(notes[0].text.find("not applied") != std::string::npos);
  CHECK(Read(pkg / "KD_1350") == "valid save");
  CHECK(txn::Verify(pkg, nullptr) == txn::Integrity::kOk);
  CHECK(Read(t.path / "Save.rex-interrupted" / "KD_1350") == "half-writ");  // kept for diagnostics only
  CHECK_FALSE(fs::exists(txn::StagingPath(pkg)));
  // A second interrupted session replaces the old diagnostics copy (bounded: one per package).
  REQUIRE(txn::Begin(pkg, txn::BeginMode::kOpen, &error));
  Write(txn::StagingPath(pkg) / "KD_1350", "again");
  REQUIRE(txn::Recover(t.path).size() == 1);
  CHECK(Read(t.path / "Save.rex-interrupted" / "KD_1350") == "again");
  CHECK(Entries(t.path) == std::vector<std::string>{"Save", "Save.rex-interrupted"});
}

TEST_CASE("recovery: a staging copy without changes is removed quietly", "[save][txn]") {
  TempDir t;
  const fs::path pkg = Committed(t.path, {{"KD_1350", "valid"}});
  std::string error;
  REQUIRE(txn::Begin(pkg, txn::BeginMode::kOpen, &error));  // the game exits with its package still open
  auto notes = txn::Recover(t.path);
  REQUIRE(notes.size() == 1);
  CHECK_FALSE(notes[0].warning);
  CHECK(Entries(t.path) == std::vector<std::string>{"Save"});
}

TEST_CASE("recovery: a new package that was never committed does not appear", "[save][txn]") {
  TempDir t;
  const fs::path pkg = t.path / "Save";
  std::string error;
  REQUIRE(txn::Begin(pkg, txn::BeginMode::kCreate, &error));
  Write(txn::StagingPath(pkg) / "GlobalSetting", "x");
  REQUIRE(txn::Recover(t.path).size() == 1);
  CHECK_FALSE(fs::exists(pkg));
}

TEST_CASE("recovery: a commit interrupted between renames is completed or rolled back", "[save][txn]") {
  TempDir t;
  // Without RENAME_EXCHANGE the old version is renamed to .rex-previous first. Died before the second
  // rename: the package is missing and .rex-previous holds the last committed version.
  Write(t.path / "Save.rex-previous" / "KD_1350", "previous");
  auto notes = txn::Recover(t.path);
  REQUIRE(notes.size() == 1);
  CHECK(Read(t.path / "Save" / "KD_1350") == "previous");
  // Died after the second rename: the new package is in place, .rex-previous is just removed.
  Write(t.path / "Save.rex-previous" / "KD_1350", "older");
  txn::Recover(t.path);
  CHECK(Read(t.path / "Save" / "KD_1350") == "previous");
  CHECK(Entries(t.path) == std::vector<std::string>{"Save"});
}

TEST_CASE("verify: damage is detected and described", "[save][txn]") {
  TempDir t;
  const fs::path pkg = Committed(t.path, {{"KD_1350", "0123456789"}, {"GlobalSetting", "settings"}});
  std::string detail;
  CHECK(txn::Verify(pkg, &detail) == txn::Integrity::kOk);
  SECTION("flipped byte") {
    Write(pkg / "KD_1350", "0123456780");
    CHECK(txn::Verify(pkg, &detail) == txn::Integrity::kDamaged);
    CHECK(detail.find("KD_1350: content differs") != std::string::npos);
  }
  SECTION("truncated file") {
    Write(pkg / "KD_1350", "01234");
    CHECK(txn::Verify(pkg, &detail) == txn::Integrity::kDamaged);
    CHECK(detail.find("KD_1350: size 5, expected 10") != std::string::npos);
  }
  SECTION("missing file") {
    fs::remove(pkg / "GlobalSetting");
    CHECK(txn::Verify(pkg, &detail) == txn::Integrity::kDamaged);
    CHECK(detail.find("GlobalSetting: missing") != std::string::npos);
  }
  SECTION("unexpected file") {
    Write(pkg / "junk", "x");
    CHECK(txn::Verify(pkg, &detail) == txn::Integrity::kDamaged);
    CHECK(detail.find("junk: unexpected file") != std::string::npos);
  }
  SECTION("damaged manifest") {
    Write(pkg / ".rex-manifest", "garbage");
    CHECK(txn::Verify(pkg, &detail) == txn::Integrity::kDamaged);
  }
  SECTION("no manifest: saves from before transactions or copied in") {
    fs::remove(pkg / ".rex-manifest");
    CHECK(txn::Verify(pkg, &detail) == txn::Integrity::kNoManifest);
  }
}

TEST_CASE("internal names are never content", "[save][txn]") {
  CHECK(txn::IsInternalName(".rex-manifest"));
  CHECK(txn::IsInternalName("Circle of Doom - Character Save.rex-staging"));
  CHECK(txn::IsInternalName("Circle of Doom - Character Save.rex-interrupted"));
  CHECK(txn::IsInternalName("Circle of Doom - Character Save.rex-previous"));
  CHECK_FALSE(txn::IsInternalName("Circle of Doom - Character Save"));
  CHECK_FALSE(txn::IsInternalName("KD_1350"));
}

TEST_CASE("atomic file write replaces whole files only", "[save][txn]") {
  TempDir t;
  const fs::path f = t.path / "x.header";
  std::string error;
  const std::string a(300, 'a'), b(5, 'b');
  REQUIRE(txn::AtomicWriteFile(f, std::span(reinterpret_cast<const uint8_t*>(a.data()), a.size()), &error));
  REQUIRE(txn::AtomicWriteFile(f, std::span(reinterpret_cast<const uint8_t*>(b.data()), b.size()), &error));
  CHECK(Read(f) == b);
  CHECK(Entries(t.path) == std::vector<std::string>{"x.header"});
  CHECK_FALSE(txn::AtomicWriteFile(t.path / "missing-dir" / "y",
                                   std::span(reinterpret_cast<const uint8_t*>(b.data()), b.size()), &error));
}

// The real thing: a child process commits new versions in a loop and is killed with SIGKILL at a random
// moment, 40 times. After each kill and recovery, the package must be exactly one complete committed version
// and pass verification.
TEST_CASE("killed during commit: the package is always one complete version", "[save][txn][kill]") {
  TempDir t;
  const fs::path pkg = t.path / "Save";
  auto version = [](int v) {
    std::map<std::string, std::string> files;
    for (int i = 0; i < 8; ++i) {
      files["file" + std::to_string(i)] = std::string(20000 + 997 * i, char('A' + v % 26)) + std::to_string(v);
    }
    return files;
  };
  Committed(t.path, version(0));
  std::mt19937 rng(1234);
  int interrupted = 0;
  for (int round = 0; round < 40; ++round) {
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
      for (int v = 1;; ++v) {
        std::string error;
        if (!txn::Begin(pkg, txn::BeginMode::kOpen, &error)) _exit(2);
        for (const auto& [name, text] : version(v)) {
          std::ofstream(txn::StagingPath(pkg) / name, std::ios::binary) << text;
        }
        if (!txn::Commit(pkg).ok) _exit(3);
      }
    }
    usleep(std::uniform_int_distribution<int>(2000, 60000)(rng));
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);
    REQUIRE(WIFSIGNALED(status));
    for (const auto& note : txn::Recover(t.path)) interrupted += note.warning;
    REQUIRE(fs::exists(pkg));
    std::string detail;
    REQUIRE(txn::Verify(pkg, &detail) == txn::Integrity::kOk);
    const auto files = Files(pkg);
    REQUIRE(files.size() == 8);
    // All files from the same version: the trailing number agrees everywhere.
    const auto& first = files.begin()->second;
    const std::string tag = first.substr(first.find_first_of("0123456789"));
    for (const auto& [name, text] : files) {
      REQUIRE(text.substr(text.find_first_of("0123456789")) == tag);
    }
  }
  INFO("rounds where the kill hit uncommitted work: " << interrupted);
  CHECK(interrupted > 0);  // the test did exercise interrupted writes
}

TEST_CASE("a damaged package the game deletes is kept aside, an intact one is not", "[save][txn]") {
  TempDir t;
  const fs::path pkg = Committed(t.path, {{"KD_1350", "0123456789"}});
  CHECK_FALSE(txn::SetAsideIfDamaged(pkg));  // intact: the caller deletes it as asked
  CHECK(fs::exists(pkg));
  Write(pkg / "KD_1350", "0123456780");
  CHECK(txn::SetAsideIfDamaged(pkg));
  CHECK_FALSE(fs::exists(pkg));
  CHECK(Read(t.path / "Save.rex-damaged" / "KD_1350") == "0123456780");
  CHECK(txn::IsInternalName("Save.rex-damaged"));
  CHECK_FALSE(txn::SetAsideIfDamaged(t.path / "missing"));
}
