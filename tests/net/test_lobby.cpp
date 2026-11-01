// The lobby service (src/coop/lobby_service.cpp, steam_provider.cpp): hosting, codes, browsing, joining and the Steam
// invitation flow, over loopback only (no Steam: a fake SteamApi).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <random>
#include <thread>

#include "coop/coop_addr.h"
#include "coop/lobby_service.h"
#include "coop/steam_provider.h"

using namespace kuf::coop;
using namespace std::chrono_literals;
namespace net = kuf::net;

namespace {

ServiceConfig Loopback(uint16_t discovery = 0) {
  ServiceConfig c;
  c.lobby_port = 0;  // any free port
  c.discovery_port = discovery;
  c.bind_address = "127.0.0.1";
  c.code_address = *ParseIpv4("127.0.0.1");
  return c;
}

LobbySettings Settings(std::string password = "") {
  LobbySettings s;
  s.name = "Test lobby";
  s.player = "Host";
  s.difficulty = 1;
  s.max_players = 4;
  s.password = std::move(password);
  return s;
}

// Polls `services` until `done` holds (up to 5 s).
template <typename F>
bool Until(std::initializer_list<LobbyService*> services, F done, std::chrono::milliseconds limit = 5000ms) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    for (LobbyService* s : services) s->Poll();
    if (done()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

uint16_t RandomPort() {
  std::random_device rd;
  return uint16_t(42000 + rd() % 8000);
}

}  // namespace

TEST_CASE("lobby service: reach of an address, the code's address", "[net][lobby]") {
  CHECK(ReachOf(*ParseIpv4("127.0.0.1")) == Reach::kLoopback);
  CHECK(ReachOf(*ParseIpv4("192.168.1.20")) == Reach::kLan);
  CHECK(ReachOf(*ParseIpv4("10.1.2.3")) == Reach::kLan);
  CHECK(ReachOf(*ParseIpv4("172.20.0.5")) == Reach::kLan);
  CHECK(ReachOf(*ParseIpv4("172.40.0.5")) == Reach::kInternet);
  CHECK(ReachOf(*ParseIpv4("81.2.3.4")) == Reach::kInternet);
  const uint32_t lan = *ParseIpv4("192.168.1.20"), pub = *ParseIpv4("81.2.3.4");
  CHECK(CodeAddress({pub, lan}, 0) == lan);  // a private address first: codes are LAN codes by default
  CHECK(CodeAddress({lan}, pub) == pub);     // the player's public address makes an internet code
  CHECK(CodeAddress({}, 0) == 0);
}

TEST_CASE("lobby service: host, join by code, members, session data, start", "[net][lobby]") {
  LobbyService host(Loopback()), guest(Loopback());
  std::string error;
  REQUIRE(host.Create(Settings(), &error));
  const Snapshot h = host.State();
  CHECK(h.phase == Phase::kHosting);
  CHECK(h.code.rfind("KUF-", 0) == 0);
  CHECK(h.reach == Reach::kLoopback);
  CHECK_FALSE(host.Create(Settings(), &error));  // one lobby at a time
  // Start is refused without the game's own session (and without a guest).
  CHECK_FALSE(host.StartGame(&error));
  CHECK(error.find("session") != std::string::npos);

  REQUIRE(guest.JoinCode(h.code, "Guest", "", &error));
  REQUIRE(Until({&host, &guest}, [&] { return guest.State().phase == Phase::kInLobby; }));
  REQUIRE(Until({&host, &guest}, [&] { return host.State().members.size() == 2; }));
  CHECK(guest.HostAddress() == *ParseIpv4("127.0.0.1"));

  host.SetSessionData({1, 2, 3});
  REQUIRE(Until({&host, &guest}, [&] { return guest.State().session_ready; }));
  CHECK(guest.SessionData() == net::Bytes{1, 2, 3});
  REQUIRE(host.StartGame(&error));
  CHECK(host.State().phase == Phase::kInGame);
  REQUIRE(Until({&host, &guest}, [&] { return guest.State().phase == Phase::kInGame; }));
  host.Leave();
  guest.Leave();
  CHECK_FALSE(host.Active());
}

TEST_CASE("lobby service: invalid, wrong-password and expired codes", "[net][lobby]") {
  LobbyService host(Loopback()), guest(Loopback());
  std::string error;
  CHECK_FALSE(guest.JoinCode("KUF-1234", "Guest", "", &error));
  CHECK(error.find("not a valid invite code") != std::string::npos);
  CHECK_FALSE(guest.Active());

  REQUIRE(host.Create(Settings("secret"), &error));
  const std::string code = host.State().code;
  REQUIRE(guest.JoinCode(code, "Guest", "wrong", &error));
  REQUIRE(Until({&host, &guest}, [&] { return !guest.Active(); }));
  CHECK(guest.State().error.find("assword") != std::string::npos);

  REQUIRE(guest.JoinCode(code, "Guest", "secret", &error));
  REQUIRE(Until({&host, &guest}, [&] { return guest.State().phase == Phase::kInLobby; }));
  guest.Leave();

  // The lobby closes: its code no longer joins anything (a new lobby has another ID and port).
  host.Leave();
  REQUIRE(guest.JoinCode(code, "Guest", "secret", &error));
  REQUIRE(Until({&guest}, [&] { return !guest.Active(); }, 15000ms));
  CHECK_FALSE(guest.State().error.empty());
}

TEST_CASE("lobby service: the browser lists only lobbies a provider found", "[net][lobby]") {
  const uint16_t discovery = RandomPort();
  LobbyService host(Loopback(discovery)), browser(Loopback());
  browser.AddProvider(MakeDirectProvider(discovery, 600ms, {"127.0.0.1"}));
  browser.Refresh();
  REQUIRE(Until({&browser}, [&] { return !browser.State().refreshing; }));
  CHECK(browser.State().lobbies.empty());  // nothing hosted: nothing shown

  std::string error;
  REQUIRE(host.Create(Settings(), &error));
  browser.Refresh();
  REQUIRE(Until({&host, &browser}, [&] { return !browser.State().refreshing; }));
  const auto lobbies = browser.State().lobbies;
  REQUIRE(lobbies.size() == 1);
  CHECK(lobbies[0].info.name == "Test lobby");
  CHECK(lobbies[0].info.max_players == 4);
  CHECK(lobbies[0].joinable);
  REQUIRE(browser.Join(lobbies[0], "Guest", "", &error));
  REQUIRE(Until({&host, &browser}, [&] { return browser.State().phase == Phase::kInLobby; }));
}

namespace {

// Steam as the provider sees it, scripted by the test.
struct FakeSteam : SteamApi {
  bool running = true;
  std::string own_invite, owner_invite;
  std::vector<uint64_t> joined;
  int created = 0, updated = 0;
  std::deque<net::steam::SteamEvent> queue;
  std::vector<net::steam::SteamEvent> taken;
  bool Running() const override { return running; }
  std::string Unavailable() const override { return "Steam is off (no App ID configured)."; }
  bool CreateLobby(const net::LobbyInfo&) override { return ++created, true; }
  bool UpdateLobby(const net::LobbyInfo&) override { return ++updated, true; }
  bool SetOwnInvite(const std::string& code) override { return own_invite = code, true; }
  std::string OwnerInvite() override { return owner_invite; }
  bool RequestLobbies() override { return true; }
  bool JoinLobby(uint64_t lobby) override { return joined.push_back(lobby), true; }
  void LeaveLobby() override {}
  bool OpenInviteDialog() override { return true; }
  void RunCallbacks() override {}
  std::vector<net::steam::SteamEvent> TakeEvents() override {
    std::vector<net::steam::SteamEvent> out(queue.begin(), queue.end());
    queue.clear();
    return out;
  }
};

}  // namespace

TEST_CASE("Steam provider: unavailable without an App ID; the menus get the reason", "[net][lobby][steam]") {
  auto fake = std::make_unique<FakeSteam>();
  fake->running = false;
  LobbyService s(Loopback());
  s.AddProvider(MakeSteamProvider(std::move(fake)));
  const auto providers = s.State().providers;
  REQUIRE(providers.size() == 1);
  CHECK_FALSE(providers[0].available);
  CHECK(providers[0].reason.find("App ID") != std::string::npos);
  std::string error;
  REQUIRE(s.Create(Settings(), &error));
  CHECK_FALSE(s.OpenInviteDialog(&error));  // no invitation provider: share the code
}

TEST_CASE("Steam provider: the host's lobby carries the native code; an accepted invitation joins natively",
          "[net][lobby][steam]") {
  auto host_steam = std::make_unique<FakeSteam>();
  FakeSteam* hs = host_steam.get();
  LobbyService host(Loopback());
  host.AddProvider(MakeSteamProvider(std::move(host_steam)));
  LobbySettings settings = Settings();
  settings.backend = net::Backend::kSteam;
  std::string error;
  REQUIRE(host.Create(settings, &error));
  CHECK(hs->created == 1);
  hs->queue.push_back({net::steam::SteamEvent::kCreated, 77, {}, ""});
  host.Poll();
  CHECK(hs->own_invite == host.State().code);  // member data, never the listed metadata
  CHECK(hs->updated >= 1);
  CHECK(host.OpenInviteDialog(&error));

  auto guest_steam = std::make_unique<FakeSteam>();
  FakeSteam* gs = guest_steam.get();
  ServiceConfig gc = Loopback();
  gc.player = "Guest";
  LobbyService guest(gc);
  guest.AddProvider(MakeSteamProvider(std::move(guest_steam)));
  gs->queue.push_back({net::steam::SteamEvent::kInviteAccepted, 77, {}, ""});
  guest.Poll();
  REQUIRE(gs->joined == std::vector<uint64_t>{77});
  CHECK(guest.State().phase == Phase::kJoining);
  gs->queue.push_back({net::steam::SteamEvent::kEntered, 77, {}, ""});
  guest.Poll();  // entered, but the owner's code has not arrived: still waiting
  CHECK(guest.State().phase == Phase::kJoining);
  gs->owner_invite = hs->own_invite;
  gs->queue.push_back({net::steam::SteamEvent::kDataChanged, 77, {}, ""});
  REQUIRE(Until({&host, &guest}, [&] { return guest.State().phase == Phase::kInLobby; }));
  REQUIRE(Until({&host, &guest}, [&] { return host.State().members.size() == 2; }));
}

TEST_CASE("Steam provider: a failed enter is reported, not a join", "[net][lobby][steam]") {
  auto fake = std::make_unique<FakeSteam>();
  FakeSteam* f = fake.get();
  LobbyService guest(Loopback());
  guest.AddProvider(MakeSteamProvider(std::move(fake)));
  f->queue.push_back({net::steam::SteamEvent::kInviteAccepted, 5, {}, ""});
  guest.Poll();
  f->queue.push_back({net::steam::SteamEvent::kEnterFailed, 5, {}, "The Steam lobby could not be joined."});
  guest.Poll();
  CHECK(guest.State().phase == Phase::kIdle);
  CHECK(guest.State().error.find("could not be joined") != std::string::npos);
}
