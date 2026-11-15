// ZIP reading for the Windows updater (M17G): the update package is the portable edition's ZIP. Only what the
// project's packages use: stored and deflated entries (RFC 1951 inflate here, bounds-checked), CRC-32 checked,
// no ZIP64, no encryption. Names are checked before anything is written: no absolute paths, drive letters,
// backslashes, "..", empty or control characters; sizes and the number of entries are limited.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kuf::setup::update {

// Raw deflate data -> bytes; false on damaged data or more output than `limit`.
bool Inflate(const uint8_t* data, size_t size, std::string* out, size_t limit);

uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0);

struct ZipEntry {
  std::string name;         // as stored ("/" separators); directories end in "/"
  uint32_t crc = 0;
  uint64_t packed = 0, size = 0;
  uint16_t method = 0;      // 0 stored, 8 deflated
  uint64_t local_offset = 0;
};

struct ZipLimits {
  uint64_t max_total = uint64_t(4) << 30;  // unpacked bytes
  uint64_t max_entry = uint64_t(2) << 30;
  size_t max_entries = 10000;
};

// The central directory of `file`; false (with *why) on a damaged, unsupported or unsafe archive.
bool ReadZipDirectory(const std::filesystem::path& file, std::vector<ZipEntry>* entries, std::string* why,
                      const ZipLimits& limits = {});

// Extracts the entries for which `map` returns a destination path (relative to `dest`; empty: skip). Every file is
// written to its final name only after its CRC checked. False (with *why) on any problem.
bool ExtractZip(const std::filesystem::path& file, const std::vector<ZipEntry>& entries,
                const std::function<std::string(const std::string& name)>& map, const std::filesystem::path& dest,
                std::string* why);

}  // namespace kuf::setup::update
