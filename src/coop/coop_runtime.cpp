// Native co-op runtime (docs/MULTIPLAYER.md "Co-op runtime"; developer configuration only, not shipped in menus).
//
// The game's own co-op code (Blueside network library, its session manager and replication) runs unchanged; this
// file answers the Xbox LIVE calls it makes:
//   - Addresses (XNetGetTitleXnAddr, XNetXnAddrToInAddr, XNetInAddrToXnAddr, XNetXnAddrToMachineId): an XNADDR is
//     derived from the instance's IPv4 address (coop_addr.h), so every conversion is a pure computation and the
//     "secure" IN_ADDR the game connects to is the peer's real address.
//   - Sessions (XMsgStartIORequest to XGI, app 0xFB): the host's XSessionCreate fills the session's ID, key, host
//     address and nonce and publishes them, with the host's XNetQosListen data, through the lobby layer (src/net)
//     as an advertisement; a guest's XSessionSearch / SearchByID return the advertisement it received, its
//     XNetQosLookup answers from it, and arbitration lists the machines this instance knows (host first).
//   - Identity (XamUserGetXUID, XamUserGetName, the LIVE sign-in bit, multiplayer privilege 254): a distinct XUID
//     and name per instance; the co-op gates pass. Nothing pretends to be signed in to Xbox LIVE elsewhere.
//   - Sockets (Linux): the game's Winsock calls on the native descriptors, with Winsock's conventions (coop_sock.h)
//     and its fixed ports 1000-1015 moved to kuf_coop_port_base; sockets are bound to kuf_coop_ip (instances on one
//     machine use distinct loopback addresses), and a broadcast is sent to each known peer instead.
// With kuf_coop empty every override forwards to the SDK's implementation (dlsym RTLD_NEXT), unchanged.
// Nothing here opens router ports, changes firewall settings or uses Xbox LIVE, Steam or any service.

#include "coop/coop_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "coop/coop_addr.h"
#include "coop/coop_xsession.h"
#include "coop/lobby_service.h"
#include "coop/steam_provider.h"
#include "runtime/lobby_screen.h"
#include "net/session.h"
#include "net/steam_lobby.h"

#if !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>

#include "coop/coop_sock.h"
#endif

REXCVAR_DEFINE_STRING(kuf_coop, "", "KUF/Co-op (developer)",
                      "Developer co-op test: host | join at startup without the menus (empty: off; the menus' Create "
                      "Lobby / Join do the same); see docs/MULTIPLAYER.md");
REXCVAR_DEFINE_STRING(kuf_coop_ip, "", "KUF/Co-op (developer)",
                      "This instance's IPv4 address, its sockets bound to it (instances on one machine: distinct "
                      "loopback addresses such as 127.0.0.10 and 127.0.0.11; empty: the machine's LAN address)");
REXCVAR_DEFINE_STRING(kuf_coop_peer, "", "KUF/Co-op (developer)", "join: the host's IPv4 address");
REXCVAR_DEFINE_INT32(kuf_coop_port_base, 37000, "KUF/Multiplayer",
                     "Where the game's ports 1000-1015 are moved (TCP 1000 to the base, UDP 1001 to base + 1, ...); "
                     "an internet host forwards the base and base + 1 (TCP and UDP) itself");
REXCVAR_DEFINE_INT32(kuf_coop_lobby_port, 36961, "KUF/Multiplayer", "The host's lobby port (TCP)");
REXCVAR_DEFINE_STRING(kuf_coop_name, "", "KUF/Multiplayer",
                      "The player name in lobbies (empty: Player <last address byte>)");
REXCVAR_DEFINE_BOOL(kuf_lan_discovery, true, "KUF/Multiplayer",
                    "Public lobbies answer and Browse Lobbies asks on the local network (UDP 36962)");
REXCVAR_DEFINE_STRING(kuf_public_address, "", "KUF/Multiplayer",
                      "Your public IPv4 address for internet invite codes (you forward the lobby and game ports "
                      "yourself; empty: codes are LAN codes)");
REXCVAR_DEFINE_STRING(kuf_steam_app_id, "", "KUF/Multiplayer",
                      "Steam invitations: an authorized App ID (empty: off; 480 Spacewar only for development tests)");
REXCVAR_DEFINE_STRING(kuf_steam_api_library, "", "KUF/Multiplayer",
                      "Steam invitations: the path of libsteam_api.so from your own Steamworks SDK");

#if !defined(_WIN32)

REX_EXTERN(__imp__sub_827E9128);
REX_EXTERN(__imp__sub_82668698);
REX_EXTERN(__imp__sub_82827E68);

namespace kuf::coop {
namespace {

constexpr uint32_t kXgiApp = 0xFB;
constexpr uint32_t kIoPending = 997;  // ERROR_IO_PENDING: the request completes through its XOVERLAPPED
constexpr uint32_t kActivePad = 0x82A803F0;  // the game's active controller (the co-op gates ask about it)
constexpr uint32_t kPrivilegeMultiplayer = 254;
constexpr uint32_t kXnAddrStatus = 0x02 | 0x04 | 0x20 | 0x40 | 0x80;  // Ethernet, static, gateway, DNS, online
constexpr uint16_t kNominalRttMs = 20;
constexpr auto kSearchWait = std::chrono::milliseconds(2000);  // a guest's search waits this long for the session
constexpr auto kRejoinDelay = std::chrono::seconds(5);         // developer join: retried while the host starts

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
void Store64(uint8_t* base, uint32_t a, uint64_t v) {
  Store32(base, a, uint32_t(v >> 32));
  Store32(base, a + 4, uint32_t(v));
}
uint16_t Load16(const uint8_t* base, uint32_t a) { return uint16_t(base[a] << 8 | base[a + 1]); }
uint32_t StackArg(const PPCContext& ctx, const uint8_t* base, int n) {  // argument n (from 0), n >= 8
  return Load32(base, ctx.r1.u32 + 0x54 + uint32_t(n - 8) * 8);
}

GuestImportFn Original(const char* symbol) {
  void* fn = dlsym(RTLD_NEXT, symbol);
  if (!fn) REXLOG_ERROR("[kuf-coop] the runtime has no {}", symbol);
  return reinterpret_cast<GuestImportFn>(fn);
}
void Call(GuestImportFn fn, PPCContext& ctx, uint8_t* base) {
  if (fn) fn(ctx, base);
}
// The SDK's implementation of an import (a void expression: `return KUF_FORWARD(...)`).
#define KUF_FORWARD(symbol) \
  Call([] { static const GuestImportFn fn = Original(symbol); return fn; }(), ctx, base)

enum class Role { kOff, kHost, kJoin };

// The co-op runtime's state. The role comes from the lobby service (the menus or the developer configuration): a
// host's or a guest's lobby makes co-op active; leaving it ends it.
struct Runtime {
  uint32_t ip = 0;           // this instance's address (its XNADDR, machine ID and XUID)
  bool bind_own = false;     // developer: sockets bound to `ip` (loopback addresses)
  uint16_t port_base = 37000;
  std::string name;
  bool dev_join = false;     // developer join: retried until the host's lobby answers
  net::JoinTarget dev_target;
  std::optional<std::chrono::steady_clock::time_point> rejoin_at;
  Phase last_phase = Phase::kIdle;
  std::string last_error;

  std::mutex mu;
  std::set<uint32_t> peers;          // addresses of the other instances seen (connect, accept, receive, sessions)
  std::map<uint32_t, uint32_t> alias;  // guest: the host's advertised address -> the address it was reached at
  std::optional<SessionAd> hosted;   // host: the session created, while it exists
  std::vector<uint8_t> qos;          // host: XNetQosListen data
  std::optional<SessionAd> joined;   // the session this instance joined or found
  std::unique_ptr<LobbyService> lobby;

  Runtime();
  Role role() const {
    const Phase p = lobby->CurrentPhase();
    if (p == Phase::kIdle) return Role::kOff;
    return lobby->IsHost() ? Role::kHost : Role::kJoin;
  }
  void Publish();  // host: the advertisement to the lobby (mu held)
  void Tick();     // the lobby's events; the developer join's retry
  std::optional<SessionAd> FoundSession(bool may_wait);
  std::vector<uint32_t> Machines();
  uint32_t Translate(uint32_t a) {  // mu held
    const auto it = alias.find(a);
    return it == alias.end() ? a : it->second;
  }
};

Runtime::Runtime() {
  const auto own = ParseIpv4(REXCVAR_GET(kuf_coop_ip));
  bind_own = own && *own != 0;
  ip = bind_own ? *own : CodeAddress(LocalAddresses(), 0);
  if (!ip) ip = *ParseIpv4("127.0.0.1");
  const int32_t pb = REXCVAR_GET(kuf_coop_port_base), lp = REXCVAR_GET(kuf_coop_lobby_port);
  port_base = uint16_t(pb >= 1024 && pb <= 65535 - kGamePortCount ? pb : 37000);
  name = REXCVAR_GET(kuf_coop_name);
  if (name.empty()) name = DefaultName(ip);
  name.resize(std::min<size_t>(name.size(), 15));

  ServiceConfig config;
  config.player = name;
  config.lobby_port = uint16_t(lp >= 1024 && lp <= 65535 ? lp : net::kDefaultLobbyPort);
  config.discovery_port = REXCVAR_GET(kuf_lan_discovery) && !bind_own ? net::kDefaultDiscoveryPort : 0;
  if (bind_own) {
    config.bind_address = FormatIpv4(ip);
    config.code_address = ip;
  }
  if (const auto pub = ParseIpv4(REXCVAR_GET(kuf_public_address))) config.public_address = *pub;
  lobby = std::make_unique<LobbyService>(config);
  lobby->AddProvider(MakeDirectProvider(config.discovery_port, std::chrono::milliseconds(800)));
  const auto app_id = net::steam::ParseAppId(REXCVAR_GET(kuf_steam_app_id));
  if (app_id && *app_id) {
    lobby->AddProvider(MakeSteamProvider(MakeSteamApi(*app_id, REXCVAR_GET(kuf_steam_api_library))));
    REXLOG_INFO("[kuf-coop] Steam invitations: App ID {} configured{}", *app_id,
                *app_id == net::steam::kSpacewarAppId ? " (480 Spacewar: development tests only)" : "");
  }

  const std::string mode = REXCVAR_GET(kuf_coop);
  if (mode.empty()) return;
  const auto host = ParseIpv4(REXCVAR_GET(kuf_coop_peer));
  if ((mode != "host" && mode != "join") || (mode == "join" && !host)) {
    REXLOG_ERROR("[kuf-coop] developer configuration refused (kuf_coop={}, kuf_coop_peer={})", mode,
                 REXCVAR_GET(kuf_coop_peer));
    return;
  }
  std::string error;
  if (mode == "host") {
    LobbySettings s;
    s.name = "KUF co-op test";
    s.player = name;
    s.max_players = 4;
    if (!lobby->Create(s, &error)) REXLOG_ERROR("[kuf-coop] host: the lobby could not start: {}", error);
  } else {
    dev_join = true;
    dev_target = {FormatIpv4(*host), config.lobby_port, 0};
    LobbyEntry e;
    e.address = dev_target.address;
    e.info.port = dev_target.port;
    lobby->Join(e, name, "", &error);
  }
  REXLOG_INFO("[kuf-coop] developer {} as {} at {} (XUID {:016X}); game ports {}-{} on {}-{}; lobby port {}", mode,
              name, FormatIpv4(ip), XuidOf(ip), kGamePortFirst, kGamePortFirst + kGamePortCount - 1, port_base,
              port_base + kGamePortCount - 1, config.lobby_port);
}

Runtime& R() {
  static Runtime* r = new Runtime();  // never destroyed: the lobby threads may outlive static destruction
  return *r;
}

void Runtime::Publish() {
  if (!lobby->IsHost()) return;
  if (!hosted) {
    lobby->SetSessionData({});
    return;
  }
  hosted->qos = qos;
  lobby->SetSessionData(EncodeAd(*hosted));
}

void Runtime::Tick() {
  lobby->Poll();
  const Phase p = lobby->CurrentPhase();
  const std::string error = lobby->LastError();
  if (p != last_phase) {
    REXLOG_INFO("[kuf-coop] lobby: {} -> {}", PhaseText(last_phase), PhaseText(p));
    if (p == Phase::kIdle) {
      std::lock_guard lock(mu);
      hosted.reset();
      joined.reset();
      alias.clear();
    }
    last_phase = p;
  }
  if (error != last_error) {
    if (!error.empty()) REXLOG_INFO("[kuf-coop] lobby: {}", error);
    last_error = error;
    if (dev_join && p == Phase::kIdle) rejoin_at = std::chrono::steady_clock::now() + kRejoinDelay;
  }
  if (rejoin_at && std::chrono::steady_clock::now() >= *rejoin_at) {
    rejoin_at.reset();
    REXLOG_INFO("[kuf-coop] guest: joining the host's lobby at {}:{} again", dev_target.address, dev_target.port);
    LobbyEntry e;
    e.address = dev_target.address;
    e.info.port = dev_target.port;
    std::string ignored;
    lobby->Join(e, name, "", &ignored);
  }
}

std::optional<SessionAd> Runtime::FoundSession(bool may_wait) {
  if (role() != Role::kJoin) return std::nullopt;
  const auto deadline = std::chrono::steady_clock::now() + kSearchWait;
  for (;;) {
    Tick();
    const net::Bytes data = lobby->SessionData();
    if (!data.empty() || !may_wait || std::chrono::steady_clock::now() >= deadline) {
      auto ad = DecodeAd(data.data(), data.size());
      if (ad) {
        // The host's game is where its lobby was reached (behind a NAT its own address is not): the session is
        // answered with that address, and the host's own address is translated to it.
        const auto advertised = IpOfXnAddr(ad->host.data());
        const uint32_t reached = lobby->HostAddress();
        std::lock_guard lock(mu);
        if (advertised && reached && *advertised != reached) {
          alias[*advertised] = reached;
          ad->host = MakeXnAddr(reached);
        }
        if (const auto h = IpOfXnAddr(ad->host.data())) peers.insert(*h);
      } else if (!data.empty()) {
        REXLOG_WARN("[kuf-coop] guest: the host's session data was not valid ({} bytes)", data.size());
      }
      return ad;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

std::vector<uint32_t> Runtime::Machines() {
  std::lock_guard lock(mu);
  std::vector<uint32_t> out;
  uint32_t host = role() == Role::kHost ? ip : lobby->HostAddress();
  if (joined) {
    if (const auto h = IpOfXnAddr(joined->host.data())) host = Translate(*h);
  }
  out.push_back(host);
  std::set<uint32_t> rest(peers.begin(), peers.end());
  rest.insert(ip);
  for (uint32_t a : rest) {
    if (a != host && out.size() < 4) out.push_back(a);
  }
  return out;
}

void NotePeer(uint32_t a) {
  Runtime& r = R();
  if (!a || a == r.ip || IsBroadcast(a) || (a >> 24) == 0) return;
  std::lock_guard lock(r.mu);
  if (r.peers.insert(a).second) REXLOG_INFO("[kuf-coop] peer {}", FormatIpv4(a));
}

// ---- Sessions (XGI messages) ----

SearchQuery QueryAt(const uint8_t* base, uint32_t buf) {
  return {Load16(base, buf + 12), Load32(base, buf + 16), Load16(base, buf + 14), Load32(base, buf + 20)};
}

uint32_t AnswerSearch(uint8_t* base, uint32_t pcb, uint32_t out, std::vector<SessionAd> ads, uint32_t max,
                      const SearchQuery& q) {
  if (!pcb) return kWsaEFault;
  if (ads.size() > max) ads.resize(max);
  const uint32_t need = SearchResultsSize(base, ads.size(), q);
  const uint32_t have = Load32(base, pcb);
  if (!out || have < need) {
    Store32(base, pcb, need);
    return kErrorInsufficientBuffer;
  }
  return WriteSearchResults(base, out, have, ads, q);
}

// The XGI session messages co-op answers; false: forwarded to the SDK (which succeeds and does nothing).
bool HandleSession(uint8_t* base, uint32_t msg, uint32_t buf, uint32_t* result) {
  Runtime& r = R();
  *result = 0;
  switch (msg) {
    case 0x000B0010: {  // XSessionCreate {obj, flags, public, private, user, pInfo, pNonce}
      const uint32_t flags = Load32(base, buf + 4), info = Load32(base, buf + 20), nonce = Load32(base, buf + 24);
      if (!(flags & 1)) {  // joining: the game copied the found session's info in
        if (info) {
          const SessionAd ad = ReadSessionInfo(base, info);
          if (const auto host = IpOfXnAddr(ad.host.data())) NotePeer(*host);
          std::lock_guard lock(r.mu);
          r.joined = ad;
        }
        REXLOG_INFO("[kuf-coop] XSessionCreate (join, flags {:08X})", flags);
        return false;
      }
      std::random_device rd;
      SessionAd ad;
      for (auto& b : ad.id) b = uint8_t(rd());
      ad.id[0] = uint8_t((ad.id[0] & 0x0F) | 0x80);  // an online peer session ID
      for (auto& b : ad.key) b = uint8_t(rd());
      ad.host = MakeXnAddr(r.ip);
      const uint32_t pub = Load32(base, buf + 8), priv = Load32(base, buf + 12);
      ad.public_slots = std::min<uint32_t>(pub, 4);
      ad.private_slots = std::min<uint32_t>(priv, 4 - ad.public_slots);
      ad.filled_public = ad.public_slots ? 1 : 0;
      ad.filled_private = ad.public_slots ? 0 : (ad.private_slots ? 1 : 0);
      if (info) WriteSessionInfo(base, info, ad);
      if (nonce) Store64(base, nonce, (uint64_t(rd()) << 32) | rd());
      std::lock_guard lock(r.mu);
      r.hosted = ad;
      r.joined = ad;
      r.Publish();
      REXLOG_INFO("[kuf-coop] XSessionCreate (host, flags {:08X}, {} public, {} private slots): published", flags,
                  pub, priv);
      return true;
    }
    case 0x000B0011: {  // XSessionDelete
      std::lock_guard lock(r.mu);
      if (r.hosted) {
        r.hosted.reset();
        r.Publish();
        REXLOG_INFO("[kuf-coop] XSessionDelete: the session is no longer advertised");
      }
      r.joined.reset();
      return false;
    }
    case 0x000B0016: {  // XSessionSearch {proc, user, max, nprops, nctx, props, ctx, pcb, results}
      const auto ad = r.FoundSession(true);
      std::vector<SessionAd> ads;
      if (ad) ads.push_back(*ad);
      *result = AnswerSearch(base, Load32(base, buf + 24), Load32(base, buf + 28), ads, Load32(base, buf + 8),
                             QueryAt(base, buf));
      REXLOG_INFO("[kuf-coop] XSessionSearch (procedure {}): {} session(s), result {}", Load32(base, buf),
                  ads.size(), *result);
      return true;
    }
    case 0x000B001B: {  // XSessionSearchByID {user, count, ids, pcb, results}
      const auto ad = r.FoundSession(true);
      std::vector<SessionAd> ads;
      const uint32_t n = Load32(base, buf + 4), ids = Load32(base, buf + 8);
      for (uint32_t i = 0; ad && ids && i < n && ads.empty(); ++i) {
        if (std::memcmp(base + ids + i * kXnKidSize, ad->id.data(), kXnKidSize) == 0) ads.push_back(*ad);
      }
      *result = AnswerSearch(base, Load32(base, buf + 12), Load32(base, buf + 16), ads, 1, SearchQuery{});
      REXLOG_INFO("[kuf-coop] XSessionSearchByID: {} session(s), result {}", ads.size(), *result);
      return true;
    }
    case 0x000B001A: {  // XSessionArbitrationRegister {obj, flags, nonce, duration, pcb, results}
      const uint32_t pcb = Load32(base, buf + 20), out = Load32(base, buf + 24);
      const std::vector<uint32_t> machines = r.Machines();
      const uint32_t need = RegistrationSize(machines.size());
      if (!pcb) {
        *result = kWsaEFault;
      } else if (!out || Load32(base, pcb) < need) {
        Store32(base, pcb, need);
        *result = kErrorInsufficientBuffer;
      } else {
        *result = WriteRegistration(base, out, Load32(base, pcb), machines);
      }
      REXLOG_INFO("[kuf-coop] XSessionArbitrationRegister: {} machine(s), result {}", machines.size(), *result);
      return true;
    }
    default:
      return false;
  }
}

void StartIORequest(PPCContext& ctx, uint8_t* base, const char* symbol) {
  const uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, overlapped = ctx.r5.u32, buf = ctx.r6.u32;
  uint32_t result = 0;
  if (!Enabled() || app != kXgiApp || !buf || !HandleSession(base, msg, buf, &result)) {
    if (Enabled() && app == kXgiApp) {
      REXLOG_INFO("[kuf-coop] XGI message {:08X} to the runtime", msg);
    }
    static const GuestImportFn sdk = Original("__imp__XMsgStartIORequest");
    static const GuestImportFn sdk_ex = Original("__imp__XMsgStartIORequestEx");
    const GuestImportFn fn = std::strcmp(symbol, "__imp__XMsgStartIORequestEx") == 0 ? sdk_ex : sdk;
    if (fn) fn(ctx, base);
    return;
  }
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, result);
    result = kIoPending;
  }
  if (result == 0 || result == kIoPending) rex::system::XThread::SetLastError(0);
  ctx.r3.u64 = result;
}

void XMsgStartIORequest(PPCContext& ctx, uint8_t* base) { StartIORequest(ctx, base, "__imp__XMsgStartIORequest"); }
void XMsgStartIORequestEx(PPCContext& ctx, uint8_t* base) {
  StartIORequest(ctx, base, "__imp__XMsgStartIORequestEx");
}

// ---- XNet ----

void XNetGetTitleXnAddr(PPCContext& ctx, uint8_t* base) {  // (caller, pxna)
  if (!Enabled()) return KUF_FORWARD("__imp__NetDll_XNetGetTitleXnAddr");
  const XnAddr a = MakeXnAddr(R().ip);
  std::memcpy(base + ctx.r4.u32, a.data(), a.size());
  ctx.r3.u64 = kXnAddrStatus;
}

void XNetXnAddrToInAddr(PPCContext& ctx, uint8_t* base) {  // (caller, pxna, pxnkid, pina)
  if (!Enabled()) return KUF_FORWARD("__imp__NetDll_XNetXnAddrToInAddr");
  const auto a = ctx.r4.u32 ? IpOfXnAddr(base + ctx.r4.u32) : std::nullopt;
  if (!a || !ctx.r6.u32) {
    REXLOG_WARN("[kuf-coop] XNetXnAddrToInAddr: not an address of this co-op scheme");
    ctx.r3.u64 = kWsaEInval;
    return;
  }
  uint32_t to;
  {
    Runtime& r = R();
    std::lock_guard lock(r.mu);
    to = r.Translate(*a);  // the host's own address -> where it was reached
  }
  Store32(base, ctx.r6.u32, to);
  NotePeer(to);
  ctx.r3.u64 = 0;
}

void XNetInAddrToXnAddr(PPCContext& ctx, uint8_t* base) {  // (caller, ina by value, pxna, pxnkid)
  if (!Enabled()) return KUF_FORWARD("__imp__NetDll_XNetInAddrToXnAddr");
  const uint32_t a = ctx.r4.u32;
  if (!a || !ctx.r5.u32) {
    ctx.r3.u64 = kWsaEInval;
    return;
  }
  const XnAddr x = MakeXnAddr(a);
  std::memcpy(base + ctx.r5.u32, x.data(), x.size());
  if (ctx.r6.u32) {
    Runtime& r = R();
    std::lock_guard lock(r.mu);
    const XnKid id = r.joined ? r.joined->id : XnKid{};
    std::memcpy(base + ctx.r6.u32, id.data(), id.size());
  }
  NotePeer(a);
  ctx.r3.u64 = 0;
}

void XNetXnAddrToMachineId(PPCContext& ctx, uint8_t* base) {  // (caller, pxna, pqwMachineId)
  if (!Enabled()) return KUF_FORWARD("__imp__NetDll_XNetXnAddrToMachineId");
  const auto a = ctx.r4.u32 ? IpOfXnAddr(base + ctx.r4.u32) : std::nullopt;
  if (!a || !ctx.r5.u32) {
    ctx.r3.u64 = kWsaEInval;
    return;
  }
  Store64(base, ctx.r5.u32, MachineIdOf(*a));
  ctx.r3.u64 = 0;
}

void XNetQosListen(PPCContext& ctx, uint8_t* base) {  // (caller, pxnkid, pb, cb, bitsPerSec, flags)
  if (!Enabled()) return KUF_FORWARD("__imp__NetDll_XNetQosListen");
  constexpr uint32_t kSetData = 4, kRelease = 0x10;
  const uint32_t pb = ctx.r5.u32, cb = ctx.r6.u32, flags = ctx.r8.u32;
  Runtime& r = R();
  std::lock_guard lock(r.mu);
  if (flags & kRelease) {
    r.qos.clear();
  } else if ((flags & kSetData) && pb) {
    r.qos.assign(base + pb, base + pb + std::min<uint32_t>(cb, kMaxQosData));
    if (cb > kMaxQosData) REXLOG_WARN("[kuf-coop] XNetQosListen: {} bytes of data, {} kept", cb, kMaxQosData);
  }
  r.Publish();
  REXLOG_INFO("[kuf-coop] XNetQosListen (flags {:X}): {} bytes of session data", flags, r.qos.size());
  ctx.r3.u64 = 0;
}

// (caller, cxna, apxna, apxnkid, apxnkey, cina, aina, aqwSvcId, cProbes, bitsPerSec, flags, hEvent, ppxnqos)
void XNetQosLookup(PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) return KUF_FORWARD("__imp__NetDll_XNetQosLookup");
  const uint32_t n = std::min<uint32_t>(ctx.r4.u32, 64), apxna = ctx.r5.u32;
  const uint32_t event = StackArg(ctx, base, 11), ppqos = StackArg(ctx, base, 12);
  std::vector<XnAddr> targets;
  for (uint32_t i = 0; apxna && i < n; ++i) {
    XnAddr t{};
    if (const uint32_t p = Load32(base, apxna + 4 * i)) std::memcpy(t.data(), base + p, t.size());
    targets.push_back(t);
  }
  std::vector<SessionAd> known;
  if (auto ad = R().FoundSession(false)) known.push_back(*ad);
  {
    Runtime& r = R();
    std::lock_guard lock(r.mu);
    if (r.hosted) known.push_back(*r.hosted);
  }
  if (!ppqos) {
    ctx.r3.u64 = kWsaEFault;
    return;
  }
  const uint32_t qos = REX_KERNEL_MEMORY()->SystemHeapAlloc(QosSize(targets, known));  // the SDK's release frees it
  if (!qos) {
    ctx.r3.u64 = kWsaENoBufs;
    return;
  }
  WriteQos(base, qos, targets, known, kNominalRttMs);
  Store32(base, ppqos, qos);
  if (event) {
    if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event)) ev->Set(0, false);
  }
  REXLOG_INFO("[kuf-coop] XNetQosLookup: {} target(s)", targets.size());
  ctx.r3.u64 = 0;
}

// ---- Identity ----

void XamUserGetXUID(PPCContext& ctx, uint8_t* base) {  // (user, type mask, pxuid)
  if (!Enabled() || ctx.r3.u32 != 0 || !ctx.r5.u32) return KUF_FORWARD("__imp__XamUserGetXUID");
  Store64(base, ctx.r5.u32, XuidOf(R().ip));
  ctx.r3.u64 = 0;
}

void XamUserGetName(PPCContext& ctx, uint8_t* base) {  // (user, buffer, length)
  if (!Enabled() || ctx.r3.u32 != 0 || !ctx.r4.u32 || !ctx.r5.u32) return KUF_FORWARD("__imp__XamUserGetName");
  const std::string& name = R().name;
  const uint32_t n = std::min<uint32_t>(uint32_t(name.size()), std::min<uint32_t>(ctx.r5.u32, 16) - 1);
  std::memcpy(base + ctx.r4.u32, name.data(), n);
  base[ctx.r4.u32 + n] = 0;
  ctx.r3.u64 = 0;
}

void XamUserCheckPrivilege(PPCContext& ctx, uint8_t* base) {  // (user, privilege, presult)
  if (!Enabled() || ctx.r4.u32 != kPrivilegeMultiplayer || !ctx.r5.u32 || (ctx.r3.u32 != 0 && ctx.r3.u32 != 0xFF)) {
    return KUF_FORWARD("__imp__XamUserCheckPrivilege");
  }
  Store32(base, ctx.r5.u32, 1);
  ctx.r3.u64 = 0;
}

// ---- Sockets ----

using rex::system::XSocket;

void Fail(uint32_t wsa) { rex::system::XThread::SetLastError(wsa); }
void FailErrno() { Fail(WsaErrorOf(errno)); }

int Fd(uint32_t handle) {
  auto s = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(handle);
  return s ? int(s->native_handle()) : -1;
}

sockaddr_in Native(uint32_t ip, uint16_t port) {
  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = htonl(ip);
  return sin;
}

// An unbound socket takes this instance's address first, so that peers see its connections and datagrams coming
// from it (on one machine, every loopback address would otherwise send from 127.0.0.1).
void BindOwnAddress(int fd) {
  if (!R().bind_own) return;
  sockaddr_in sin{};
  socklen_t len = sizeof(sin);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&sin), &len) == 0 && sin.sin_port != 0) return;
  const sockaddr_in own = Native(R().ip, 0);
  bind(fd, reinterpret_cast<const sockaddr*>(&own), sizeof(own));
}

int Flags(uint32_t guest_flags) {
  int f = 0;
  if (guest_flags & 1) f |= MSG_OOB;
  if (guest_flags & 2) f |= MSG_PEEK;
  if (guest_flags & 8) f |= MSG_WAITALL;
  return f;
}

#define KUF_SOCKET_OR_FORWARD(symbol, handle)              \
  if (!Enabled()) return KUF_FORWARD(symbol);              \
  const int fd = Fd(handle);                               \
  if (fd < 0) {                                            \
    Fail(kWsaENotSock);                                    \
    ctx.r3.u64 = uint64_t(-1);                             \
    return;                                                \
  }

void Bind(PPCContext& ctx, uint8_t* base) {  // (caller, s, name, namelen)
  KUF_SOCKET_OR_FORWARD("__imp__NetDll_bind", ctx.r4.u32);
  const auto a = ctx.r5.u32 ? ReadGuestSockaddr(base + ctx.r5.u32, ctx.r6.u32) : std::nullopt;
  if (!a) {
    Fail(kWsaEFault);
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  const Runtime& r = R();
  // Developer loopback instances bind their own address; otherwise the game's own choice (any address) stands.
  const sockaddr_in sin = Native(a->ip ? a->ip : r.bind_own ? r.ip : 0, ToHostPort(a->port, r.port_base));
  if (bind(fd, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin)) != 0) {
    REXLOG_WARN("[kuf-coop] bind {}:{} failed: {}", FormatIpv4(ntohl(sin.sin_addr.s_addr)), ntohs(sin.sin_port),
                   std::strerror(errno));
    FailErrno();
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  REXLOG_INFO("[kuf-coop] bound {}:{} (game port {})", FormatIpv4(ntohl(sin.sin_addr.s_addr)), ntohs(sin.sin_port),
              a->port);
  ctx.r3.u64 = 0;
}

void Connect(PPCContext& ctx, uint8_t* base) {  // (caller, s, name, namelen)
  KUF_SOCKET_OR_FORWARD("__imp__NetDll_connect", ctx.r4.u32);
  const auto a = ctx.r5.u32 ? ReadGuestSockaddr(base + ctx.r5.u32, ctx.r6.u32) : std::nullopt;
  if (!a) {
    Fail(kWsaEFault);
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  BindOwnAddress(fd);
  NotePeer(a->ip);
  const sockaddr_in sin = Native(a->ip, ToHostPort(a->port, R().port_base));
  REXLOG_INFO("[kuf-coop] connect to {}:{} (game port {})", FormatIpv4(a->ip), ntohs(sin.sin_port), a->port);
  if (connect(fd, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin)) != 0) {
    if (errno != EINPROGRESS && errno != EALREADY) {
      REXLOG_INFO("[kuf-coop] connect {}:{}: {}", FormatIpv4(a->ip), ntohs(sin.sin_port), std::strerror(errno));
    }
    FailErrno();
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  ctx.r3.u64 = 0;
}

void Listen(PPCContext& ctx, uint8_t* base) {  // (caller, s, backlog)
  KUF_SOCKET_OR_FORWARD("__imp__NetDll_listen", ctx.r4.u32);
  if (listen(fd, int(ctx.r5.u32)) != 0) {
    FailErrno();
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  ctx.r3.u64 = 0;
}

void Accept(PPCContext& ctx, uint8_t* base) {  // (caller, s, addr, addrlen)
  if (!Enabled()) return KUF_FORWARD("__imp__NetDll_accept");
  auto listener = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(ctx.r4.u32);
  if (!listener) {
    Fail(kWsaENotSock);
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  rex::system::N_XSOCKADDR scratch{};
  int scratch_len = sizeof(scratch);
  auto socket = listener->Accept(&scratch, &scratch_len);  // the SDK's object for the new descriptor
  if (!socket) {
    FailErrno();
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  const int fd = int(socket->native_handle());
  // Winsock gives the accepted socket the listener's blocking mode; Linux does not.
  if (fcntl(int(listener->native_handle()), F_GETFL) & O_NONBLOCK) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  }
  sockaddr_in from{};
  socklen_t len = sizeof(from);
  getpeername(fd, reinterpret_cast<sockaddr*>(&from), &len);
  const Addr4 a{ntohl(from.sin_addr.s_addr), ToGamePort(ntohs(from.sin_port), R().port_base)};
  NotePeer(a.ip);
  if (ctx.r5.u32 && ctx.r6.u32 && Load32(base, ctx.r6.u32) >= kGuestSockaddrSize) {
    WriteGuestSockaddr(base + ctx.r5.u32, a);
    Store32(base, ctx.r6.u32, kGuestSockaddrSize);
  }
  REXLOG_INFO("[kuf-coop] accepted {}:{}", FormatIpv4(a.ip), a.port);
  ctx.r3.u64 = socket->handle();
}

void Recv(PPCContext& ctx, uint8_t* base) {  // (caller, s, buf, len, flags)
  KUF_SOCKET_OR_FORWARD("__imp__NetDll_recv", ctx.r4.u32);
  const ssize_t n = recv(fd, base + ctx.r5.u32, ctx.r6.u32, Flags(ctx.r7.u32));
  if (n < 0) FailErrno();
  ctx.r3.u64 = uint64_t(int64_t(n));
}

void Send(PPCContext& ctx, uint8_t* base) {  // (caller, s, buf, len, flags)
  KUF_SOCKET_OR_FORWARD("__imp__NetDll_send", ctx.r4.u32);
  const ssize_t n = send(fd, base + ctx.r5.u32, ctx.r6.u32, Flags(ctx.r7.u32) | MSG_NOSIGNAL);
  if (n < 0) FailErrno();
  ctx.r3.u64 = uint64_t(int64_t(n));
}

void RecvFrom(PPCContext& ctx, uint8_t* base) {  // (caller, s, buf, len, flags, from, fromlen)
  KUF_SOCKET_OR_FORWARD("__imp__NetDll_recvfrom", ctx.r4.u32);
  sockaddr_in from{};
  socklen_t len = sizeof(from);
  const ssize_t n =
      recvfrom(fd, base + ctx.r5.u32, ctx.r6.u32, Flags(ctx.r7.u32), reinterpret_cast<sockaddr*>(&from), &len);
  if (n < 0) {
    FailErrno();
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  const Addr4 a{ntohl(from.sin_addr.s_addr), ToGamePort(ntohs(from.sin_port), R().port_base)};
  NotePeer(a.ip);
  if (ctx.r9.u32 && ctx.r10.u32 && Load32(base, ctx.r10.u32) >= kGuestSockaddrSize) {
    WriteGuestSockaddr(base + ctx.r9.u32, a);
    Store32(base, ctx.r10.u32, kGuestSockaddrSize);
  }
  ctx.r3.u64 = uint64_t(int64_t(n));
}

void SendTo(PPCContext& ctx, uint8_t* base) {  // (caller, s, buf, len, flags, to, tolen)
  KUF_SOCKET_OR_FORWARD("__imp__NetDll_sendto", ctx.r4.u32);
  const auto a = ctx.r9.u32 ? ReadGuestSockaddr(base + ctx.r9.u32, ctx.r10.u32) : std::nullopt;
  if (!a) {
    Fail(kWsaEFault);
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  BindOwnAddress(fd);
  Runtime& r = R();
  const uint16_t port = ToHostPort(a->port, r.port_base);
  std::vector<uint32_t> to;
  if (IsBroadcast(a->ip)) {  // to each other instance known (no broadcast between loopback addresses)
    std::lock_guard lock(r.mu);
    to.assign(r.peers.begin(), r.peers.end());
  } else {
    to.push_back(a->ip);
  }
  ssize_t n = ssize_t(ctx.r6.u32);
  for (uint32_t ip : to) {
    const sockaddr_in sin = Native(ip, port);
    const ssize_t sent = sendto(fd, base + ctx.r8.u32, ctx.r6.u32, Flags(ctx.r7.u32) | MSG_NOSIGNAL,
                                reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));
    if (sent < 0 && to.size() == 1) n = -1;
  }
  if (n < 0) FailErrno();
  ctx.r3.u64 = uint64_t(int64_t(n));
}

void SetSockOpt(PPCContext& ctx, uint8_t* base) {  // (caller, s, level, name, optval, optlen)
  KUF_SOCKET_OR_FORWARD("__imp__NetDll_setsockopt", ctx.r4.u32);
  const SockOpt o = MapSockOpt(ctx.r5.u32, ctx.r6.u32);
  const uint32_t val = ctx.r7.u32, len = ctx.r8.u32;
  int rc = 0;
  switch (o.kind) {
    case SockOpt::kInt: {
      const int v = !val ? 0 : len >= 4 ? int(Load32(base, val)) : int(base[val]);
      rc = setsockopt(fd, o.level, o.name, &v, sizeof(v));
      break;
    }
    case SockOpt::kLinger: {  // Winsock: two u_shorts
      linger l{};
      if (val && len >= 4) l = {Load16(base, val), Load16(base, val + 2)};
      rc = setsockopt(fd, o.level, o.name, &l, sizeof(l));
      break;
    }
    case SockOpt::kTimeout: {  // Winsock: milliseconds
      const uint32_t ms = val && len >= 4 ? Load32(base, val) : 0;
      timeval tv{time_t(ms / 1000), suseconds_t((ms % 1000) * 1000)};
      rc = setsockopt(fd, o.level, o.name, &tv, sizeof(tv));
      break;
    }
    case SockOpt::kAccept:
      break;
    case SockOpt::kUnknown:
      REXLOG_INFO("[kuf-coop] setsockopt level {:X} option {:X}: not translated, ignored", ctx.r5.u32, ctx.r6.u32);
      break;
  }
  if (rc != 0) FailErrno();
  ctx.r3.u64 = rc == 0 ? 0 : uint64_t(-1);
}

void IoctlSocket(PPCContext& ctx, uint8_t* base) {  // (caller, s, cmd, argp)
  KUF_SOCKET_OR_FORWARD("__imp__NetDll_ioctlsocket", ctx.r4.u32);
  const uint32_t cmd = ctx.r5.u32, arg = ctx.r6.u32;
  if (!arg) {
    Fail(kWsaEFault);
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  int rc = -1;
  if (cmd == kFionbio) {
    const int flags = fcntl(fd, F_GETFL);
    rc = flags < 0 ? -1 : fcntl(fd, F_SETFL, Load32(base, arg) ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
  } else if (cmd == kFionread) {
    int avail = 0;
    rc = ioctl(fd, FIONREAD, &avail);
    if (rc == 0) Store32(base, arg, uint32_t(avail));
  } else {
    errno = EINVAL;
  }
  if (rc != 0) FailErrno();
  ctx.r3.u64 = rc == 0 ? 0 : uint64_t(-1);
}

// (caller, nfds (ignored, as Winsock does), readfds, writefds, exceptfds, timeout)
void Select(PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) return KUF_FORWARD("__imp__NetDll_select");
  const uint32_t sets[3] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
  std::vector<uint32_t> handles[3];
  std::vector<pollfd> fds;
  std::vector<uint32_t> owner;  // the handle of each pollfd
  const auto slot = [&](uint32_t h) -> int {
    for (size_t i = 0; i < owner.size(); ++i) {
      if (owner[i] == h) return int(i);
    }
    const int fd = Fd(h);
    if (fd < 0) return -1;
    owner.push_back(h);
    fds.push_back({fd, 0, 0});
    return int(fds.size() - 1);
  };
  constexpr short kEvents[3] = {POLLIN, POLLOUT, POLLPRI};
  for (int s = 0; s < 3; ++s) {
    if (!sets[s]) continue;
    handles[s] = ReadFdSet(base + sets[s]);
    for (uint32_t h : handles[s]) {
      const int i = slot(h);
      if (i < 0) {
        Fail(kWsaENotSock);
        ctx.r3.u64 = uint64_t(-1);
        return;
      }
      fds[size_t(i)].events |= kEvents[s];
    }
  }
  const int rc = poll(fds.data(), fds.size(), TimeoutMs(ctx.r8.u32 ? base + ctx.r8.u32 : nullptr));
  if (rc < 0) {
    FailErrno();
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  // Winsock: readable also on a closed or failed connection (recv tells which); writable only when connected; a
  // failed connect is in the exception set, not the writable one.
  int total = 0;
  for (int s = 0; s < 3; ++s) {
    if (!sets[s]) continue;
    std::vector<uint32_t> ready;
    for (uint32_t h : handles[s]) {
      const size_t i = size_t(slot(h));
      const short ev = fds[i].revents;
      const bool failed = (ev & POLLERR) != 0;
      const bool yes = s == 0   ? (ev & (POLLIN | POLLHUP | POLLERR)) != 0
                       : s == 1 ? (ev & POLLOUT) != 0 && !failed
                                : (ev & POLLPRI) != 0 || failed;
      if (yes) ready.push_back(h);
    }
    WriteFdSet(base + sets[s], ready);
    total += int(ready.size());
  }
  ctx.r3.u64 = uint64_t(int64_t(total));
}

struct Override {
  const char* symbol;
  GuestImportFn fn;
};
constexpr Override kOverrides[] = {
    {"__imp__XMsgStartIORequest", XMsgStartIORequest},
    {"__imp__XMsgStartIORequestEx", XMsgStartIORequestEx},
    {"__imp__NetDll_XNetGetTitleXnAddr", XNetGetTitleXnAddr},
    {"__imp__NetDll_XNetXnAddrToInAddr", XNetXnAddrToInAddr},
    {"__imp__NetDll_XNetInAddrToXnAddr", XNetInAddrToXnAddr},
    {"__imp__NetDll_XNetXnAddrToMachineId", XNetXnAddrToMachineId},
    {"__imp__NetDll_XNetQosListen", XNetQosListen},
    {"__imp__NetDll_XNetQosLookup", XNetQosLookup},
    {"__imp__XamUserGetXUID", XamUserGetXUID},
    {"__imp__XamUserGetName", XamUserGetName},
    {"__imp__XamUserCheckPrivilege", XamUserCheckPrivilege},
    {"__imp__NetDll_bind", Bind},
    {"__imp__NetDll_connect", Connect},
    {"__imp__NetDll_listen", Listen},
    {"__imp__NetDll_accept", Accept},
    {"__imp__NetDll_recv", Recv},
    {"__imp__NetDll_send", Send},
    {"__imp__NetDll_recvfrom", RecvFrom},
    {"__imp__NetDll_sendto", SendTo},
    {"__imp__NetDll_setsockopt", SetSockOpt},
    {"__imp__NetDll_ioctlsocket", IoctlSocket},
    {"__imp__NetDll_select", Select},
};

}  // namespace

bool Enabled() { return R().role() != Role::kOff; }

LobbyService& Lobby() { return *R().lobby; }

std::string PlayerName() { return R().name; }

void Maintain() {
  R().Tick();
  kuf::lobby_screen::Tick();  // the multiplayer menus' model (Start Game, the guest's handover)
}

GuestImportFn ImportOverride(const char* symbol) {
  for (const Override& o : kOverrides) {
    if (std::strcmp(o.symbol, symbol) == 0) return o.fn;
  }
  return nullptr;
}

}  // namespace kuf::coop

// The LIVE sign-in bit of a controller (pad): set for the game's active controller in co-op, so the co-op gates and
// the session code see the player as able to play online. Other controllers keep the game's answer.
REX_HOOK_RAW(sub_827E9128) {
  if (kuf::coop::Enabled() && ctx.r3.u32 < 4 &&
      ctx.r3.u32 == kuf::coop::Load32(base, kuf::coop::kActivePad)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_827E9128(ctx, base);
}

// The network library's sign-in state (the XDK's XUserGetSigninState, a thunk): user 0 is signed in to Xbox LIVE (2)
// in co-op. Its sign-in handler (sub_827E9210) counts LIVE users from it, and with none, sub_827EAD10 hands the
// session requests to an offline stub (vtable 0x8208F9C0) whose host request fails: the "cannot connect to Xbox LIVE"
// box seen in the first two-instance test. Only this library's three callers see it; the runtime's own sign-in state
// (and every other caller) stays as it is.
REX_HOOK_RAW(sub_82827E68) {
  if (kuf::coop::Enabled() && ctx.r3.u32 == 0) {
    ctx.r3.u64 = 2;
    return;
  }
  __imp__sub_82827E68(ctx, base);
}

// The game type screen's 5-tick poll of the co-op gate (sub_82669770's test without its message box).
REX_HOOK_RAW(sub_82668698) {
  if (kuf::coop::Enabled() || kuf::lobby_screen::Active()) {
    kuf::coop::Maintain();
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82668698(ctx, base);
}

// Normal builds bind the imports to the overrides here; import-audit builds wrap every import and reach them
// through ImportOverride() (runtime/import_audit.cpp).
#if !defined(KUFCOD_IMPORT_AUDIT)
#define KUF_COOP_HOOK(name)                                                                   \
  REX_HOOK_RAW(__imp__##name) {                                                               \
    static const kuf::coop::GuestImportFn fn = kuf::coop::ImportOverride("__imp__" #name);    \
    fn(ctx, base);                                                                            \
  }
KUF_COOP_HOOK(XMsgStartIORequest)
KUF_COOP_HOOK(XMsgStartIORequestEx)
KUF_COOP_HOOK(NetDll_XNetGetTitleXnAddr)
KUF_COOP_HOOK(NetDll_XNetXnAddrToInAddr)
KUF_COOP_HOOK(NetDll_XNetInAddrToXnAddr)
KUF_COOP_HOOK(NetDll_XNetXnAddrToMachineId)
KUF_COOP_HOOK(NetDll_XNetQosListen)
KUF_COOP_HOOK(NetDll_XNetQosLookup)
KUF_COOP_HOOK(XamUserGetXUID)
KUF_COOP_HOOK(XamUserGetName)
KUF_COOP_HOOK(XamUserCheckPrivilege)
KUF_COOP_HOOK(NetDll_bind)
KUF_COOP_HOOK(NetDll_connect)
KUF_COOP_HOOK(NetDll_listen)
KUF_COOP_HOOK(NetDll_accept)
KUF_COOP_HOOK(NetDll_recv)
KUF_COOP_HOOK(NetDll_send)
KUF_COOP_HOOK(NetDll_recvfrom)
KUF_COOP_HOOK(NetDll_sendto)
KUF_COOP_HOOK(NetDll_setsockopt)
KUF_COOP_HOOK(NetDll_ioctlsocket)
KUF_COOP_HOOK(NetDll_select)
#endif

#else  // _WIN32: not available yet (the socket layer is POSIX)

namespace kuf::coop {
bool Enabled() { return false; }
void Maintain() {}
std::string PlayerName() { return "Player"; }
GuestImportFn ImportOverride(const char*) { return nullptr; }
}  // namespace kuf::coop

#endif
