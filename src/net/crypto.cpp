#include "net/crypto.h"

#include <cstring>

#include <sys/random.h>

namespace kuf::net {
namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256State {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t block[64] = {};
  size_t used = 0;
  uint64_t bits = 0;

  void Compress() {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = uint32_t(block[4 * i]) << 24 | uint32_t(block[4 * i + 1]) << 16 | uint32_t(block[4 * i + 2]) << 8 |
             uint32_t(block[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = k + s1 + ch + kRoundConstants[i] + w[i];
      const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      k = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
  }
  void Update(const uint8_t* data, size_t size) {
    bits += uint64_t(size) * 8;
    while (size > 0) {
      const size_t n = std::min(size, sizeof(block) - used);
      std::memcpy(block + used, data, n);
      used += n;
      data += n;
      size -= n;
      if (used == sizeof(block)) {
        Compress();
        used = 0;
      }
    }
  }
  Digest Finish() {
    const uint64_t total = bits;
    const uint8_t one = 0x80;
    Update(&one, 1);
    const uint8_t zero = 0;
    while (used != 56) Update(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i) length[i] = uint8_t(total >> (56 - 8 * i));
    Update(length, 8);
    Digest out;
    for (int i = 0; i < 8; ++i) {
      out[4 * i] = uint8_t(h[i] >> 24);
      out[4 * i + 1] = uint8_t(h[i] >> 16);
      out[4 * i + 2] = uint8_t(h[i] >> 8);
      out[4 * i + 3] = uint8_t(h[i]);
    }
    return out;
  }
};

}  // namespace

Digest Sha256(const uint8_t* data, size_t size) {
  Sha256State s;
  s.Update(data, size);
  return s.Finish();
}

Digest HmacSha256(const uint8_t* key, size_t key_size, const uint8_t* data, size_t size) {
  uint8_t k[64] = {};
  if (key_size > sizeof(k)) {
    const Digest d = Sha256(key, key_size);
    std::memcpy(k, d.data(), d.size());
  } else if (key_size) {
    std::memcpy(k, key, key_size);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  Sha256State inner;
  inner.Update(ipad, sizeof(ipad));
  inner.Update(data, size);
  const Digest ih = inner.Finish();
  Sha256State outer;
  outer.Update(opad, sizeof(opad));
  outer.Update(ih.data(), ih.size());
  return outer.Finish();
}

Digest Pbkdf2Sha256(std::string_view password, const uint8_t* salt, size_t salt_size, uint32_t iterations) {
  // One block (32 bytes of output): U1 = HMAC(P, S || INT(1)), Ui = HMAC(P, Ui-1), T = U1 ^ ... ^ Uc.
  Bytes first(salt, salt + salt_size);
  first.insert(first.end(), {0, 0, 0, 1});
  const auto* p = reinterpret_cast<const uint8_t*>(password.data());
  Digest u = HmacSha256(p, password.size(), first.data(), first.size());
  Digest t = u;
  for (uint32_t i = 1; i < iterations; ++i) {
    u = HmacSha256(p, password.size(), u.data(), u.size());
    for (size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
  }
  return t;
}

bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t size) {
  uint8_t diff = 0;
  for (size_t i = 0; i < size; ++i) diff |= uint8_t(a[i] ^ b[i]);
  return diff == 0;
}

bool RandomBytes(uint8_t* out, size_t size) {
  size_t done = 0;
  while (done < size) {
    const ssize_t n = getrandom(out + done, size - done, 0);
    if (n <= 0) return false;
    done += size_t(n);
  }
  return true;
}

std::string Hex(const uint8_t* data, size_t size) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 15];
  }
  return out;
}

}  // namespace kuf::net
