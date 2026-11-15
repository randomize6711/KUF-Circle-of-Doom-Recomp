// SHA-512 (FIPS 180-4), for Ed25519 and SSH signatures (the Windows updater, M17G).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kuf::setup::update {

class Sha512 {
 public:
  Sha512();
  void Update(const void* data, size_t size);
  std::array<uint8_t, 64> Final();

 private:
  void Block(const uint8_t* p);
  uint64_t h_[8];
  uint8_t buffer_[128];
  size_t used_ = 0;
  uint64_t total_ = 0;  // bytes
};

std::array<uint8_t, 64> Sha512Of(const void* data, size_t size);
inline std::array<uint8_t, 64> Sha512Of(const std::string& s) { return Sha512Of(s.data(), s.size()); }

}  // namespace kuf::setup::update
