// SHA-256 (FIPS 180-4), small and dependency-free, for the setup core on every system (M16: no glib in the core).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kuf::setup {

class Sha256 {
 public:
  Sha256() { Reset(); }
  void Reset();
  void Update(const void* data, size_t size);
  void Update(std::string_view data) { Update(data.data(), data.size()); }
  // The digest as lower-case hex; the object must be Reset() before it is used again.
  std::string HexDigest();

 private:
  void Block(const uint8_t* p);
  std::array<uint32_t, 8> h_{};
  uint64_t length_ = 0;
  uint8_t buffer_[64] = {};
  size_t used_ = 0;
};

// SHA-256 of `data` as lower-case hex.
std::string Sha256Hex(std::string_view data);

}  // namespace kuf::setup
