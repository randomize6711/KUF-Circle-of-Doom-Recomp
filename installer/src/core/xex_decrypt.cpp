#include "core/xex_decrypt.h"

#include <cstring>
#include <mutex>
#include <utility>
#include <vector>


namespace kuf::setup {

namespace {

// ---- AES-128 (FIPS-197) -----------------------------------------------------------------------------------------

struct Tables {
  uint8_t sbox[256], inv[256], m9[256], m11[256], m13[256], m14[256];
};

uint8_t Mul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) {
    if (b & 1) r ^= a;
    a = uint8_t(a << 1 ^ (a & 0x80 ? 0x1B : 0));
    b >>= 1;
  }
  return r;
}

const Tables& T() {
  static const Tables t = [] {
    Tables x{};
    // The S-box from the multiplicative inverse in GF(2^8) and the affine map: p walks the powers of 3, q its inverse.
    uint8_t p = 1, q = 1;
    do {
      p = uint8_t(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
      q ^= uint8_t(q << 1);
      q ^= uint8_t(q << 2);
      q ^= uint8_t(q << 4);
      if (q & 0x80) q ^= 0x09;
      const auto rotl = [](uint8_t v, int s) { return uint8_t(v << s | v >> (8 - s)); };
      x.sbox[p] = uint8_t(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4) ^ 0x63);
    } while (p != 1);
    x.sbox[0] = 0x63;
    for (int i = 0; i < 256; ++i) {
      x.inv[x.sbox[i]] = uint8_t(i);
      x.m9[i] = Mul(uint8_t(i), 9);
      x.m11[i] = Mul(uint8_t(i), 11);
      x.m13[i] = Mul(uint8_t(i), 13);
      x.m14[i] = Mul(uint8_t(i), 14);
    }
    return x;
  }();
  return t;
}

struct Schedule {
  uint8_t rk[176];
  explicit Schedule(const AesKey& key) {
    const Tables& t = T();
    std::memcpy(rk, key.data(), 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t w[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
      if (i % 16 == 0) {
        const uint8_t first = w[0];
        w[0] = uint8_t(t.sbox[w[1]] ^ rcon);
        w[1] = t.sbox[w[2]];
        w[2] = t.sbox[w[3]];
        w[3] = t.sbox[first];
        rcon = uint8_t(rcon << 1 ^ (rcon & 0x80 ? 0x1B : 0));
      }
      for (int j = 0; j < 4; ++j) rk[i + j] = uint8_t(rk[i - 16 + j] ^ w[j]);
    }
  }
  ~Schedule() { volatile uint8_t* p = rk; for (size_t i = 0; i < sizeof(rk); ++i) p[i] = 0; }
};

void AddRoundKey(uint8_t s[16], const uint8_t* k) {
  for (int i = 0; i < 16; ++i) s[i] ^= k[i];
}

void Encrypt(const Schedule& ks, uint8_t s[16]) {
  const Tables& t = T();
  AddRoundKey(s, ks.rk);
  for (int round = 1; round <= 10; ++round) {
    uint8_t n[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) n[c * 4 + r] = t.sbox[s[((c + r) % 4) * 4 + r]];  // SubBytes, ShiftRows
    }
    if (round != 10) {
      for (int c = 0; c < 4; ++c) {  // MixColumns
        const uint8_t a0 = n[c * 4], a1 = n[c * 4 + 1], a2 = n[c * 4 + 2], a3 = n[c * 4 + 3];
        n[c * 4] = uint8_t(Mul(a0, 2) ^ Mul(a1, 3) ^ a2 ^ a3);
        n[c * 4 + 1] = uint8_t(a0 ^ Mul(a1, 2) ^ Mul(a2, 3) ^ a3);
        n[c * 4 + 2] = uint8_t(a0 ^ a1 ^ Mul(a2, 2) ^ Mul(a3, 3));
        n[c * 4 + 3] = uint8_t(Mul(a0, 3) ^ a1 ^ a2 ^ Mul(a3, 2));
      }
    }
    std::memcpy(s, n, 16);
    AddRoundKey(s, ks.rk + round * 16);
  }
}

void Decrypt(const Schedule& ks, uint8_t s[16]) {
  const Tables& t = T();
  AddRoundKey(s, ks.rk + 160);
  for (int round = 9; round >= 0; --round) {
    uint8_t n[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) n[((c + r) % 4) * 4 + r] = t.inv[s[c * 4 + r]];  // InvShiftRows, InvSubBytes
    }
    AddRoundKey(n, ks.rk + round * 16);
    if (round != 0) {
      for (int c = 0; c < 4; ++c) {  // InvMixColumns
        const uint8_t a0 = n[c * 4], a1 = n[c * 4 + 1], a2 = n[c * 4 + 2], a3 = n[c * 4 + 3];
        n[c * 4] = uint8_t(t.m14[a0] ^ t.m11[a1] ^ t.m13[a2] ^ t.m9[a3]);
        n[c * 4 + 1] = uint8_t(t.m9[a0] ^ t.m14[a1] ^ t.m11[a2] ^ t.m13[a3]);
        n[c * 4 + 2] = uint8_t(t.m13[a0] ^ t.m9[a1] ^ t.m14[a2] ^ t.m11[a3]);
        n[c * 4 + 3] = uint8_t(t.m11[a0] ^ t.m13[a1] ^ t.m9[a2] ^ t.m14[a3]);
      }
    }
    std::memcpy(s, n, 16);
  }
}

// ---- the retail key ---------------------------------------------------------------------------------------------

std::mutex g_mutex;
bool g_test_key_set = false;
AesKey g_test_key{};
RetailKeyProvider g_provider = nullptr;

bool MasterKey(AesKey* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_test_key_set) {
    *out = g_test_key;
    return true;
  }
  return g_provider && g_provider(out);
}

[[maybe_unused]] uint32_t Be32(const std::string& d, size_t o) {
  const auto* p = reinterpret_cast<const unsigned char*>(d.data()) + o;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}  // namespace

void AesEncryptBlock(const AesKey& key, uint8_t block[16]) { Encrypt(Schedule(key), block); }
void AesDecryptBlock(const AesKey& key, uint8_t block[16]) { Decrypt(Schedule(key), block); }

void AesCbcDecrypt(const AesKey& key, uint8_t iv[16], uint8_t* data, size_t size) {
  const Schedule ks(key);
  for (size_t o = 0; o + 16 <= size; o += 16) {
    uint8_t cipher[16];
    std::memcpy(cipher, data + o, 16);
    Decrypt(ks, data + o);
    for (int i = 0; i < 16; ++i) data[o + i] ^= iv[i];
    std::memcpy(iv, cipher, 16);
  }
}

void AesCbcEncrypt(const AesKey& key, uint8_t iv[16], uint8_t* data, size_t size) {
  const Schedule ks(key);
  for (size_t o = 0; o + 16 <= size; o += 16) {
    for (int i = 0; i < 16; ++i) data[o + i] ^= iv[i];
    Encrypt(ks, data + o);
    std::memcpy(iv, data + o, 16);
  }
}

bool RetailDecryptionAvailable() {
  if (!kXexDecryptionBuilt) return false;
  AesKey k;
  const bool ok = MasterKey(&k);
  k.fill(0);
  return ok;
}

void SetRetailKeyProvider(RetailKeyProvider provider) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_provider = provider;
}

void SetRetailKeyForTests(const AesKey* key) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_test_key_set = key != nullptr;
  g_test_key = key ? *key : AesKey{};
}

// The XEX2 layout as in core/game_data.cpp: the file format header (size, encryption, compression, then for basic
// compression (data, zeros) block sizes) and the security info, whose 16-byte file key is at offset 0x150.
XexDecrypt DecryptXexWithKey(std::string* data, const AesKey& master) {
#if !KUF_SETUP_XEX_DECRYPTION
  (void)data;
  (void)master;
  return XexDecrypt::kNoKey;
#else
  std::string& d = *data;
  if (d.size() < 24 || Be32(d, 0) != 0x58455832) return XexDecrypt::kMalformed;
  const uint64_t payload = Be32(d, 8), security = Be32(d, 16), count = Be32(d, 20);
  if (24 + count * 8 > d.size() || payload > d.size() || security + 0x160 > d.size()) return XexDecrypt::kMalformed;
  uint64_t format = 0;
  for (uint64_t i = 0; i < count; ++i) {
    if (Be32(d, 24 + i * 8) == 0x3FF) format = Be32(d, 28 + i * 8);
  }
  if (format == 0 || format + 8 > d.size()) return XexDecrypt::kMalformed;
  const uint64_t format_size = Be32(d, format);
  if (format_size < 8 || format + format_size > d.size()) return XexDecrypt::kMalformed;
  const uint32_t encryption = Be32(d, format + 4) >> 16, compression = Be32(d, format + 4) & 0xFFFF;
  if (encryption == 0) return XexDecrypt::kNotEncrypted;
  if (encryption != 1 || compression > 1) return XexDecrypt::kUnsupported;
  // The spans to decrypt, one CBC chain over them in order: the whole payload, or each block's data.
  std::vector<std::pair<uint64_t, uint64_t>> spans;
  if (compression == 0) {
    spans.push_back({payload, (d.size() - payload) / 16 * 16});
  } else {
    uint64_t from = payload;
    for (uint64_t o = format + 8; o + 8 <= format + format_size; o += 8) {
      const uint64_t size = Be32(d, o);
      if (size % 16 != 0 || from + size > d.size()) return XexDecrypt::kMalformed;
      spans.push_back({from, size});
      from += size;
    }
  }
  uint8_t session[16];
  std::memcpy(session, d.data() + security + 0x150, 16);
  AesDecryptBlock(master, session);
  AesKey session_key;
  std::memcpy(session_key.data(), session, 16);
  std::memset(session, 0, sizeof(session));
  std::string out = d;
  uint8_t iv[16] = {};
  for (const auto& [at, size] : spans) {
    AesCbcDecrypt(session_key, iv, reinterpret_cast<uint8_t*>(out.data()) + at, size);
  }
  session_key.fill(0);
  out[format + 4] = 0;  // encryption: none
  out[format + 5] = 0;
  d.swap(out);
  return XexDecrypt::kOk;
#endif
}

XexDecrypt DecryptRetailXex(std::string* data) {
  AesKey master;
  if (!MasterKey(&master)) return XexDecrypt::kNoKey;
  const XexDecrypt r = DecryptXexWithKey(data, master);
  master.fill(0);
  return r;
}

}  // namespace kuf::setup
