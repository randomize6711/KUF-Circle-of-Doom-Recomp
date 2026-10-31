// Executes the game's own recompiled checksum routines (zlib 1.2.3 adler32/crc32 and the engine's table CRC)
// and compares them with independent host implementations over varied lengths and alignments. These
// functions exercise loads, unrolled loops, rotates, carries and the save/restore helpers.
//
// Addresses are for default.xex v1 (sha256 3ec406cc...29ff); see docs/ENGINE_ANALYSIS.md.

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "guest_env.h"

extern "C" void sub_8224C420(PPCContext&, uint8_t*);  // zlib adler32(adler, buf, len)
extern "C" void sub_8224C760(PPCContext&, uint8_t*);  // zlib crc32 (big-endian table path)(crc, buf, len)
extern "C" void sub_82269BE8(PPCContext&, uint8_t*);  // engine CRC(unused, buf, len), init 0, no xorout

namespace {

using kuf::test::GuestEnv;

uint32_t HostAdler32(uint32_t adler, const std::vector<uint8_t>& data) {
  uint32_t a = adler & 0xFFFF, b = adler >> 16;
  for (uint8_t byte : data) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

const std::array<uint32_t, 256>& CrcTable() {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  return table;
}

uint32_t HostCrc32Raw(uint32_t crc, const std::vector<uint8_t>& data) {
  for (uint8_t byte : data) crc = CrcTable()[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  return crc;
}

uint32_t HostZlibCrc32(uint32_t crc, const std::vector<uint8_t>& data) {
  return ~HostCrc32Raw(~crc, data);
}

std::vector<uint8_t> RandomBytes(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> v(n);
  for (auto& b : v) b = static_cast<uint8_t>(rng());
  return v;
}

}  // namespace

TEST_CASE("zlib adler32 matches host", "[image][zlib]") {
  auto& env = GuestEnv::Get();
  const size_t len = GENERATE(0, 1, 2, 15, 16, 17, 255, 5551, 5552, 5553, 65536, 200003);
  const uint32_t offset = GENERATE(0u, 1u, 3u);
  env.Reset();
  auto data = RandomBytes(len, static_cast<uint32_t>(len * 7 + offset));
  std::vector<uint8_t> padded(offset, 0xAA);
  padded.insert(padded.end(), data.begin(), data.end());
  uint32_t guest_buf = env.Put(padded) + offset;
  for (uint32_t seed : {1u, 0x12345678u}) {
    CAPTURE(len, offset, seed);
    uint32_t expected = len == 0 ? seed : HostAdler32(seed, data);
    REQUIRE(env.Call(sub_8224C420, seed, guest_buf, static_cast<uint32_t>(len)) == expected);
  }
}

TEST_CASE("zlib adler32 null buffer returns 1", "[image][zlib]") {
  REQUIRE(GuestEnv::Get().Call(sub_8224C420, 0xDEADBEEF, 0, 100) == 1);
}

TEST_CASE("zlib crc32 matches host", "[image][zlib]") {
  auto& env = GuestEnv::Get();
  const size_t len = GENERATE(0, 1, 3, 4, 5, 31, 32, 33, 1000, 65536, 100001);
  const uint32_t offset = GENERATE(0u, 1u, 2u, 3u);
  env.Reset();
  auto data = RandomBytes(len, static_cast<uint32_t>(len * 13 + offset));
  std::vector<uint8_t> padded(offset, 0x55);
  padded.insert(padded.end(), data.begin(), data.end());
  uint32_t guest_buf = env.Put(padded) + offset;
  for (uint32_t seed : {0u, 0xFFFFFFFFu, 0xCBF43926u}) {
    CAPTURE(len, offset, seed);
    REQUIRE(env.Call(sub_8224C760, seed, guest_buf, static_cast<uint32_t>(len)) == HostZlibCrc32(seed, data));
  }
}

TEST_CASE("zlib crc32 known vector", "[image][zlib]") {
  auto& env = GuestEnv::Get();
  env.Reset();
  const std::vector<uint8_t> check = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  REQUIRE(env.Call(sub_8224C760, 0, env.Put(check), 9) == 0xCBF43926u);
}

TEST_CASE("engine table CRC matches host", "[image][engine]") {
  auto& env = GuestEnv::Get();
  const size_t len = GENERATE(0, 1, 7, 64, 4097);
  env.Reset();
  auto data = RandomBytes(len, static_cast<uint32_t>(len + 99));
  uint32_t guest_buf = env.Put(data);
  CAPTURE(len);
  REQUIRE(env.Call(sub_82269BE8, 0xFFFFFFFF, guest_buf, static_cast<uint32_t>(len)) == HostCrc32Raw(0, data));
}
