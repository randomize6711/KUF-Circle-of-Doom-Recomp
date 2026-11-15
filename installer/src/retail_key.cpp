// The built-in retail XEX key (core/xex_decrypt.h): linked only into kuf-setup, kuf-launcher and the tests, never
// into the core library, so kuf-localbuild (in the game package) does not carry it. The bytes come from a header the
// build generates from the pinned SDK's git history (installer/CMakeLists.txt); they are used only if their SHA-256
// is the known one (the digest tools/package/audit_release.py also holds).
#include <cstring>

#include "core/sha256.h"
#include "core/xex_decrypt.h"
#include "kuf_retail_key.h"  // build folder only, never committed

namespace kuf::setup {
namespace {

bool BuiltInRetailKey(AesKey* out) {
#if KUF_SETUP_HAS_RETAIL_KEY
  constexpr char kRetailKeySha256[] = "a0bd3f5f56adf2b0238a2702c0c0cd8548fb34b3dc19345943dc011010723fe4";
  AesKey k;
  std::memcpy(k.data(), kRetailXexKeyBytes, 16);
  const bool ok = Sha256Hex(std::string_view(reinterpret_cast<const char*>(k.data()), k.size())) == kRetailKeySha256;
  if (ok) *out = k;
  k.fill(0);
  return ok;
#else
  (void)out;
  return false;
#endif
}

const bool g_registered = (SetRetailKeyProvider(&BuiltInRetailKey), true);

}  // namespace
}  // namespace kuf::setup
