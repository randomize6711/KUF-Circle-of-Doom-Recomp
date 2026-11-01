// Lobbies over direct connections (docs/MULTIPLAYER.md): a host listens (TCP) and answers LAN discovery (UDP); a
// guest connects by address, invite code or a discovered lobby. Each side runs its own thread; the game polls their
// events. The host decides who joins: protocol version, capacity, a private lobby's ID (from its invite code), the
// password proof (nothing the guest says is trusted otherwise); failed password attempts are limited per address.
// Nothing here opens router ports or changes firewall settings: hosting over the internet needs the player to forward
// the port themselves (explained in the interface).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/protocol.h"

namespace kuf::net {

constexpr uint16_t kDefaultLobbyPort = 36961;      // TCP
constexpr uint16_t kDefaultDiscoveryPort = 36962;  // UDP (LAN discovery)

struct HostConfig {
  std::string lobby_name;
  std::string host_name;
  uint8_t difficulty = 0;
  uint8_t max_players = kMaxPlayers;
  Visibility visibility = Visibility::kPublic;
  std::string password;               // empty: none (kept only as a derived key once started)
  std::string bind_address = "0.0.0.0";
  uint16_t port = kDefaultLobbyPort;  // 0: any free port (tests)
  uint16_t discovery_port = kDefaultDiscoveryPort;  // 0: no LAN discovery
  std::chrono::milliseconds handshake_timeout{10000};
  std::chrono::milliseconds idle_timeout{10000};
};

struct Event {
  enum Kind { kJoined, kLeft, kLobbyChanged, kStarted, kRejected, kDisconnected, kError, kSessionData } kind = kError;
  std::string text;         // a member's name / a reason
  RejectReason reason = RejectReason::kMalformed;
};

class Host {
 public:
  Host();
  ~Host();
  // Starts listening; false (with `error`) if the port is taken or the configuration is invalid.
  bool Start(const HostConfig& config, std::string* error);
  void Stop();  // closes the lobby: guests are told
  // Only the host starts the game; refused when fewer than two players are in.
  bool StartGame(std::string* error);
  LobbyState State() const;
  uint16_t Port() const;
  std::vector<Event> TakeEvents();
  // The game's session data (opaque here): sent to every guest now and to each later one when it joins.
  void SetSessionData(const Bytes& data);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct JoinTarget {
  std::string address;   // IPv4 address or host name
  uint16_t port = kDefaultLobbyPort;
  uint64_t lobby_id = 0; // from the browser or the invite code (a private lobby needs it)
};

class Guest {
 public:
  Guest();
  ~Guest();
  // Connects and joins in the background; events tell how it went (kJoined, kRejected, ...).
  void Join(const JoinTarget& target, const std::string& player, const std::string& password,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));
  void Leave();
  bool Joined() const;
  std::optional<LobbyState> State() const;
  std::vector<Event> TakeEvents();
  // The host's latest session data (empty until it sends some).
  Bytes SessionData() const;
  // The IPv4 address the host was reached at, host byte order (0 before connecting, or for IPv6).
  uint32_t HostAddress() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct Discovered {
  LobbyInfo info;
  std::string address;  // where it answered from
};
// Asks the given addresses (by default the local network's broadcast address) on the discovery port and collects
// the public lobbies that answer within `wait`.
std::vector<Discovered> Discover(const std::vector<std::string>& addresses, uint16_t port,
                                 std::chrono::milliseconds wait);

}  // namespace kuf::net
