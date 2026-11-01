// Hashing for lobby passwords (docs/MULTIPLAYER.md "Passwords"): SHA-256 (FIPS 180-4), HMAC-SHA-256 (RFC 2104) and
// PBKDF2-HMAC-SHA-256 (RFC 8018), self-contained so the game gains no library dependency; checked against the
// standards' test vectors (tests/net). Random bytes from the kernel (getrandom).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kuf::net {

using Digest = std::array<uint8_t, 32>;
using Bytes = std::vector<uint8_t>;

Digest Sha256(const uint8_t* data, size_t size);
inline Digest Sha256(std::string_view s) { return Sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
Digest HmacSha256(const uint8_t* key, size_t key_size, const uint8_t* data, size_t size);
Digest Pbkdf2Sha256(std::string_view password, const uint8_t* salt, size_t salt_size, uint32_t iterations);

// True if equal, in time independent of where they differ.
bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t size);
// Cryptographically random bytes (the kernel's getrandom); false if unavailable.
bool RandomBytes(uint8_t* out, size_t size);

std::string Hex(const uint8_t* data, size_t size);
inline std::string Hex(const Digest& d) { return Hex(d.data(), d.size()); }

}  // namespace kuf::net
