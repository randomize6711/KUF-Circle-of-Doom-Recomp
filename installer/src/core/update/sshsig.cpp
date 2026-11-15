#include "core/update/sshsig.h"

#include <cstdint>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

#include "core/sha256.h"
#include "core/update/ed25519.h"
#include "core/update/sha512.h"

namespace kuf::setup::update {
namespace {

// The SSH wire format: big-endian uint32 lengths before each string.
struct Reader {
  const std::string& s;
  size_t at = 0;
  bool ok = true;
  uint32_t U32() {
    if (at + 4 > s.size()) {
      ok = false;
      return 0;
    }
    const uint32_t v = uint32_t(uint8_t(s[at])) << 24 | uint32_t(uint8_t(s[at + 1])) << 16 |
                       uint32_t(uint8_t(s[at + 2])) << 8 | uint8_t(s[at + 3]);
    at += 4;
    return v;
  }
  std::string String() {
    const uint32_t n = U32();
    if (!ok || n > s.size() - at) {
      ok = false;
      return {};
    }
    std::string out = s.substr(at, n);
    at += n;
    return out;
  }
  bool Done() const { return ok && at == s.size(); }
};

void PutString(std::string* out, const std::string& v) {
  const uint32_t n = uint32_t(v.size());
  for (int k = 3; k >= 0; --k) out->push_back(char((n >> (8 * k)) & 0xff));
  *out += v;
}

// An Ed25519 public key blob ("ssh-ed25519", 32 bytes) -> the 32 bytes.
bool Ed25519Key(const std::string& blob, std::string* key) {
  Reader r{blob};
  const std::string type = r.String();
  *key = r.String();
  return r.Done() && type == "ssh-ed25519" && key->size() == 32;
}

// The keys allowed_signers lists for `identity` with `ns` allowed (no namespaces= option: every namespace).
std::vector<std::string> AllowedKeys(const std::string& file, const std::string& identity, const std::string& ns) {
  std::vector<std::string> keys;
  std::istringstream lines(file);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream in(line);
    std::string principals;
    if (!(in >> principals) || principals[0] == '#') continue;
    bool match = false;
    std::istringstream ps(principals);
    for (std::string p; std::getline(ps, p, ',');) match |= p == identity;
    if (!match) continue;
    std::string word, namespaces;
    bool has_namespaces = false;
    // Options (namespaces="a,b", cert-authority, valid-after=...) come before the key type.
    while (in >> word && word.rfind("ssh-", 0) != 0) {
      if (word.rfind("namespaces=", 0) == 0) {
        has_namespaces = true;
        namespaces = word.substr(11);
        if (namespaces.size() >= 2 && namespaces.front() == '"' && namespaces.back() == '"') {
          namespaces = namespaces.substr(1, namespaces.size() - 2);
        }
      } else if (word == "cert-authority" || word.rfind("valid-", 0) == 0) {
        word.clear();  // certificates and validity windows are not used by the project: such a line is skipped
        break;
      }
    }
    if (word != "ssh-ed25519") continue;
    if (has_namespaces) {
      bool allowed = false;
      std::istringstream ns_list(namespaces);
      for (std::string n; std::getline(ns_list, n, ',');) allowed |= n == ns;
      if (!allowed) continue;
    }
    std::string b64, blob, key;
    if (in >> b64 && DecodeBase64(b64, &blob) && Ed25519Key(blob, &key)) keys.push_back(key);
  }
  return keys;
}

}  // namespace

bool DecodeBase64(const std::string& text, std::string* out) {
  out->clear();
  uint32_t acc = 0;
  int bits = 0, pad = 0;
  for (char ch : text) {
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
    int v;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+') v = 62;
    else if (ch == '/') v = 63;
    else if (ch == '=') {
      ++pad;
      continue;
    } else return false;
    if (pad) return false;  // data after padding
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(char((acc >> bits) & 0xff));
    }
  }
  return pad <= 2;
}

namespace {
// The digest of the signed data under the signature's hash ("sha512" or "sha256"); false if unknown or unreadable.
using DigestOf = std::function<bool(const std::string& hash, std::string* digest)>;
bool VerifyWith(const std::string& allowed_signers, const std::string& identity, const std::string& ns,
                const DigestOf& digest_of, const std::string& armored, std::string* why);
}  // namespace

bool VerifySshSignature(const std::string& allowed_signers, const std::string& identity, const std::string& ns,
                        const std::string& message, const std::string& armored, std::string* why) {
  return VerifyWith(allowed_signers, identity, ns, [&](const std::string& hash, std::string* digest) {
    if (hash == "sha512") {
      const auto d = Sha512Of(message);
      digest->assign(reinterpret_cast<const char*>(d.data()), d.size());
      return true;
    }
    if (hash == "sha256") {
      const std::string hex = Sha256Hex(message);
      for (size_t k = 0; k + 1 < hex.size(); k += 2) digest->push_back(char(std::stoi(hex.substr(k, 2), nullptr, 16)));
      return true;
    }
    return false;
  }, armored, why);
}

bool VerifySshSignatureFile(const std::string& allowed_signers, const std::string& identity, const std::string& ns,
                            const std::filesystem::path& file, const std::string& armored, std::string* why) {
  return VerifyWith(allowed_signers, identity, ns, [&](const std::string& hash, std::string* digest) {
    std::ifstream in(file, std::ios::binary);
    if (!in || (hash != "sha512" && hash != "sha256")) return false;
    Sha512 s512;
    Sha256 s256;
    std::vector<char> buf(1 << 16);
    while (in) {
      in.read(buf.data(), std::streamsize(buf.size()));
      const size_t n = size_t(in.gcount());
      if (hash == "sha512") s512.Update(buf.data(), n); else s256.Update(buf.data(), n);
    }
    if (hash == "sha512") {
      const auto d = s512.Final();
      digest->assign(reinterpret_cast<const char*>(d.data()), d.size());
    } else {
      const std::string hex = s256.HexDigest();
      for (size_t k = 0; k + 1 < hex.size(); k += 2) digest->push_back(char(std::stoi(hex.substr(k, 2), nullptr, 16)));
    }
    return true;
  }, armored, why);
}

namespace {
bool VerifyWith(const std::string& allowed_signers, const std::string& identity, const std::string& ns,
                const DigestOf& digest_of, const std::string& armored, std::string* why) {
  const auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  const std::string begin = "-----BEGIN SSH SIGNATURE-----", end = "-----END SSH SIGNATURE-----";
  const size_t b = armored.find(begin), e = armored.find(end);
  if (b == std::string::npos || e == std::string::npos || e < b) return fail("not an SSH signature file");
  std::string blob;
  if (!DecodeBase64(armored.substr(b + begin.size(), e - b - begin.size()), &blob)) {
    return fail("the signature's base64 is damaged");
  }
  if (blob.size() < 6 || blob.compare(0, 6, "SSHSIG") != 0) return fail("not an SSHSIG signature");
  Reader r{blob, 6};
  const uint32_t version = r.U32();
  const std::string public_key = r.String(), sig_ns = r.String(), reserved = r.String(), hash = r.String(),
                    signature = r.String();
  if (!r.Done() || version != 1) return fail("the signature's structure is damaged");
  if (sig_ns != ns) return fail("signed for '" + sig_ns + "', expected '" + ns + "'");
  std::string key;
  if (!Ed25519Key(public_key, &key)) return fail("not an Ed25519 signature");
  bool listed = false;
  for (const std::string& k : AllowedKeys(allowed_signers, identity, ns)) listed |= k == key;
  if (!listed) return fail("the signing key is not allowed for " + identity + " in " + ns);
  Reader sr{signature};
  const std::string sig_type = sr.String(), sig = sr.String();
  if (!sr.Done() || sig_type != "ssh-ed25519" || sig.size() != 64) return fail("the signature's structure is damaged");
  std::string digest;
  if (hash != "sha512" && hash != "sha256") return fail("unknown hash '" + hash + "'");
  if (!digest_of(hash, &digest)) return fail("cannot read the signed file");
  std::string signed_data = "SSHSIG";
  PutString(&signed_data, ns);
  PutString(&signed_data, reserved);
  PutString(&signed_data, hash);
  PutString(&signed_data, digest);
  if (!Ed25519Verify(reinterpret_cast<const uint8_t*>(sig.data()),
                     reinterpret_cast<const uint8_t*>(signed_data.data()), signed_data.size(),
                     reinterpret_cast<const uint8_t*>(key.data()))) {
    return fail("the signature does not match the file");
  }
  return true;
}

}  // namespace

}  // namespace kuf::setup::update
