// Native co-op: the game's Winsock calls on Linux sockets (docs/MULTIPLAYER.md "Co-op runtime"). Pure (no guest
// memory): tests/net.
//
// The game uses the console's Winsock conventions: SOL_SOCKET is 0xFFFF and its option numbers are Winsock's,
// non-blocking mode is ioctlsocket(FIONBIO), errors come from WSAGetLastError() as WSAE* codes (a non-blocking call
// that cannot complete yet is WSAEWOULDBLOCK, also for a connect in progress), select() takes handle sets and ignores
// its first argument, and a failed connect shows in the exception set. Guest structures are big-endian.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace kuf::coop {

// Winsock error codes (as WSAGetLastError returns them on the console).
constexpr uint32_t kWsaEIntr = 10004, kWsaEBadF = 10009, kWsaEAcces = 10013, kWsaEFault = 10014,
                   kWsaEInval = 10022, kWsaEMFile = 10024, kWsaEWouldBlock = 10035, kWsaEInProgress = 10036,
                   kWsaEAlready = 10037, kWsaENotSock = 10038, kWsaEMsgSize = 10040, kWsaENoProtoOpt = 10042,
                   kWsaEAddrInUse = 10048, kWsaEAddrNotAvail = 10049, kWsaENetDown = 10050,
                   kWsaENetUnreach = 10051, kWsaEConnAborted = 10053, kWsaEConnReset = 10054,
                   kWsaENoBufs = 10055, kWsaEIsConn = 10056, kWsaENotConn = 10057, kWsaEShutdown = 10058,
                   kWsaETimedOut = 10060, kWsaEConnRefused = 10061, kWsaEHostUnreach = 10065;
// A Linux errno as the console's error (a connect in progress is WSAEWOULDBLOCK, as Winsock reports it).
uint32_t WsaErrorOf(int err);

// ioctlsocket commands.
constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;

// A socket option: what to do with the game's (level, name).
struct SockOpt {
  enum Kind { kInt, kLinger, kTimeout, kAccept, kUnknown } kind = kUnknown;  // kAccept: succeed, nothing to do
  int level = 0, name = 0;
};
SockOpt MapSockOpt(uint32_t level, uint32_t name);

// IPv4 socket addresses (host byte order here).
struct Addr4 {
  uint32_t ip = 0;
  uint16_t port = 0;
};
constexpr uint32_t kGuestSockaddrSize = 16;
// A guest sockaddr_in (family 2); nullopt for anything else or a short length.
std::optional<Addr4> ReadGuestSockaddr(const uint8_t* p, uint32_t len);
void WriteGuestSockaddr(uint8_t* p, const Addr4& a);  // 16 bytes
inline bool IsBroadcast(uint32_t ip) { return ip == 0xFFFFFFFFu; }

// Guest handle sets for select(): a count and up to 64 handles.
constexpr uint32_t kFdSetMax = 64;
std::vector<uint32_t> ReadFdSet(const uint8_t* p);
void WriteFdSet(uint8_t* p, const std::vector<uint32_t>& handles);
// A guest timeval as milliseconds for poll(); -1 (wait) for none.
int TimeoutMs(const uint8_t* timeval);

}  // namespace kuf::coop
