// Ed25519 signature verification (RFC 8032), for the release signatures the Windows updater checks (M17G). Only
// verification: the project never signs on a player's PC. The arithmetic follows TweetNaCl (public domain); the
// curve constants are computed from their definitions at first use rather than written out.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kuf::setup::update {

// True when `signature` (64 bytes: R, S) is `public_key`'s (32 bytes) signature of the message. Refuses a
// non-canonical S (S >= the group order) and a public key that is not a curve point.
bool Ed25519Verify(const uint8_t signature[64], const uint8_t* message, size_t size, const uint8_t public_key[32]);

}  // namespace kuf::setup::update
