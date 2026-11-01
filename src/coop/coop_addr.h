// Native co-op: the addresses and session data the game's Xbox LIVE code expects (docs/MULTIPLAYER.md "Co-op
// runtime"). Pure (no guest memory): tests/net.
//
// An instance's XNADDR is derived from its IPv4 address alone (ina = inaOnline = the address, abEnet and abOnline
// carry a "KUF" mark and the address), so any peer's XNADDR translates back to its address without a table, and
// its machine ID is unique per address. The game's fixed ports (TCP 1000, UDP 1001: privileged on Linux and shared
// by every instance on one machine) map to an unprivileged base. The session advertisement is what the host
// publishes through the lobby (opaque to the lobby layer): the session's ID, the host's XNADDR, the key exchange
// key, the slots and the game's QoS data.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kuf::coop {

constexpr size_t kXnAddrSize = 36;   // XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]
constexpr size_t kXnKidSize = 8;
constexpr size_t kXnKeySize = 16;
constexpr size_t kSessionInfoSize = kXnKidSize + kXnAddrSize + kXnKeySize;  // XSESSION_INFO (60)
constexpr uint16_t kGamePortFirst = 1000;  // the game's TCP 1000 and UDP 1001 (and room up to 1015)
constexpr uint16_t kGamePortCount = 16;
constexpr size_t kMaxQosData = 1024;

using XnAddr = std::array<uint8_t, kXnAddrSize>;
using XnKid = std::array<uint8_t, kXnKidSize>;
using XnKey = std::array<uint8_t, kXnKeySize>;

// `ipv4` in host byte order (127.0.0.10 is 0x7F00000A).
XnAddr MakeXnAddr(uint32_t ipv4);
// The address an XNADDR of this scheme stands for; nullopt for any other XNADDR.
std::optional<uint32_t> IpOfXnAddr(const uint8_t* xnaddr);
uint64_t MachineIdOf(uint32_t ipv4);

// A game port to the port actually used, and back; ports outside the game's range are left as they are.
uint16_t ToHostPort(uint16_t game_port, uint16_t base);
uint16_t ToGamePort(uint16_t host_port, uint16_t base);

// The identity of the instance at an address: a distinct XUID (offline range, "KUF" mark: every peer's XUID follows
// from its address, as its XNADDR and machine ID do) and a default name (at most 15 characters, as a gamertag).
uint64_t XuidOf(uint32_t ipv4);
std::string DefaultName(uint32_t ipv4);

std::optional<uint32_t> ParseIpv4(const std::string& text);  // "a.b.c.d" -> host byte order
std::string FormatIpv4(uint32_t ipv4);

struct SessionAd {
  XnKid id{};
  XnAddr host{};
  XnKey key{};
  uint32_t public_slots = 0, private_slots = 0, filled_public = 0, filled_private = 0;
  std::vector<uint8_t> qos;  // the host's XNetQosListen data (at most kMaxQosData bytes)
};
std::vector<uint8_t> EncodeAd(const SessionAd& ad);
std::optional<SessionAd> DecodeAd(const uint8_t* data, size_t size);

}  // namespace kuf::coop
