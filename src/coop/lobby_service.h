// The multiplayer lobby service (docs/MULTIPLAYER.md "Lobby service"): one interface between the menus, the lobby layer
// (src/net) and the co-op runtime, independent of any invitation backend.
//
//   menus (lobby_menu, the game type screen)  ->  LobbyService  ->  net::Host / net::Guest (the native lobby: joining,
//                                                                    password, capacity, the session data)
//                                                               ->  InviteProvider: Direct (LAN discovery, invite
//                                                                    codes) always; Steam only when configured
//   co-op runtime (coop_runtime.cpp)  <->  LobbyService: the role and addresses, the game's session advertisement
//
// Every way into a lobby ends in the same native join: a browser entry or a code is a net::JoinTarget; a Steam lobby
// (browsed or an accepted invitation) is entered on Steam and resolved to the owner's invite code (Steam member data)
// and then joined natively. Gameplay never goes through a provider. A new provider (EOS, Discord) is one more
// InviteProvider; the menus only see LobbyEntry, ProviderStatus and Snapshot.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/protocol.h"
#include "net/session.h"

namespace kuf::coop {

// Where an invite code's address can be reached from.
enum class Reach { kLoopback, kLan, kInternet };
Reach ReachOf(uint32_t ipv4);
const char* ReachText(Reach r);  // "this computer only" / "LAN only" / "internet (the host forwards its ports)"

// The address an invite code carries: the configured public address if any (internet), else the best local address
// (private ranges first). `local` are this machine's IPv4 addresses (host byte order); 0 when there is none.
uint32_t CodeAddress(const std::vector<uint32_t>& local, uint32_t public_address);
std::vector<uint32_t> LocalAddresses();  // getifaddrs: up, IPv4, not loopback

struct LobbySettings {
  std::string name;
  std::string player;  // the host's player name
  uint8_t difficulty = 0;
  uint8_t max_players = net::kMaxPlayers;  // 2-4
  net::Visibility visibility = net::Visibility::kPublic;
  std::string password;  // empty: none
  net::Backend backend = net::Backend::kDirect;  // where the lobby is also announced (Steam: when available)
};

// A lobby the browser can show and join. Only lobbies a working provider found: never a placeholder.
struct LobbyEntry {
  net::LobbyInfo info;
  net::Backend backend = net::Backend::kDirect;
  std::string address;        // direct: where it answered
  uint64_t provider_lobby = 0;  // Steam: its Steam lobby
  bool joinable = true;
  std::string reason;          // why not
};

struct ProviderStatus {
  net::Backend backend = net::Backend::kDirect;
  std::string name;
  bool available = false;
  std::string reason;      // why not (shown by the menus)
  bool can_invite = false;  // an invite dialog (friends) exists
};

struct ProviderEvent {
  enum Kind {
    kList,      // entries: this provider's lobbies (a refresh's answer)
    kResolved,  // code: the native lobby behind provider_lobby (enter succeeded)
    kInvite,    // provider_lobby: an invitation was accepted (the service enters and resolves it)
    kError,     // text
  } kind = kList;
  std::vector<LobbyEntry> entries;
  std::string code;
  uint64_t provider_lobby = 0;
  std::string text;
};

class InviteProvider {
 public:
  virtual ~InviteProvider() = default;
  virtual ProviderStatus Status() const = 0;
  // The host's lobby opened or changed (`code`: its native invite code) / closed.
  virtual void Announce(const LobbySettings& settings, const net::LobbyInfo& info, const std::string& code) = 0;
  virtual void Withdraw() = 0;
  virtual void RequestList() = 0;
  virtual bool OpenInviteDialog(std::string* error) = 0;
  // Enters a provider lobby to learn its native lobby (kResolved or kError).
  virtual void Resolve(uint64_t provider_lobby) = 0;
  virtual std::vector<ProviderEvent> Poll() = 0;
};

// LAN discovery (UDP broadcast, public lobbies only) and invite codes; always available. `addresses`: where to ask
// (by default the broadcast address; tests: loopback).
std::unique_ptr<InviteProvider> MakeDirectProvider(uint16_t discovery_port, std::chrono::milliseconds wait,
                                                   std::vector<std::string> addresses = {"255.255.255.255"});

struct ServiceConfig {
  std::string player = "Player";  // the player's name for joins that do not give one (accepted invitations)
  uint16_t lobby_port = net::kDefaultLobbyPort;
  uint16_t discovery_port = net::kDefaultDiscoveryPort;  // 0: no LAN discovery
  std::string bind_address = "0.0.0.0";
  uint32_t public_address = 0;  // the host's public IPv4 for internet codes (the player's own setting), 0: none
  uint32_t code_address = 0;    // overrides the address in codes (developer: loopback tests), 0: automatic
};

enum class Phase { kIdle, kHosting, kJoining, kInLobby, kInGame };
const char* PhaseText(Phase p);

struct Snapshot {
  Phase phase = Phase::kIdle;
  bool host = false;
  net::LobbyInfo info;
  std::vector<net::Member> members;
  std::string code;            // host: the invite code
  Reach reach = Reach::kLan;   // host: where the code works
  std::vector<LobbyEntry> lobbies;  // the last browse (all providers)
  bool refreshing = false;
  std::string error;           // the last failure, for the menus ("" none)
  std::vector<ProviderStatus> providers;
  bool session_ready = false;  // host: the game created its session; guest: the host's session arrived
};

class LobbyService {
 public:
  explicit LobbyService(ServiceConfig config);
  ~LobbyService();
  void AddProvider(std::unique_ptr<InviteProvider> provider);

  bool Create(const LobbySettings& settings, std::string* error);
  // A code "KUF-...", validated before anything is sent.
  bool JoinCode(const std::string& code, const std::string& player, const std::string& password, std::string* error);
  bool Join(const LobbyEntry& entry, const std::string& player, const std::string& password, std::string* error);
  void Leave();
  void Refresh();
  bool OpenInviteDialog(std::string* error);
  // Host: the game is started; refused unless the game's own session exists (session_ready) and a guest joined.
  bool StartGame(std::string* error);
  void Poll();  // the providers' and the lobby's events; call it every tick
  Snapshot State() const;

  // The co-op runtime's side.
  void SetSessionData(const net::Bytes& data);  // host: the game's session advertisement
  net::Bytes SessionData() const;              // guest: the host's
  uint32_t HostAddress() const;                // guest: where the host was reached (its game address)
  bool Active() const;                         // hosting, joining, in a lobby or in the game
  Phase CurrentPhase() const;
  bool IsHost() const;
  std::string LastError() const;

 private:
  void HandleProviderEvent(const ProviderEvent& e);
  void Fail(const std::string& text);
  ServiceConfig config_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<InviteProvider>> providers_;
  std::unique_ptr<net::Host> host_;
  std::unique_ptr<net::Guest> guest_;
  Phase phase_ = Phase::kIdle;
  LobbySettings settings_;
  std::string code_;
  Reach reach_ = Reach::kLan;
  std::vector<LobbyEntry> lobbies_;
  int refreshing_ = 0;
  std::string error_;
  bool session_ready_ = false;
  bool guest_joined_ = false;
  // An accepted invitation / Steam browser entry being resolved: then joined with these.
  uint64_t resolving_ = 0;
  std::string pending_player_, pending_password_;
};

}  // namespace kuf::coop
