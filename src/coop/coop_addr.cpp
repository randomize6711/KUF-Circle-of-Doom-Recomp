#include "coop/coop_addr.h"

#include <cstdio>
#include <cstring>

namespace kuf::coop {
namespace {

constexpr uint8_t kMark[3] = {'K', 'U', 'F'};
constexpr uint32_t kAdMagic = 0x4B554641;  // "KUFA"

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

}  // namespace

XnAddr MakeXnAddr(uint32_t ipv4) {
  XnAddr a{};
  for (int i = 0; i < 4; ++i) {
    a[i] = uint8_t(ipv4 >> (24 - 8 * i));      // ina (network byte order, as the console stores it)
    a[4 + i] = a[i];                           // inaOnline
  }
  a[8] = uint8_t(kGamePortFirst >> 8);        // wPortOnline (big-endian)
  a[9] = uint8_t(kGamePortFirst);
  a[10] = 0x02;                                // abEnet: a locally administered address with the mark and ...
  std::memcpy(&a[11], kMark, 3);
  a[14] = a[2];                                // ... the address's low bytes
  a[15] = a[3];
  std::memcpy(&a[16], kMark, 3);               // abOnline: the mark and the whole address
  std::memcpy(&a[19], &a[0], 4);
  return a;
}

std::optional<uint32_t> IpOfXnAddr(const uint8_t* x) {
  if (x[10] != 0x02 || std::memcmp(&x[11], kMark, 3) != 0 || std::memcmp(&x[16], kMark, 3) != 0 ||
      std::memcmp(&x[19], &x[0], 4) != 0) {
    return std::nullopt;
  }
  return Get32(&x[0]);
}

uint64_t MachineIdOf(uint32_t ipv4) { return 0xFA4B554600000000ull | ipv4; }

uint16_t ToHostPort(uint16_t game_port, uint16_t base) {
  if (game_port >= kGamePortFirst && game_port < kGamePortFirst + kGamePortCount) {
    return uint16_t(base + (game_port - kGamePortFirst));
  }
  return game_port;
}

uint16_t ToGamePort(uint16_t host_port, uint16_t base) {
  if (host_port >= base && host_port < base + kGamePortCount) return uint16_t(kGamePortFirst + (host_port - base));
  return host_port;
}

uint64_t XuidOf(uint32_t ipv4) { return 0xE0004B0000000000ull | uint64_t(ipv4) << 8 | 1; }
std::string DefaultName(uint32_t ipv4) { return "Player " + std::to_string(ipv4 & 255); }

std::optional<uint32_t> ParseIpv4(const std::string& text) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char extra = 0;
  if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 ||
      d > 255) {
    return std::nullopt;
  }
  return a << 24 | b << 16 | c << 8 | d;
}

std::string FormatIpv4(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) +
         "." + std::to_string(ip & 255);
}

std::vector<uint8_t> EncodeAd(const SessionAd& ad) {
  std::vector<uint8_t> out;
  Put32(out, kAdMagic);
  out.insert(out.end(), ad.id.begin(), ad.id.end());
  out.insert(out.end(), ad.host.begin(), ad.host.end());
  out.insert(out.end(), ad.key.begin(), ad.key.end());
  Put32(out, ad.public_slots);
  Put32(out, ad.private_slots);
  Put32(out, ad.filled_public);
  Put32(out, ad.filled_private);
  const size_t q = std::min(ad.qos.size(), kMaxQosData);
  Put32(out, uint32_t(q));
  out.insert(out.end(), ad.qos.begin(), ad.qos.begin() + ptrdiff_t(q));
  return out;
}

std::optional<SessionAd> DecodeAd(const uint8_t* data, size_t size) {
  constexpr size_t kFixed = 4 + kSessionInfoSize + 5 * 4;
  if (size < kFixed || Get32(data) != kAdMagic) return std::nullopt;
  SessionAd ad;
  const uint8_t* p = data + 4;
  std::memcpy(ad.id.data(), p, kXnKidSize);
  p += kXnKidSize;
  std::memcpy(ad.host.data(), p, kXnAddrSize);
  p += kXnAddrSize;
  std::memcpy(ad.key.data(), p, kXnKeySize);
  p += kXnKeySize;
  ad.public_slots = Get32(p);
  ad.private_slots = Get32(p + 4);
  ad.filled_public = Get32(p + 8);
  ad.filled_private = Get32(p + 12);
  const uint32_t q = Get32(p + 16);
  p += 20;
  if (q > kMaxQosData || size != kFixed + q) return std::nullopt;
  if (ad.public_slots + ad.private_slots > 4 || ad.filled_public > ad.public_slots ||
      ad.filled_private > ad.private_slots || !IpOfXnAddr(ad.host.data())) {
    return std::nullopt;  // the game's 4 players; a host address of this scheme only
  }
  ad.qos.assign(p, p + q);
  return ad;
}

}  // namespace kuf::coop
