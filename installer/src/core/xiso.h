// Xbox 360 disc images (docs/RETAIL_IMPORT_PLAN.md A3): the XDVDFS file system of the game partition, read without
// mounting and without any key (XDVDFS is not encrypted; only default.xex inside it may be). The setup's own reader,
// written from the format's public description: the volume descriptor at sector 32 of the partition ("MICROSOFT*XBOX*
// MEDIA" at both ends, then the root directory's sector and size, little-endian), and each directory a binary tree of
// entries (left and right offsets in 4-byte units, start sector, size, attributes, name). Every value read from the
// image is bounds-checked; a damaged or hostile image is refused, never followed out of range.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/util.h"

namespace kuf::setup {

constexpr uint64_t kXisoSector = 2048;
// Where the game partition starts: an extracted partition, XGD3, XGD2, XGD1 (the original Xbox layout).
constexpr uint64_t kXisoPartitionOffsets[] = {0, 0x2080000, 0xFD90000, 0x18300000};

struct IsoFile {
  std::string path;     // relative, '/'-separated, as on the disc
  uint64_t offset = 0;  // in the image file
  uint64_t size = 0;
};

enum class IsoStatus { kUnreadable, kNotXiso, kMalformed, kOk };
struct IsoImage {
  IsoStatus status = IsoStatus::kUnreadable;
  std::string error;           // for kMalformed: what was wrong
  uint64_t partition = 0;      // the game partition's offset
  std::vector<IsoFile> files;  // regular files only, in tree order
};

// Limits against damaged images.
constexpr int kXisoMaxDepth = 32;
constexpr size_t kXisoMaxEntries = 100000;
constexpr uint64_t kXisoMaxDirectoryBytes = uint64_t(16) << 20;

IsoImage ReadXiso(const fs::path& image);

// True if `path` is a regular file whose content is an XDVDFS image (the volume descriptor at one of the offsets).
bool IsXisoFile(const fs::path& path);

}  // namespace kuf::setup
