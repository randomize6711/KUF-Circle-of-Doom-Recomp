#include "coop/coop_sock.h"

#include <algorithm>
#include <cerrno>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace kuf::coop {
namespace {

uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}

}  // namespace

uint32_t WsaErrorOf(int err) {
  switch (err) {
    case EINTR: return kWsaEIntr;
    case EBADF: return kWsaENotSock;
    case EACCES: case EPERM: return kWsaEAcces;
    case EFAULT: return kWsaEFault;
    case EINVAL: return kWsaEInval;
    case EMFILE: case ENFILE: return kWsaEMFile;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EAGAIN: case EINPROGRESS: return kWsaEWouldBlock;
    case EALREADY: return kWsaEAlready;
    case ENOTSOCK: return kWsaENotSock;
    case EMSGSIZE: return kWsaEMsgSize;
    case ENOPROTOOPT: return kWsaENoProtoOpt;
    case EADDRINUSE: return kWsaEAddrInUse;
    case EADDRNOTAVAIL: return kWsaEAddrNotAvail;
    case ENETDOWN: return kWsaENetDown;
    case ENETUNREACH: return kWsaENetUnreach;
    case ECONNABORTED: return kWsaEConnAborted;
    case ECONNRESET: return kWsaEConnReset;
    case ENOBUFS: case ENOMEM: return kWsaENoBufs;
    case EISCONN: return kWsaEIsConn;
    case ENOTCONN: return kWsaENotConn;
    case EPIPE: case ESHUTDOWN: return kWsaEShutdown;
    case ETIMEDOUT: return kWsaETimedOut;
    case ECONNREFUSED: return kWsaEConnRefused;
    case EHOSTUNREACH: case EHOSTDOWN: return kWsaEHostUnreach;
    default: return kWsaEInval;
  }
}

SockOpt MapSockOpt(uint32_t level, uint32_t name) {
  if (level == 0xFFFF) {  // SOL_SOCKET
    switch (name) {
      case 0x0004: return {SockOpt::kInt, SOL_SOCKET, SO_REUSEADDR};
      case 0x0008: return {SockOpt::kInt, SOL_SOCKET, SO_KEEPALIVE};
      case 0x0020: return {SockOpt::kInt, SOL_SOCKET, SO_BROADCAST};
      case 0x0080: return {SockOpt::kLinger, SOL_SOCKET, SO_LINGER};
      case 0x0100: return {SockOpt::kInt, SOL_SOCKET, SO_OOBINLINE};
      case 0x1001: return {SockOpt::kInt, SOL_SOCKET, SO_SNDBUF};
      case 0x1002: return {SockOpt::kInt, SOL_SOCKET, SO_RCVBUF};
      case 0x1005: return {SockOpt::kTimeout, SOL_SOCKET, SO_SNDTIMEO};
      case 0x1006: return {SockOpt::kTimeout, SOL_SOCKET, SO_RCVTIMEO};
      case 0xFFFFFF7F:  // SO_DONTLINGER: Linux's default
      case 0xFFFFFFFB:  // SO_EXCLUSIVEADDRUSE: Linux never shares a bound address without SO_REUSEADDR
      case 0x5801:      // the console's "insecure socket" options (no XNet encryption here)
      case 0x5802:
        return {SockOpt::kAccept, 0, 0};
      default: return {};
    }
  }
  if (level == 6 && name == 1) return {SockOpt::kInt, IPPROTO_TCP, TCP_NODELAY};
  if (level == 0 && name == 4) return {SockOpt::kInt, IPPROTO_IP, IP_TTL};  // Winsock's IP_TTL is 4
  return {};
}

std::optional<Addr4> ReadGuestSockaddr(const uint8_t* p, uint32_t len) {
  if (!p || len < 8 || (uint32_t(p[0]) << 8 | p[1]) != 2) return std::nullopt;
  return Addr4{Get32(p + 4), uint16_t(p[2] << 8 | p[3])};
}

void WriteGuestSockaddr(uint8_t* p, const Addr4& a) {
  std::fill(p, p + kGuestSockaddrSize, uint8_t(0));
  p[1] = 2;
  p[2] = uint8_t(a.port >> 8);
  p[3] = uint8_t(a.port);
  Put32(p + 4, a.ip);
}

std::vector<uint32_t> ReadFdSet(const uint8_t* p) {
  std::vector<uint32_t> out;
  const uint32_t n = std::min(Get32(p), kFdSetMax);
  for (uint32_t i = 0; i < n; ++i) out.push_back(Get32(p + 4 + 4 * i));
  return out;
}

void WriteFdSet(uint8_t* p, const std::vector<uint32_t>& handles) {
  const uint32_t n = uint32_t(std::min<size_t>(handles.size(), kFdSetMax));
  Put32(p, n);
  for (uint32_t i = 0; i < n; ++i) Put32(p + 4 + 4 * i, handles[i]);
}

int TimeoutMs(const uint8_t* tv) {
  if (!tv) return -1;
  const int64_t sec = int32_t(Get32(tv)), usec = int32_t(Get32(tv + 4));
  const int64_t ms = std::max<int64_t>(0, sec * 1000 + (usec + 999) / 1000);
  return int(std::min<int64_t>(ms, 0x7FFFFFFF));
}

}  // namespace kuf::coop
