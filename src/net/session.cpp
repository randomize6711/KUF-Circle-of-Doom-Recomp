#include "net/session.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kuf::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPingEvery = std::chrono::milliseconds(2000);
constexpr int kMaxFailedPasswords = 5;                // per address
constexpr auto kFailedWindow = std::chrono::seconds(60);
constexpr size_t kMaxPending = 16;                    // connections not yet in the lobby

void NonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

// Sends everything or fails (the frames are small; a full send buffer is a dead peer).
bool SendAll(int fd, const Bytes& data) {
  size_t done = 0;
  for (int tries = 0; done < data.size() && tries < 200; ++tries) {
    const ssize_t n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
    if (n > 0) {
      done += size_t(n);
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd p{fd, POLLOUT, 0};
      poll(&p, 1, 10);
    } else {
      return false;
    }
  }
  return done == data.size();
}

// Reads what is available into `buffer`; false when the peer closed or failed.
bool ReadSome(int fd, Bytes* buffer) {
  uint8_t chunk[4096];
  for (;;) {
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n > 0) {
      buffer->insert(buffer->end(), chunk, chunk + n);
      if (buffer->size() > kMaxFrame * 4) return false;  // a flood
      continue;
    }
    if (n == 0) return false;
    return errno == EAGAIN || errno == EWOULDBLOCK;
  }
}

std::string AddressOf(const sockaddr_in& a) {
  char buf[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
  return buf;
}

// Lobby IDs fit in 32 bits: an invite code carries the whole ID (a private lobby is joined only with it).
uint64_t RandomId() {
  uint32_t v = 0;
  RandomBytes(reinterpret_cast<uint8_t*>(&v), sizeof(v));
  return v ? v : 1;
}

}  // namespace

// ---- host ---------------------------------------------------------------------------------------------------------

struct Host::Impl {
  struct Conn {
    int fd = -1;
    std::string address;
    Bytes in;
    enum Stage { kHello, kAuth, kMember } stage = kHello;
    Clock::time_point since = Clock::now(), last_heard = Clock::now(), last_ping = Clock::now();
    Hello hello;
    Nonce server_nonce{};
    uint8_t index = 0;
    std::string name;
  };

  HostConfig config;
  Digest key{};  // PBKDF2(password, salt): the password itself is not kept
  Salt salt{};
  bool has_password = false;
  LobbyInfo info;
  int listen_fd = -1, discovery_fd = -1, wake[2] = {-1, -1};
  std::vector<Conn> conns;
  std::map<std::string, std::vector<Clock::time_point>> failures;
  mutable std::mutex mutex;
  std::deque<Event> events;
  std::vector<Member> members;  // the guests in the lobby (mutex): read by State() from other threads
  std::vector<Event> later;     // join / leave events: posted once the member list is published
  Bytes session_data;           // the game's session data (mutex)
  bool session_dirty = false;   // to be sent to every member (mutex)
  std::thread thread;
  std::atomic<bool> running{false};
  bool start_requested = false;

  void Post(Event e) {
    std::lock_guard lock(mutex);
    if (events.size() < 256) events.push_back(std::move(e));
  }

  LobbyState StateLocked() const {
    LobbyState s;
    s.info = info;
    s.members.push_back({0, info.host});
    s.members.insert(s.members.end(), members.begin(), members.end());
    s.info.players = uint8_t(s.members.size());
    return s;
  }

  // The thread's view of the guests into the shared snapshot (after any change).
  void Publish() {
    std::vector<Member> now;
    for (const Conn& c : conns) {
      if (c.fd >= 0 && c.stage == Conn::kMember) now.push_back({c.index, c.name});
    }
    std::lock_guard lock(mutex);
    members = std::move(now);
    info.players = uint8_t(members.size() + 1);
  }

  void Broadcast(const Bytes& frame) {
    for (Conn& c : conns) {
      if (c.stage == Conn::kMember && !SendAll(c.fd, frame)) c.last_heard = Clock::time_point{};  // dropped next pass
    }
  }

  void BroadcastLobby() {
    Publish();
    LobbyState s;
    {
      std::lock_guard lock(mutex);
      s = StateLocked();
    }
    Broadcast(EncodeLobby(s));
  }

  void Close(Conn& c, std::optional<RejectReason> reason) {
    if (reason) SendAll(c.fd, EncodeReject(*reason));
    close(c.fd);
    c.fd = -1;
  }

  bool TooManyFailures(const std::string& address) {
    auto& f = failures[address];
    const auto now = Clock::now();
    std::erase_if(f, [&](Clock::time_point t) { return now - t > kFailedWindow; });
    return int(f.size()) >= kMaxFailedPasswords;
  }

  uint8_t FreeIndex() {
    for (uint8_t i = 1; i < kMaxPlayers; ++i) {
      if (std::none_of(conns.begin(), conns.end(), [&](const Conn& c) { return c.stage == Conn::kMember && c.index == i; })) {
        return i;
      }
    }
    return 0;
  }

  int Members() const {
    return 1 + int(std::count_if(conns.begin(), conns.end(), [](const Conn& c) { return c.stage == Conn::kMember; }));
  }

  void Handle(Conn& c, const Message& m) {
    c.last_heard = Clock::now();
    switch (c.stage) {
      case Conn::kHello: {
        if (m.type != MsgType::kHello) return Close(c, RejectReason::kMalformed);
        if (m.hello.version != kProtocolVersion) return Close(c, RejectReason::kVersion);
        if (info.started) return Close(c, RejectReason::kStarted);
        if (Members() >= info.max_players) return Close(c, RejectReason::kFull);
        // A private lobby is joined only with its ID (in its invite code); a public one also by address alone.
        if (m.hello.lobby_id != info.id && (info.visibility != Visibility::kPublic || m.hello.lobby_id != 0)) {
          return Close(c, info.visibility == Visibility::kPublic ? RejectReason::kClosed : RejectReason::kNotInvited);
        }
        if (TooManyFailures(c.address)) return Close(c, RejectReason::kPassword);
        c.hello = m.hello;
        RandomBytes(c.server_nonce.data(), c.server_nonce.size());
        Challenge ch;
        ch.lobby_id = info.id;
        ch.server_nonce = c.server_nonce;
        ch.salt = salt;
        ch.password = has_password;
        if (!SendAll(c.fd, EncodeChallenge(ch))) return Close(c, std::nullopt);
        c.stage = Conn::kAuth;
        return;
      }
      case Conn::kAuth: {
        if (m.type != MsgType::kAuth) return Close(c, RejectReason::kMalformed);
        const Digest want = GuestProof(key, c.hello.client_nonce, c.server_nonce, info.id);
        if (!ConstantTimeEqual(want.data(), m.auth.proof.data(), want.size())) {
          failures[c.address].push_back(Clock::now());
          return Close(c, RejectReason::kPassword);
        }
        if (Members() >= info.max_players) return Close(c, RejectReason::kFull);  // filled meanwhile
        Welcome w;
        w.proof = HostProof(key, c.hello.client_nonce, c.server_nonce, info.id);
        w.index = FreeIndex();
        if (w.index == 0) return Close(c, RejectReason::kFull);
        if (!SendAll(c.fd, EncodeWelcome(w))) return Close(c, std::nullopt);
        c.stage = Conn::kMember;
        c.index = w.index;
        c.name = c.hello.player;
        later.push_back({Event::kJoined, c.name});
        BroadcastLobby();
        Bytes data;
        {
          std::lock_guard lock(mutex);
          data = session_data;
        }
        if (!data.empty()) SendAll(c.fd, EncodeSession(data));
        return;
      }
      case Conn::kMember: {
        if (m.type == MsgType::kPing) {
          SendAll(c.fd, EncodeSimple(MsgType::kPong, m.ping.seq));
        } else if (m.type == MsgType::kLeave) {
          const std::string name = c.name;
          Close(c, std::nullopt);
          later.push_back({Event::kLeft, name});
        } else if (m.type != MsgType::kPong) {
          Close(c, RejectReason::kMalformed);  // guests send nothing else in the lobby
        }
        return;
      }
    }
  }

  void Discovery() {
    uint8_t buf[512];
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const ssize_t n = recvfrom(discovery_fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n <= 0) return;
    const auto nonce = DecodeDiscoveryQuery(buf, size_t(n));
    if (!nonce) return;
    LobbyInfo shown;
    {
      std::lock_guard lock(mutex);
      if (info.visibility != Visibility::kPublic || info.started) return;  // private and friends-only: never listed
      shown = StateLocked().info;
    }
    const Bytes answer = EncodeDiscoveryAnswer(*nonce, shown);
    sendto(discovery_fd, answer.data(), answer.size(), 0, reinterpret_cast<sockaddr*>(&from), len);
  }

  void Run() {
    while (running) {
      std::vector<pollfd> fds;
      fds.push_back({wake[0], POLLIN, 0});
      fds.push_back({listen_fd, POLLIN, 0});
      if (discovery_fd >= 0) fds.push_back({discovery_fd, POLLIN, 0});
      const size_t first_conn = fds.size();
      for (const Conn& c : conns) fds.push_back({c.fd, POLLIN, 0});
      poll(fds.data(), fds.size(), 200);
      if (fds[0].revents & POLLIN) {
        char b[16];
        while (read(wake[0], b, sizeof(b)) > 0) {
        }
      }
      if (fds[1].revents & POLLIN) {
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        const int fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&from), &len);
        if (fd >= 0) {
          const size_t pending = std::count_if(conns.begin(), conns.end(), [](const Conn& c) { return c.stage != Conn::kMember; });
          if (pending >= kMaxPending) {
            close(fd);
          } else {
            NonBlocking(fd);
            const int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            Conn c;
            c.fd = fd;
            c.address = AddressOf(from);
            conns.push_back(std::move(c));
          }
        }
      }
      if (discovery_fd >= 0 && (fds[2].revents & POLLIN)) Discovery();
      const auto now = Clock::now();
      bool changed = false;
      for (size_t i = 0; i < conns.size() && first_conn + i < fds.size(); ++i) {
        Conn& c = conns[i];
        if (c.fd < 0) continue;
        if (fds[first_conn + i].revents & (POLLIN | POLLHUP | POLLERR)) {
          std::vector<Message> msgs;
          const bool alive = ReadSome(c.fd, &c.in);
          if (!TakeFrames(&c.in, &msgs)) {
            const bool member = c.stage == Conn::kMember;
            Close(c, RejectReason::kMalformed);
            if (member) changed = true;
            continue;
          }
          for (const Message& m : msgs) {
            const bool was_member = c.stage == Conn::kMember;
            Handle(c, m);
            if (c.fd < 0) {
              changed |= was_member;
              break;
            }
          }
          if (!alive && c.fd >= 0) {
            const bool member = c.stage == Conn::kMember;
            const std::string name = c.name;
            Close(c, std::nullopt);
            if (member) {
              later.push_back({Event::kLeft, name});
              changed = true;
            }
          }
        }
        if (c.fd < 0) continue;
        const auto limit = c.stage == Conn::kMember ? config.idle_timeout : config.handshake_timeout;
        if (now - (c.stage == Conn::kMember ? c.last_heard : c.since) > limit) {
          const bool member = c.stage == Conn::kMember;
          const std::string name = c.name;
          Close(c, RejectReason::kTimeout);
          if (member) {
            later.push_back({Event::kLeft, name + " (timed out)"});
            changed = true;
          }
          continue;
        }
        if (c.stage == Conn::kMember && now - c.last_ping > kPingEvery) {
          c.last_ping = now;
          SendAll(c.fd, EncodeSimple(MsgType::kPing, 0));
        }
      }
      std::erase_if(conns, [](const Conn& c) { return c.fd < 0; });
      bool start = false;
      Bytes data;
      bool send_data = false;
      {
        std::lock_guard lock(mutex);
        start = start_requested;
        start_requested = false;
        send_data = session_dirty;
        session_dirty = false;
        if (send_data) data = session_data;
      }
      if (send_data) Broadcast(EncodeSession(data));
      if (start) {
        {
          std::lock_guard lock(mutex);
          info.started = true;
        }
        BroadcastLobby();
        Broadcast(EncodeSimple(MsgType::kStart));
      }
      if (changed) {
        BroadcastLobby();
        later.push_back({Event::kLobbyChanged, ""});
      }
      if (!later.empty()) {
        Publish();  // the list first, then the events that report it
        for (Event& e : later) Post(std::move(e));
        later.clear();
      }
    }
    for (Conn& c : conns) {
      if (c.fd >= 0) Close(c, RejectReason::kClosed);
    }
    conns.clear();
    Publish();
  }
};

Host::Host() : impl_(std::make_unique<Impl>()) {}
Host::~Host() { Stop(); }

bool Host::Start(const HostConfig& config, std::string* error) {
  Stop();
  Impl& d = *impl_;
  if (config.max_players < kMinPlayers || config.max_players > kMaxPlayers) {
    *error = "The game supports 2 to 4 players.";
    return false;
  }
  d.config = config;
  d.info = {};
  d.info.id = RandomId();
  d.info.name = CleanName(config.lobby_name, "Lobby");
  d.info.host = CleanName(config.host_name, "Host");
  d.info.difficulty = config.difficulty;
  d.info.max_players = config.max_players;
  d.info.visibility = config.visibility;
  d.info.backend = Backend::kDirect;
  d.has_password = !config.password.empty();
  d.info.password = d.has_password;
  RandomBytes(d.salt.data(), d.salt.size());
  d.key = PasswordKey(config.password, d.salt, kPbkdf2Iterations);  // also for "no password" (an empty one)
  d.config.password.assign(d.config.password.size(), '\0');
  d.config.password.clear();

  d.listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  const int one = 1;
  setsockopt(d.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(config.port);
  if (inet_pton(AF_INET, config.bind_address.c_str(), &addr.sin_addr) != 1 ||
      bind(d.listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(d.listen_fd, 8) != 0) {
    *error = "Could not open the lobby port " + std::to_string(config.port) + " (" + std::strerror(errno) + ").";
    close(d.listen_fd);
    d.listen_fd = -1;
    return false;
  }
  NonBlocking(d.listen_fd);
  socklen_t len = sizeof(addr);
  getsockname(d.listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
  d.info.port = ntohs(addr.sin_port);
  if (config.discovery_port) {
    d.discovery_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    setsockopt(d.discovery_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in da{};
    da.sin_family = AF_INET;
    da.sin_port = htons(config.discovery_port);
    inet_pton(AF_INET, config.bind_address.c_str(), &da.sin_addr);
    if (bind(d.discovery_fd, reinterpret_cast<sockaddr*>(&da), sizeof(da)) != 0) {
      close(d.discovery_fd);  // another lobby on this machine answers: this one is still joinable by address
      d.discovery_fd = -1;
    } else {
      NonBlocking(d.discovery_fd);
    }
  }
  if (pipe2(d.wake, O_NONBLOCK | O_CLOEXEC) != 0) {
    *error = "Could not start the lobby.";
    return false;
  }
  d.running = true;
  d.thread = std::thread([&d] { d.Run(); });
  return true;
}

void Host::Stop() {
  Impl& d = *impl_;
  if (d.running) {
    d.running = false;
    (void)!write(d.wake[1], "x", 1);
    if (d.thread.joinable()) d.thread.join();
  }
  for (int* fd : {&d.listen_fd, &d.discovery_fd, &d.wake[0], &d.wake[1]}) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
  }
}

bool Host::StartGame(std::string* error) {
  Impl& d = *impl_;
  std::lock_guard lock(d.mutex);
  if (!d.running) {
    *error = "The lobby is not open.";
    return false;
  }
  if (d.StateLocked().members.size() < 2) {
    *error = "At least one more player must join first.";
    return false;
  }
  d.start_requested = true;
  (void)!write(d.wake[1], "x", 1);
  return true;
}

LobbyState Host::State() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->StateLocked();
}

uint16_t Host::Port() const { return impl_->info.port; }

void Host::SetSessionData(const Bytes& data) {
  Impl& d = *impl_;
  {
    std::lock_guard lock(d.mutex);
    d.session_data.assign(data.begin(), data.begin() + ptrdiff_t(std::min(data.size(), kMaxSessionData)));
    d.session_dirty = true;
  }
  if (d.running) (void)!write(d.wake[1], "x", 1);
}

std::vector<Event> Host::TakeEvents() {
  std::lock_guard lock(impl_->mutex);
  std::vector<Event> out(impl_->events.begin(), impl_->events.end());
  impl_->events.clear();
  return out;
}

// ---- guest --------------------------------------------------------------------------------------------------------

struct Guest::Impl {
  mutable std::mutex mutex;
  std::deque<Event> events;
  std::optional<LobbyState> state;
  Bytes session_data;
  std::thread thread;
  std::atomic<bool> running{false}, joined{false};
  std::atomic<uint32_t> host_ipv4{0};  // the address the host was reached at (host byte order)
  int wake[2] = {-1, -1};

  void Post(Event e) {
    std::lock_guard lock(mutex);
    if (events.size() < 256) events.push_back(std::move(e));
  }

  void Run(JoinTarget target, std::string player, std::string password, std::chrono::milliseconds timeout) {
    auto fail = [&](Event::Kind kind, const std::string& text, RejectReason reason = RejectReason::kMalformed) {
      Post({kind, text, reason});
    };
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(target.address.c_str(), std::to_string(target.port).c_str(), &hints, &res) != 0 || !res) {
      return fail(Event::kError, "The host's address could not be found.");
    }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    NonBlocking(fd);
    const int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (res->ai_family == AF_INET) {
      host_ipv4 = ntohl(reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
    }
    freeaddrinfo(res);
    const auto deadline = Clock::now() + timeout;
    if (rc != 0 && errno != EINPROGRESS) {
      close(fd);
      return fail(Event::kError, "Could not connect to the host.");
    }
    {
      pollfd p{fd, POLLOUT, 0};
      const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
      int err = 0;
      socklen_t len = sizeof(err);
      if (poll(&p, 1, std::max(ms, 0)) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        close(fd);
        return fail(Event::kError, "Could not connect to the host (no answer: is the address right, and is the port "
                                   "reachable?).");
      }
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    Hello hello;
    hello.player = CleanName(player, "Player");
    hello.lobby_id = target.lobby_id;
    RandomBytes(hello.client_nonce.data(), hello.client_nonce.size());
    if (!SendAll(fd, EncodeHello(hello))) {
      close(fd);
      return fail(Event::kError, "The connection to the host failed.");
    }
    enum { kChallenge, kWelcome, kMember } stage = kChallenge;
    Bytes in;
    Digest key{};
    Nonce server_nonce{};
    uint64_t lobby_id = 0;
    auto last_heard = Clock::now();
    while (running) {
      pollfd fds[2] = {{fd, POLLIN, 0}, {wake[0], POLLIN, 0}};
      poll(fds, 2, 200);
      if (!running) break;
      if (stage != kMember && Clock::now() > deadline) {
        close(fd);
        return fail(Event::kRejected, RejectText(RejectReason::kTimeout), RejectReason::kTimeout);
      }
      if (stage == kMember && Clock::now() - last_heard > std::chrono::seconds(10)) {
        close(fd);
        joined = false;
        return fail(Event::kDisconnected, "The connection to the host was lost.");
      }
      if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const bool alive = ReadSome(fd, &in);
      std::vector<Message> msgs;
      if (!TakeFrames(&in, &msgs)) {
        close(fd);
        joined = false;
        return fail(Event::kDisconnected, "The host sent invalid data.");
      }
      for (const Message& m : msgs) {
        last_heard = Clock::now();
        if (m.type == MsgType::kReject) {
          close(fd);
          joined = false;
          return fail(stage == kMember ? Event::kDisconnected : Event::kRejected, RejectText(m.reject.reason),
                      m.reject.reason);
        }
        if (stage == kChallenge) {
          if (m.type != MsgType::kChallenge) break;
          server_nonce = m.challenge.server_nonce;
          // The lobby asked for (a code or the browser) must be the one answering; by address alone, whichever.
          if (target.lobby_id != 0 && m.challenge.lobby_id != target.lobby_id) {
            close(fd);
            return fail(Event::kRejected, "A different lobby answered at that address.", RejectReason::kClosed);
          }
          lobby_id = m.challenge.lobby_id;
          key = PasswordKey(m.challenge.password ? password : std::string(), m.challenge.salt, m.challenge.iterations);
          Auth auth;
          auth.proof = GuestProof(key, hello.client_nonce, server_nonce, lobby_id);
          SendAll(fd, EncodeAuth(auth));
          stage = kWelcome;
        } else if (stage == kWelcome) {
          if (m.type != MsgType::kWelcome) continue;
          // The host proves it knows the password too (a lobby impersonating another is refused).
          const Digest want = HostProof(key, hello.client_nonce, server_nonce, lobby_id);
          if (!ConstantTimeEqual(want.data(), m.welcome.proof.data(), want.size())) {
            close(fd);
            return fail(Event::kRejected, "The host could not prove it is that lobby.", RejectReason::kPassword);
          }
          stage = kMember;
          joined = true;
        } else if (m.type == MsgType::kLobby) {
          {
            std::lock_guard lock(mutex);
            const bool first = !state;
            state = m.lobby;
            if (first) events.push_back({Event::kJoined, m.lobby.info.name});
          }
          Post({Event::kLobbyChanged, ""});
        } else if (m.type == MsgType::kPing) {
          SendAll(fd, EncodeSimple(MsgType::kPong, m.ping.seq));
        } else if (m.type == MsgType::kStart) {
          Post({Event::kStarted, ""});
        } else if (m.type == MsgType::kSession) {
          {
            std::lock_guard lock(mutex);
            session_data = m.session;
          }
          Post({Event::kSessionData, ""});
        }
      }
      if (!alive) {
        close(fd);
        joined = false;
        return fail(Event::kDisconnected, "The host closed the connection.");
      }
    }
    SendAll(fd, EncodeSimple(MsgType::kLeave));
    close(fd);
    joined = false;
  }
};

Guest::Guest() : impl_(std::make_unique<Impl>()) {}
Guest::~Guest() { Leave(); }

void Guest::Join(const JoinTarget& target, const std::string& player, const std::string& password,
                 std::chrono::milliseconds timeout) {
  Leave();
  Impl& d = *impl_;
  if (pipe2(d.wake, O_NONBLOCK | O_CLOEXEC) != 0) return;
  d.running = true;
  {
    std::lock_guard lock(d.mutex);
    d.state.reset();
    d.session_data.clear();
  }
  d.host_ipv4 = 0;
  d.thread = std::thread([&d, target, player, password, timeout] { d.Run(target, player, password, timeout); });
}

void Guest::Leave() {
  Impl& d = *impl_;
  if (d.running) {
    d.running = false;
    (void)!write(d.wake[1], "x", 1);
  }
  if (d.thread.joinable()) d.thread.join();
  for (int* fd : {&d.wake[0], &d.wake[1]}) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
  }
}

bool Guest::Joined() const { return impl_->joined; }

uint32_t Guest::HostAddress() const { return impl_->host_ipv4; }

std::optional<LobbyState> Guest::State() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}

Bytes Guest::SessionData() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->session_data;
}

std::vector<Event> Guest::TakeEvents() {
  std::lock_guard lock(impl_->mutex);
  std::vector<Event> out(impl_->events.begin(), impl_->events.end());
  impl_->events.clear();
  return out;
}

// ---- discovery ----------------------------------------------------------------------------------------------------

std::vector<Discovered> Discover(const std::vector<std::string>& addresses, uint16_t port,
                                 std::chrono::milliseconds wait) {
  std::vector<Discovered> out;
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return out;
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  Nonce nonce{};
  RandomBytes(nonce.data(), nonce.size());
  const Bytes query = EncodeDiscoveryQuery(nonce);
  for (const std::string& a : addresses) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (inet_pton(AF_INET, a.c_str(), &to.sin_addr) != 1) continue;
    sendto(fd, query.data(), query.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
  }
  const auto deadline = Clock::now() + wait;
  while (Clock::now() < deadline && out.size() < 64) {
    pollfd p{fd, POLLIN, 0};
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
    if (poll(&p, 1, std::max(ms, 0)) != 1) break;
    uint8_t buf[512];
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n <= 0) continue;
    if (auto info = DecodeDiscoveryAnswer(buf, size_t(n), nonce)) {
      const std::string addr = AddressOf(from);
      if (std::none_of(out.begin(), out.end(), [&](const Discovered& d) { return d.info.id == info->id; })) {
        out.push_back({*info, addr});
      }
    }
  }
  close(fd);
  return out;
}

}  // namespace kuf::net
