// The game folder is mounted through a read-only HostPathDevice (the runtime's default, enforced by
// KufCodApp::OnPreSetup). Every way the guest could modify it must fail and leave the host files untouched.
// Uses a scratch directory with dummy files, never the real game folder.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/system/xtypes.h>

namespace fs = std::filesystem;
using rex::X_STATUS;

namespace {

std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct ScratchGameDir {
  fs::path root;
  ScratchGameDir() {
    root = fs::temp_directory_path() / ("kuf_ro_test_" + std::to_string(std::random_device{}()));
    fs::remove_all(root);
    fs::create_directories(root / "Data");
    std::ofstream(root / "initres.pak", std::ios::binary) << "original pak bytes";
    std::ofstream(root / "Data" / "x.sox", std::ios::binary) << "original sox bytes";
  }
  ~ScratchGameDir() { fs::remove_all(root); }
};

}  // namespace

TEST_CASE("read-only game mount refuses every modification", "[vfs]") {
  ScratchGameDir dir;
  rex::filesystem::HostPathDevice device("\\Device\\Harddisk0\\Partition1", dir.root, /*read_only=*/true);
  REQUIRE(device.Initialize());
  REQUIRE(device.is_read_only());

  rex::filesystem::Entry* pak = device.ResolvePath("initres.pak");
  REQUIRE(pak != nullptr);
  rex::filesystem::Entry* data = device.ResolvePath("Data");
  REQUIRE(data != nullptr);

  SECTION("open for write is denied") {
    rex::filesystem::File* file = nullptr;
    CHECK(pak->Open(rex::filesystem::FileAccess::kFileWriteData, &file) == X_STATUS_ACCESS_DENIED);
    CHECK(file == nullptr);
    CHECK(pak->Open(rex::filesystem::FileAccess::kFileAppendData, &file) == X_STATUS_ACCESS_DENIED);
  }
  SECTION("rename is denied (patches/rexglue-sdk/0001)") {
    CHECK(pak->Rename("game:\\renamed.pak") == X_STATUS_ACCESS_DENIED);
    CHECK(fs::exists(dir.root / "initres.pak"));
    CHECK_FALSE(fs::exists(dir.root / "renamed.pak"));
  }
  SECTION("delete, create, truncate and attribute changes are refused") {
    rex::filesystem::Entry* sox = device.ResolvePath("Data\\x.sox");
    REQUIRE(sox != nullptr);
    CHECK_FALSE(data->Delete(sox));
    CHECK(data->CreateEntry("new.bin", 0) == nullptr);
    CHECK_FALSE(pak->Truncate());
    CHECK_FALSE(pak->SetAttributes(0));
    CHECK_FALSE(fs::exists(dir.root / "Data" / "new.bin"));
  }
  CHECK(ReadAll(dir.root / "initres.pak") == "original pak bytes");
  CHECK(ReadAll(dir.root / "Data" / "x.sox") == "original sox bytes");
}
