// The lobby and co-op sockets on Linux and Windows (M17F): one small layer instead of #ifdefs in every file. Only
// what src/net and src/coop use: IPv4 TCP and UDP, non-blocking, poll. The protocol above it is byte-for-byte the
// same on both systems (src/net/protocol.cpp writes every field explicitly, big-endian), so a Linux and a Windows
// player meet in the same lobby.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace kuf::net::sock {

#if defined(_WIN32)
using Socket = SOCKET;
inline constexpr Socket kInvalid = INVALID_SOCKET;
using PollFd = WSAPOLLFD;
using AddrLen = int;
#else
using Socket = int;
inline constexpr Socket kInvalid = -1;
using PollFd = pollfd;
using AddrLen = socklen_t;
#endif

// Winsock started once per process (no-op elsewhere); every function below calls it.
bool Startup();

// An IPv4 socket (SOCK_STREAM or SOCK_DGRAM), not inherited by child processes.
Socket Open(int type);
// Closes and sets it to kInvalid (kInvalid itself is ignored).
void Close(Socket& s);
void SetNonBlocking(Socket s);
void SetNoDelay(Socket s);
// A listening TCP socket that no other program can take over (Windows: SO_EXCLUSIVEADDRUSE, since its
// SO_REUSEADDR would let one; POSIX: SO_REUSEADDR, which only skips TIME_WAIT).
void SetListenAddressOptions(Socket s);
// A UDP socket several lobbies on one machine may bind (SO_REUSEADDR on both systems).
void SetReuseAddress(Socket s);
void SetBroadcast(Socket s);

// Sends what it can: > 0 bytes, 0 when it would block, < 0 on failure (never raises SIGPIPE).
long Send(Socket s, const void* data, size_t size);
// > 0 bytes, 0 when the peer closed, -1 when it would block, -2 on failure.
long Receive(Socket s, void* data, size_t size);
long SendTo(Socket s, const void* data, size_t size, const sockaddr_in& to);
long ReceiveFrom(Socket s, void* data, size_t size, sockaddr_in* from);
// poll() / WSAPoll(); the number of ready entries, or < 0.
int Poll(PollFd* fds, size_t count, int timeout_ms);
// A non-blocking connect: true when it completed or is under way.
bool ConnectStarted(Socket s, const sockaddr* addr, AddrLen len);
// The pending error of a socket (SO_ERROR); 0 when none.
int PendingError(Socket s);
// The last socket error, as text (for messages).
std::string LastErrorText();

// Wakes a thread waiting in Poll from another thread: a pipe on POSIX, a loopback UDP socket on Windows (WSAPoll
// takes sockets only).
class Waker {
 public:
  Waker() = default;
  ~Waker() { Close(); }
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;
  bool Open();
  void Close();
  void Signal();
  void Drain();
  // What to Poll for POLLIN.
  Socket Pollable() const { return read_; }

 private:
  Socket read_ = kInvalid;
#if !defined(_WIN32)
  Socket write_ = kInvalid;
#endif
};

}  // namespace kuf::net::sock
