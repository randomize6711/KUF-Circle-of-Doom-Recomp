#include "core/xiso.h"

#include "core/platform.h"

#include <cstring>
#include <set>
#include <string_view>

namespace kuf::setup {

namespace {
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMagicLen = 20;
constexpr uint64_t kDescriptorSector = 32;
constexpr size_t kMagicTail = 0x7EC;  // the magic again at the descriptor's end

uint32_t Le32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Le16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }

// Positioned reads of the image (core/platform.h: pread on Linux, ReadFile with an offset on Windows).
class Fd {
 public:
  explicit Fd(const fs::path& p) : file_(p) {}
  bool ok() const { return file_.ok(); }
  bool Read(uint64_t offset, void* out, size_t n) const { return file_.ReadAt(offset, out, n); }

 private:
  mutable InFile file_;
};

struct Descriptor {
  uint64_t partition = 0;
  uint32_t root_sector = 0;
  uint32_t root_size = 0;
};

bool FindDescriptor(const Fd& fd, uint64_t image_size, Descriptor* d) {
  unsigned char sector[kXisoSector];
  for (uint64_t part : kXisoPartitionOffsets) {
    const uint64_t at = part + kDescriptorSector * kXisoSector;
    if (at + kXisoSector > image_size || !fd.Read(at, sector, sizeof(sector))) continue;
    if (std::memcmp(sector, kMagic, kMagicLen) != 0 || std::memcmp(sector + kMagicTail, kMagic, kMagicLen) != 0) continue;
    d->partition = part;
    d->root_sector = Le32(sector + 20);
    d->root_size = Le32(sector + 24);
    return true;
  }
  return false;
}

bool ValidName(std::string_view n) {
  if (n.empty() || n == "." || n == "..") return false;
  for (char c : n) {
    if (c == '/' || c == '\\' || c == '\0') return false;
  }
  return true;
}
}  // namespace

bool IsXisoFile(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return false;
  const uint64_t size = fs::file_size(path, ec);
  if (ec) return false;
  Fd fd(path);
  Descriptor d;
  return fd.ok() && FindDescriptor(fd, size, &d);
}

IsoImage ReadXiso(const fs::path& image) {
  IsoImage out;
  std::error_code ec;
  const uint64_t size = fs::file_size(image, ec);
  Fd fd(image);
  if (ec || !fd.ok()) return out;
  Descriptor d;
  if (!FindDescriptor(fd, size, &d)) {
    out.status = IsoStatus::kNotXiso;
    return out;
  }
  out.partition = d.partition;
  auto fail = [&](std::string why) {
    out.status = IsoStatus::kMalformed;
    out.error = std::move(why);
    out.files.clear();
    return out;
  };

  struct Dir {
    uint32_t sector, size;
    std::string prefix;
    int depth;
  };
  std::vector<Dir> dirs{{d.root_sector, d.root_size, "", 0}};
  std::set<uint32_t> seen_dirs;
  std::set<std::string> seen_paths;
  size_t entries = 0;
  while (!dirs.empty()) {
    const Dir dir = dirs.back();
    dirs.pop_back();
    if (dir.depth > kXisoMaxDepth) return fail("folders nested too deeply");
    if (dir.size == 0) continue;  // an empty folder (its sector may be anything)
    if (!seen_dirs.insert(dir.sector).second) return fail("a folder appears twice (a loop)");
    if (dir.size > kXisoMaxDirectoryBytes) return fail("a folder table is too large");
    const uint64_t at = d.partition + uint64_t(dir.sector) * kXisoSector;
    if (at + dir.size > size) return fail("a folder table lies outside the image");
    std::vector<unsigned char> table(dir.size);
    if (!fd.Read(at, table.data(), table.size())) {
      out.status = IsoStatus::kUnreadable;
      return out;
    }
    if (table.size() >= 4 && Le16(&table[0]) == 0xFFFF && Le16(&table[2]) == 0xFFFF) continue;  // empty, padded
    std::vector<uint32_t> nodes{0};
    std::set<uint32_t> seen_nodes;
    while (!nodes.empty()) {
      const uint32_t o = nodes.back();
      nodes.pop_back();
      if (!seen_nodes.insert(o).second) return fail("a folder table refers to itself (a loop)");
      if (uint64_t(o) + 14 > table.size()) return fail("a folder entry lies outside its table");
      const unsigned char* e = &table[o];
      const uint16_t left = Le16(e), right = Le16(e + 2);
      const uint32_t sector = Le32(e + 4), fsize = Le32(e + 8);
      const uint8_t attributes = e[12], name_len = e[13];
      if (uint64_t(o) + 14 + name_len > table.size()) return fail("a file name lies outside its table");
      const std::string name(reinterpret_cast<const char*>(e + 14), name_len);
      if (!ValidName(name)) return fail("an invalid file name");
      if (++entries > kXisoMaxEntries) return fail("too many files");
      const std::string path = dir.prefix + name;
      std::string folded = path;
      for (char& c : folded) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
      if (!seen_paths.insert(folded).second) return fail("a file name appears twice: " + path);
      if (attributes & 0x10) {
        dirs.push_back({sector, fsize, path + "/", dir.depth + 1});
      } else {
        const uint64_t offset = d.partition + uint64_t(sector) * kXisoSector;
        if (offset + fsize > size) return fail("the file " + path + " lies outside the image");
        out.files.push_back({path, offset, fsize});
      }
      // 0 means no child (offset 0 is this tree's root); 0xFFFF is padding.
      if (right != 0 && right != 0xFFFF) nodes.push_back(uint32_t(right) * 4);
      if (left != 0 && left != 0xFFFF) nodes.push_back(uint32_t(left) * 4);
    }
  }
  out.status = IsoStatus::kOk;
  return out;
}

}  // namespace kuf::setup
