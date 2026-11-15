// Decrypting a retail default.xex in the setup and the launchers (docs/RETAIL_FILES.md). Only they do
// this: the runtime and the code generator stay as they are (they accept only an unencrypted executable, patch 0019).
// The player's encrypted file is decrypted in memory, checked against the supported version's image SHA-256 and
// installed unencrypted, so the local build and every update work from it without any key.
//
// On by default and in every release (docs/RETAIL_FILES.md "Decision of 2026-11-08 (2)"): KUF_SETUP_RETAIL_DECRYPTION
// takes the key from the pinned SDK's own git history (installer/CMakeLists.txt) into a generated header for
// src/retail_key.cpp (linked only into the setup and the launchers), which uses it only if its SHA-256 is the known
// one. With it OFF the XEX routine below is not compiled (DecryptXexWithKey returns kNoKey; for its tests:
// KUF_SETUP_DECRYPTION_CODE_FOR_TESTS) and a retail dump needs a separate unencrypted default.xex.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace kuf::setup {

using AesKey = std::array<uint8_t, 16>;

#ifndef KUF_SETUP_XEX_DECRYPTION
#define KUF_SETUP_XEX_DECRYPTION 0
#endif
// Whether this build has the XEX decryption routine at all (installer/CMakeLists.txt).
inline constexpr bool kXexDecryptionBuilt = KUF_SETUP_XEX_DECRYPTION != 0;

// AES-128 (FIPS-197), one 16-byte block in place.
void AesEncryptBlock(const AesKey& key, uint8_t block[16]);
void AesDecryptBlock(const AesKey& key, uint8_t block[16]);
// AES-128-CBC decryption in place (`size` a multiple of 16), the IV updated as the chain continues.
void AesCbcDecrypt(const AesKey& key, uint8_t iv[16], uint8_t* data, size_t size);
void AesCbcEncrypt(const AesKey& key, uint8_t iv[16], uint8_t* data, size_t size);  // for tests

// True when this build carries the retail key (and it is the right one).
bool RetailDecryptionAvailable();

enum class XexDecrypt { kOk, kNotEncrypted, kUnsupported, kMalformed, kNoKey };
// Turns an encrypted XEX (encryption 1, compression none or basic) into the same XEX unencrypted: the session key from
// the header's file key (AES-ECB with the master key), the payload by AES-CBC with a zero IV, the encryption field set to
// 0. Nothing else changes. `data` is changed only on kOk.
XexDecrypt DecryptXexWithKey(std::string* data, const AesKey& master);
XexDecrypt DecryptRetailXex(std::string* data);  // with the built-in retail key

// Where the retail key comes from: src/retail_key.cpp, linked only into the setup program and its tests (not into
// kuf-localbuild, which ships in the game package), registers itself at start-up. Without a provider there is no key.
using RetailKeyProvider = bool (*)(AesKey* out);
void SetRetailKeyProvider(RetailKeyProvider provider);

// Tests only: the master key DecryptRetailXex uses (nullptr: the built-in one again).
void SetRetailKeyForTests(const AesKey* key);

}  // namespace kuf::setup
