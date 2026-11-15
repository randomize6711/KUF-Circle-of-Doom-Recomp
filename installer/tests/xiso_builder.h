// An Xbox 360 disc image (XDVDFS) written by the tests (installer/tests/test_retail.cpp, test_install.cpp).
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "core/xiso.h"

namespace kuf::setup::test {

inline std::string Le16(uint32_t v) { return std::string{char(v), char(v >> 8)}; }
inline std::string Le32(uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; }

// An XDVDFS image: files by path (folders implied; a path ending in '/' is an empty folder). Each folder's table is a
// chain of entries, linked alternately by left and right offsets, so both are exercised.
struct Xiso {
  std::map<std::string, std::string> files;
  uint64_t partition = 0;
  uint32_t root_sector = 0, root_size = 0;
  std::map<std::string, uint64_t> entry_at;  // path -> the image offset of its directory entry
  std::map<uint64_t, std::string> chunks;    // image offset -> bytes
  uint32_t next = 33;                        // the next free sector (32 is the volume descriptor)

  uint32_t Alloc(uint64_t bytes) {
    const uint32_t s = next;
    next += std::max<uint32_t>(1, uint32_t((bytes + kXisoSector - 1) / kXisoSector));
    return s;
  }
  // Writes the folder `prefix` ("" for the root) and returns its table's (sector, size).
  std::pair<uint32_t, uint32_t> Folder(const std::string& prefix) {
    std::map<std::string, bool> children;  // name -> is a folder
    for (const auto& [path, data] : files) {
      if (path.rfind(prefix, 0) != 0 || path.size() == prefix.size()) continue;
      const std::string rest = path.substr(prefix.size());
      const size_t slash = rest.find('/');
      if (slash == std::string::npos) children[rest] = false;
      else children[rest.substr(0, slash)] = true;
    }
    if (children.empty()) return {0, 0};
    struct E { std::string name; uint32_t sector, size; bool dir; };
    std::vector<E> entries;
    for (const auto& [name, dir] : children) {
      if (dir) {
        const auto [sec, size] = Folder(prefix + name + "/");
        entries.push_back({name, sec, size, true});
      } else {
        const std::string& data = files.at(prefix + name);
        const uint32_t sec = Alloc(data.size());
        chunks[partition + uint64_t(sec) * kXisoSector] = data;
        entries.push_back({name, sec, uint32_t(data.size()), false});
      }
    }
    std::vector<uint32_t> offsets;
    uint32_t o = 0;
    for (const E& e : entries) {
      offsets.push_back(o);
      o += (14 + uint32_t(e.name.size()) + 3) & ~3u;
    }
    const uint32_t table_sector = Alloc(o);
    std::string table;
    for (size_t i = 0; i < entries.size(); ++i) {
      const uint32_t child = i + 1 < entries.size() ? offsets[i + 1] / 4 : 0;
      const bool by_left = i % 2 == 1;
      std::string e = Le16(by_left ? child : 0) + Le16(by_left ? 0 : child) + Le32(entries[i].sector) +
                      Le32(entries[i].size) + std::string(1, char(entries[i].dir ? 0x10 : 0x80)) +
                      std::string(1, char(entries[i].name.size())) + entries[i].name;
      e.resize((e.size() + 3) & ~size_t(3), '\xFF');
      entry_at[prefix + entries[i].name] = partition + uint64_t(table_sector) * kXisoSector + table.size();
      table += e;
    }
    chunks[partition + uint64_t(table_sector) * kXisoSector] = table;
    return {table_sector, o};
  }
  void Build(const fs::path& out) {
    std::tie(root_sector, root_size) = Folder("");
    std::string d = "MICROSOFT*XBOX*MEDIA" + Le32(root_sector) + Le32(root_size) + std::string(8, '\0');
    d.resize(0x7EC, '\0');
    d += "MICROSOFT*XBOX*MEDIA";
    d.resize(kXisoSector, '\0');
    chunks[partition + 32 * kXisoSector] = d;
    const int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (const auto& [at, bytes] : chunks) REQUIRE(pwrite(fd, bytes.data(), bytes.size(), off_t(at)) == ssize_t(bytes.size()));
    REQUIRE(ftruncate(fd, off_t(partition + uint64_t(next + 1) * kXisoSector)) == 0);  // a whole last sector
    close(fd);
  }
};

inline void Patch(const fs::path& file, uint64_t at, const std::string& bytes) {
  const int fd = open(file.c_str(), O_WRONLY);
  REQUIRE(pwrite(fd, bytes.data(), bytes.size(), off_t(at)) == ssize_t(bytes.size()));
  close(fd);
}

}  // namespace kuf::setup::test
