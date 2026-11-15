// SSH signatures (OpenSSH's PROTOCOL.sshsig, as `ssh-keygen -Y sign` makes them) checked against an allowed_signers
// file, the way `ssh-keygen -Y verify -f allowed_signers -I identity -n namespace -s file.sig` does: the releases'
// trust anchor (docs/RELEASING.md), for the Windows updater (M17G), where ssh-keygen is not available. Only
// Ed25519 keys (the project's release key is one).
#pragma once

#include <filesystem>
#include <string>

namespace kuf::setup::update {

// True when `armored` (the .sig file's text) is a valid signature of `message` under `ns` by a key that
// `allowed_signers` (the file's text) lists for `identity` with `ns` among its namespaces. *why says what failed.
bool VerifySshSignature(const std::string& allowed_signers, const std::string& identity, const std::string& ns,
                        const std::string& message, const std::string& armored, std::string* why);

// The same for a file, read in chunks (a package of any size; the signature covers its SHA-512 or SHA-256).
bool VerifySshSignatureFile(const std::string& allowed_signers, const std::string& identity, const std::string& ns,
                            const std::filesystem::path& file, const std::string& armored, std::string* why);

// Base64 (RFC 4648, with padding; whitespace ignored). False on anything else.
bool DecodeBase64(const std::string& text, std::string* out);

}  // namespace kuf::setup::update
