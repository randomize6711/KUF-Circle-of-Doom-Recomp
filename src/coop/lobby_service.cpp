#include "coop/lobby_service.h"

#include <algorithm>
#include <atomic>

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "coop/coop_addr.h"

namespace kuf::coop {
namespace {

bool Private(uint32_t a) {
  return (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168) ||
         (a >> 16) == (169u << 8 | 254) || (a >> 22) == (100u << 2 | 1);  // 100.64/10 (carrier NAT)
}

// LAN discovery: a broadcast on the discovery port, answered by public lobbies that have not started.
class DirectProvider : public InviteProvider {
 public:
  DirectProvider(uint16_t port, std::chrono::milliseconds wait, std::vector<std::string> addresses)
      : port_(port), wait_(wait), addresses_(std::move(addresses)) {}
  ~DirectProvider() override {
    if (thread_.joinable()) thread_.join();
  }
  ProviderStatus Status() const override {
    ProviderStatus s;
    s.backend = net::Backend::kDirect;
    s.name = "LAN / direct";
    s.available = port_ != 0;
    s.reason = port_ ? "" : "LAN discovery is off.";
    s.can_invite = false;  // invite codes instead
    return s;
  }
  void Announce(const LobbySettings&, const net::LobbyInfo&, const std::string&) override {}  // the host answers
  void Withdraw() override {}
  void RequestList() override {
    if (busy_ || !port_) return;
    if (thread_.joinable()) thread_.join();
    busy_ = true;
    thread_ = std::thread([this] {
      const auto found = net::Discover(addresses_, port_, wait_);
      std::vector<LobbyEntry> entries;
      for (const net::Discovered& d : found) {
        LobbyEntry e;
        e.info = d.info;
        e.backend = net::Backend::kDirect;
        e.address = d.address;
        e.joinable = !d.info.started && d.info.players < d.info.max_players;
        e.reason = d.info.started ? "The game has started." : e.joinable ? "" : "The lobby is full.";
        entries.push_back(std::move(e));
      }
      std::lock_guard lock(mutex_);
      done_ = std::move(entries);
      ready_ = true;
      busy_ = false;
    });
  }
  bool OpenInviteDialog(std::string* error) override {
    *error = "Share the invite code instead (LAN and direct connections have no friends list).";
    return false;
  }
  void Resolve(uint64_t) override {}
  std::vector<ProviderEvent> Poll() override {
    std::lock_guard lock(mutex_);
    if (!ready_) return {};
    ready_ = false;
    ProviderEvent e;
    e.kind = ProviderEvent::kList;
    e.entries = std::move(done_);
    return {std::move(e)};
  }

 private:
  uint16_t port_;
  std::chrono::milliseconds wait_;
  std::vector<std::string> addresses_;
  std::thread thread_;
  std::atomic<bool> busy_{false};
  std::mutex mutex_;
  bool ready_ = false;
  std::vector<LobbyEntry> done_;
};

std::string Rejected(const net::Event& e) {
  if (e.reason == net::RejectReason::kNotInvited || e.reason == net::RejectReason::kClosed) {
    return "This invite code is no longer valid (the lobby was closed or replaced).";
  }
  return net::RejectText(e.reason);
}

}  // namespace

Reach ReachOf(uint32_t ipv4) {
  if ((ipv4 >> 24) == 127) return Reach::kLoopback;
  return Private(ipv4) ? Reach::kLan : Reach::kInternet;
}

const char* ReachText(Reach r) {
  switch (r) {
    case Reach::kLoopback: return "This computer only";
    case Reach::kLan: return "LAN only (the same network)";
    default: return "Internet (the host forwards its ports)";
  }
}

uint32_t CodeAddress(const std::vector<uint32_t>& local, uint32_t public_address) {
  if (public_address) return public_address;
  for (uint32_t a : local) {
    if (Private(a)) return a;
  }
  for (uint32_t a : local) {
    if ((a >> 24) != 127 && a != 0) return a;
  }
  return 0;
}

std::vector<uint32_t> LocalAddresses() {
  std::vector<uint32_t> out;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }
    out.push_back(ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr));
  }
  freeifaddrs(list);
  return out;
}

std::unique_ptr<InviteProvider> MakeDirectProvider(uint16_t discovery_port, std::chrono::milliseconds wait,
                                                   std::vector<std::string> addresses) {
  return std::make_unique<DirectProvider>(discovery_port, wait, std::move(addresses));
}

const char* PhaseText(Phase p) {
  switch (p) {
    case Phase::kHosting: return "hosting";
    case Phase::kJoining: return "joining";
    case Phase::kInLobby: return "in a lobby";
    case Phase::kInGame: return "in the game";
    default: return "idle";
  }
}

LobbyService::LobbyService(ServiceConfig config) : config_(std::move(config)) {}
LobbyService::~LobbyService() { Leave(); }

void LobbyService::AddProvider(std::unique_ptr<InviteProvider> provider) {
  std::lock_guard lock(mutex_);
  providers_.push_back(std::move(provider));
}

void LobbyService::Fail(const std::string& text) { error_ = text; }

bool LobbyService::Create(const LobbySettings& settings, std::string* error) {
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kIdle) {
    *error = "Leave the current lobby first.";
    return false;
  }
  if (settings.max_players < net::kMinPlayers || settings.max_players > net::kMaxPlayers) {
    *error = "A lobby has 2 to 4 players.";
    return false;
  }
  net::HostConfig hc;
  hc.lobby_name = settings.name;
  hc.host_name = settings.player;
  hc.difficulty = settings.difficulty;
  hc.max_players = settings.max_players;
  hc.visibility = settings.visibility;
  hc.password = settings.password;
  hc.bind_address = config_.bind_address;
  hc.port = config_.lobby_port;
  hc.discovery_port = config_.discovery_port;
  auto host = std::make_unique<net::Host>();
  if (!host->Start(hc, error)) return false;
  const uint32_t address = config_.code_address ? config_.code_address
                                                : CodeAddress(LocalAddresses(), config_.public_address);
  const net::LobbyInfo info = host->State().info;
  code_ = address ? net::FormatInviteCode({address, host->Port(), uint32_t(info.id)}) : "";
  reach_ = address ? ReachOf(address) : Reach::kLan;
  host_ = std::move(host);
  settings_ = settings;
  phase_ = Phase::kHosting;
  error_.clear();
  session_ready_ = false;
  for (auto& p : providers_) {
    if (p->Status().available && (p->Status().backend == settings.backend || p->Status().backend == net::Backend::kDirect)) {
      p->Announce(settings, info, code_);
    }
  }
  return true;
}

bool LobbyService::JoinCode(const std::string& code, const std::string& player, const std::string& password,
                            std::string* error) {
  const auto c = net::ParseInviteCode(code);
  if (!c || c->ipv4 == 0 || c->port == 0) {
    *error = "That is not a valid invite code (codes look like KUF-XXXX-XXXX-...; check the letters).";
    return false;
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kIdle) {
    *error = "Leave the current lobby first.";
    return false;
  }
  guest_ = std::make_unique<net::Guest>();
  guest_->Join({FormatIpv4(c->ipv4), c->port, c->lobby}, player, password);
  phase_ = Phase::kJoining;
  error_.clear();
  session_ready_ = false;
  guest_joined_ = false;
  return true;
}

bool LobbyService::Join(const LobbyEntry& entry, const std::string& player, const std::string& password,
                        std::string* error) {
  if (!entry.joinable) {
    *error = entry.reason.empty() ? "This lobby cannot be joined." : entry.reason;
    return false;
  }
  if (entry.backend == net::Backend::kDirect) {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kIdle) {
      *error = "Leave the current lobby first.";
      return false;
    }
    guest_ = std::make_unique<net::Guest>();
    guest_->Join({entry.address, entry.info.port, entry.info.id}, player, password);
    phase_ = Phase::kJoining;
    error_.clear();
    session_ready_ = false;
    guest_joined_ = false;
    return true;
  }
  std::lock_guard lock(mutex_);
  for (auto& p : providers_) {
    if (p->Status().backend == entry.backend && p->Status().available) {
      resolving_ = entry.provider_lobby;
      pending_player_ = player;
      pending_password_ = password;
      phase_ = Phase::kJoining;
      error_.clear();
      p->Resolve(entry.provider_lobby);
      return true;
    }
  }
  *error = "That lobby's service is not available.";
  return false;
}

void LobbyService::Leave() {
  std::lock_guard lock(mutex_);
  for (auto& p : providers_) p->Withdraw();
  if (host_) host_->Stop();
  if (guest_) guest_->Leave();
  host_.reset();
  guest_.reset();
  phase_ = Phase::kIdle;
  code_.clear();
  session_ready_ = false;
  guest_joined_ = false;
  resolving_ = 0;
}

void LobbyService::Refresh() {
  std::lock_guard lock(mutex_);
  lobbies_.clear();
  refreshing_ = 0;
  for (auto& p : providers_) {
    if (!p->Status().available) continue;
    p->RequestList();
    ++refreshing_;
  }
}

bool LobbyService::OpenInviteDialog(std::string* error) {
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kHosting) {
    *error = "Create a lobby first.";
    return false;
  }
  for (auto& p : providers_) {
    const ProviderStatus s = p->Status();
    if (s.available && s.can_invite && s.backend == settings_.backend) return p->OpenInviteDialog(error);
  }
  *error = "No friends list is available: share the invite code instead.";
  return false;
}

bool LobbyService::StartGame(std::string* error) {
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kHosting || !host_) {
    *error = "Only the host starts the game.";
    return false;
  }
  if (!session_ready_) {
    *error = "The game's co-op session is not ready yet (it was not created): the game cannot start.";
    return false;
  }
  if (!host_->StartGame(error)) return false;
  phase_ = Phase::kInGame;
  return true;
}

void LobbyService::HandleProviderEvent(const ProviderEvent& e) {
  switch (e.kind) {
    case ProviderEvent::kList:
      for (const LobbyEntry& l : e.entries) lobbies_.push_back(l);
      if (refreshing_ > 0) --refreshing_;
      break;
    case ProviderEvent::kResolved: {
      if (!resolving_ || e.provider_lobby != resolving_) break;
      resolving_ = 0;
      const auto c = net::ParseInviteCode(e.code);
      if (!c || !c->ipv4) {
        phase_ = Phase::kIdle;
        Fail("The lobby did not give a valid connection.");
        break;
      }
      guest_ = std::make_unique<net::Guest>();
      guest_->Join({FormatIpv4(c->ipv4), c->port, c->lobby}, pending_player_, pending_password_);
      guest_joined_ = false;
      session_ready_ = false;
      break;
    }
    case ProviderEvent::kInvite:
      if (phase_ == Phase::kInGame) {
        Fail("An invitation arrived during a game: leave the game to accept it.");
        break;
      }
      if (host_) host_->Stop();
      if (guest_) guest_->Leave();
      host_.reset();
      guest_.reset();
      for (auto& p : providers_) {
        const ProviderStatus s = p->Status();
        if (s.available && s.backend != net::Backend::kDirect) {
          resolving_ = e.provider_lobby;
          pending_player_ = config_.player;
          pending_password_.clear();  // a password still has to be given (the host's lobby asks)
          phase_ = Phase::kJoining;
          error_.clear();
          p->Resolve(e.provider_lobby);
          break;
        }
      }
      break;
    case ProviderEvent::kError:
      if (resolving_) {
        resolving_ = 0;
        phase_ = Phase::kIdle;
      }
      Fail(e.text);
      break;
  }
}

void LobbyService::Poll() {
  std::lock_guard lock(mutex_);
  for (auto& p : providers_) {
    for (const ProviderEvent& e : p->Poll()) HandleProviderEvent(e);
  }
  if (host_) {
    bool changed = false;
    for (const net::Event& e : host_->TakeEvents()) {
      changed |= e.kind == net::Event::kJoined || e.kind == net::Event::kLeft;
    }
    if (changed) {
      const net::LobbyInfo info = host_->State().info;
      for (auto& p : providers_) {
        if (p->Status().available) p->Announce(settings_, info, code_);
      }
    }
  }
  if (guest_) {
    for (const net::Event& e : guest_->TakeEvents()) {
      switch (e.kind) {
        case net::Event::kJoined:
          guest_joined_ = true;
          if (phase_ == Phase::kJoining) phase_ = Phase::kInLobby;
          break;
        case net::Event::kSessionData:
          session_ready_ = !guest_->SessionData().empty();
          break;
        case net::Event::kStarted:
          phase_ = Phase::kInGame;
          break;
        case net::Event::kRejected:
          phase_ = Phase::kIdle;
          Fail(Rejected(e));
          break;
        case net::Event::kDisconnected:
        case net::Event::kError:
          phase_ = Phase::kIdle;
          Fail(e.text.empty() ? "The connection to the host was lost." : e.text);
          break;
        default:
          break;
      }
    }
    if (phase_ == Phase::kIdle) guest_.reset();
  }
}

Snapshot LobbyService::State() const {
  std::lock_guard lock(mutex_);
  Snapshot s;
  s.phase = phase_;
  s.host = host_ != nullptr;
  if (host_) {
    const net::LobbyState st = host_->State();
    s.info = st.info;
    s.members = st.members;
  } else if (guest_) {
    if (const auto st = guest_->State()) {
      s.info = st->info;
      s.members = st->members;
    }
  }
  s.code = code_;
  s.reach = reach_;
  s.lobbies = lobbies_;
  s.refreshing = refreshing_ > 0;
  s.error = error_;
  for (const auto& p : providers_) s.providers.push_back(p->Status());
  s.session_ready = session_ready_;
  return s;
}

void LobbyService::SetSessionData(const net::Bytes& data) {
  std::lock_guard lock(mutex_);
  if (!host_) return;
  host_->SetSessionData(data);
  session_ready_ = !data.empty();
}

net::Bytes LobbyService::SessionData() const {
  std::lock_guard lock(mutex_);
  return guest_ ? guest_->SessionData() : net::Bytes{};
}

uint32_t LobbyService::HostAddress() const {
  std::lock_guard lock(mutex_);
  return guest_ ? guest_->HostAddress() : 0;
}

Phase LobbyService::CurrentPhase() const {
  std::lock_guard lock(mutex_);
  return phase_;
}

bool LobbyService::IsHost() const {
  std::lock_guard lock(mutex_);
  return host_ != nullptr;
}

std::string LobbyService::LastError() const {
  std::lock_guard lock(mutex_);
  return error_;
}

bool LobbyService::Active() const {
  std::lock_guard lock(mutex_);
  return phase_ != Phase::kIdle;
}

}  // namespace kuf::coop
