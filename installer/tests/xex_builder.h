// Encrypted test executables (installer/tests/test_retail.cpp, test_install.cpp): a small XEX with a security info
// block, encrypted with a random test key, never the retail one.
#pragma once

#include <cstring>
#include <string>

#include "core/xex_decrypt.h"

namespace kuf::setup::test {

constexpr uint32_t kTestTitle = 0x424507D1, kTestMedia = 0x7092B5EC, kTestVersion = 1;

inline AesKey KeyFromHex(const std::string& hex) {
  AesKey k{};
  for (size_t i = 0; i < 16; ++i) k[i] = uint8_t(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
  return k;
}
inline std::string Hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) s += {d[p[i] >> 4], d[p[i] & 15]};
  return s;
}
inline std::string Unhex(const std::string& hex) {
  std::string s;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) s += char(std::stoul(hex.substr(i, 2), nullptr, 16));
  return s;
}

// A small XEX with a security info block (its file key at 0x150) and basic compression in two blocks of 32 and 48
// bytes, plain or encrypted with `master` (a random test key, never the retail one).
inline std::string EncryptableXex(const std::string& image80, const AesKey* master, uint32_t title = kTestTitle) {
  const auto be32 = [](uint32_t v) { return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; };
  const uint32_t security = 48, format = security + 0x180, execution = format + 24, payload = execution + 24;
  std::string x = "XEX2" + be32(1) + be32(payload) + be32(0) + be32(security) + be32(3);
  x += be32(0x3FF) + be32(format) + be32(0x40006) + be32(execution);
  x += std::string(8, '\0');  // header padding to 48
  x.resize(security, '\0');
  std::string sec(0x180, '\0');
  const std::string size = be32(32 + 16 + 48);
  sec.replace(4, 4, size);
  const AesKey session = KeyFromHex("00112233445566778899aabbccddeeff");
  uint8_t file_key[16];
  std::memcpy(file_key, session.data(), 16);
  if (master) AesEncryptBlock(*master, file_key);
  sec.replace(0x150, 16, std::string(reinterpret_cast<char*>(file_key), 16));
  x += sec;
  x += be32(24) + std::string{0, char(master ? 1 : 0), 0, 1} + be32(32) + be32(16) + be32(48) + be32(0);
  x += be32(kTestMedia) + be32(kTestVersion) + be32(kTestVersion) + be32(title) + std::string{0, 0, 1, 1} + be32(0);
  std::string data = image80;
  if (master) {
    uint8_t iv[16] = {};
    AesCbcEncrypt(session, iv, reinterpret_cast<uint8_t*>(data.data()), data.size());
  }
  return x + data;
}

struct TestKey {
  explicit TestKey(const AesKey& k) { SetRetailKeyForTests(&k); }
  ~TestKey() { SetRetailKeyForTests(nullptr); }
};

inline const std::string kImage80 = std::string(32, 'A') + std::string(48, 'B').replace(5, 6, "secret");

}  // namespace kuf::setup::test
