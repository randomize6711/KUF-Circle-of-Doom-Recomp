#include "net/sock.h"

#include <cerrno>
#include <cstring>
#include <mutex>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kuf::net::sock {

#if defined(_WIN32)

bool Startup() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA data{};
    ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  return ok;
}

Socket Open(int type) {
  if (!Startup()) return kInvalid;
  return WSASocketW(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP, nullptr, 0,
                    WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

void Close(Socket& s) {
  if (s != kInvalid) closesocket(s);
  s = kInvalid;
}

void SetNonBlocking(Socket s) {
  u_long on = 1;
  ioctlsocket(s, FIONBIO, &on);
}

static void SetInt(Socket s, int level, int name, int value) {
  setsockopt(s, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
}

void SetListenAddressOptions(Socket s) { SetInt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1); }

long Send(Socket s, const void* data, size_t size) {
  const int n = send(s, static_cast<const char*>(data), int(size), 0);
  if (n >= 0) return n;
  return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
}

long Receive(Socket s, void* data, size_t size) {
  const int n = recv(s, static_cast<char*>(data), int(size), 0);
  if (n >= 0) return n;
  return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : -2;
}

long SendTo(Socket s, const void* data, size_t size, const sockaddr_in& to) {
  return sendto(s, static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

long ReceiveFrom(Socket s, void* data, size_t size, sockaddr_in* from) {
  AddrLen len = sizeof(*from);
  return recvfrom(s, static_cast<char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(from), &len);
}

int Poll(PollFd* fds, size_t count, int timeout_ms) { return WSAPoll(fds, ULONG(count), timeout_ms); }

bool ConnectStarted(Socket s, const sockaddr* addr, AddrLen len) {
  return connect(s, addr, len) == 0 || WSAGetLastError() == WSAEWOULDBLOCK;
}

std::string LastErrorText() {
  const int code = WSAGetLastError();
  char* text = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 DWORD(code), 0, reinterpret_cast<char*>(&text), 0, nullptr);
  std::string out = text ? text : "error " + std::to_string(code);
  if (text) LocalFree(text);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.')) out.pop_back();
  return out;
}

bool Waker::Open() {
  Close();
  read_ = sock::Open(SOCK_DGRAM);
  if (read_ == kInvalid) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(read_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    sock::Close(read_);
    return false;
  }
  SetNonBlocking(read_);
  return true;
}

void Waker::Close() { sock::Close(read_); }

void Waker::Signal() {
  if (read_ == kInvalid) return;
  sockaddr_in self{};
  AddrLen len = sizeof(self);
  if (getsockname(read_, reinterpret_cast<sockaddr*>(&self), &len) == 0) {
    sendto(read_, "x", 1, 0, reinterpret_cast<sockaddr*>(&self), len);
  }
}

void Waker::Drain() {
  char b[16];
  while (recv(read_, b, sizeof(b), 0) > 0) {
  }
}

#else  // POSIX

bool Startup() { return true; }

Socket Open(int type) { return socket(AF_INET, type | SOCK_CLOEXEC, 0); }

void Close(Socket& s) {
  if (s != kInvalid) close(s);
  s = kInvalid;
}

void SetNonBlocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }

static void SetInt(Socket s, int level, int name, int value) { setsockopt(s, level, name, &value, sizeof(value)); }

void SetListenAddressOptions(Socket s) { SetInt(s, SOL_SOCKET, SO_REUSEADDR, 1); }

long Send(Socket s, const void* data, size_t size) {
  const ssize_t n = send(s, data, size, MSG_NOSIGNAL);
  if (n >= 0) return long(n);
  return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

long Receive(Socket s, void* data, size_t size) {
  const ssize_t n = recv(s, data, size, 0);
  if (n >= 0) return long(n);
  return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

long SendTo(Socket s, const void* data, size_t size, const sockaddr_in& to) {
  return long(sendto(s, data, size, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)));
}

long ReceiveFrom(Socket s, void* data, size_t size, sockaddr_in* from) {
  AddrLen len = sizeof(*from);
  return long(recvfrom(s, data, size, 0, reinterpret_cast<sockaddr*>(from), &len));
}

int Poll(PollFd* fds, size_t count, int timeout_ms) { return poll(fds, nfds_t(count), timeout_ms); }

bool ConnectStarted(Socket s, const sockaddr* addr, AddrLen len) {
  return connect(s, addr, len) == 0 || errno == EINPROGRESS;
}

std::string LastErrorText() { return std::strerror(errno); }

bool Waker::Open() {
  Close();
  int fds[2];
  if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) return false;
  read_ = fds[0];
  write_ = fds[1];
  return true;
}

void Waker::Close() {
  sock::Close(read_);
  sock::Close(write_);
}

void Waker::Signal() {
  if (write_ != kInvalid) (void)!write(write_, "x", 1);
}

void Waker::Drain() {
  char b[16];
  while (read(read_, b, sizeof(b)) > 0) {
  }
}

#endif

void SetNoDelay(Socket s) { SetInt(s, IPPROTO_TCP, TCP_NODELAY, 1); }
void SetReuseAddress(Socket s) { SetInt(s, SOL_SOCKET, SO_REUSEADDR, 1); }
void SetBroadcast(Socket s) { SetInt(s, SOL_SOCKET, SO_BROADCAST, 1); }

int PendingError(Socket s) {
  int err = 0;
  AddrLen len = sizeof(err);
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0) return -1;
  return err;
}

}  // namespace kuf::net::sock
