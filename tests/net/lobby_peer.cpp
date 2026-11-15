// One side of a lobby across programs (M17F): built for Linux and for Windows from this one source, so that
// tests/net/cross_platform_lobby.sh can put a Linux host with a Windows guest and the other way round (the Windows
// program under Wine on the development machine; on two real PCs the same commands work as they are).
//
//   kuf_lobby_peer host --port P --discovery D --password PW --name NAME
//   kuf_lobby_peer join --address A --port P --discovery D --password PW --name NAME [--expect-reject]
//
// The host publishes 256 bytes of session data, waits for one guest, starts the game, waits for the guest to leave.
// The guest finds the lobby by LAN discovery, joins (password proof both ways), checks the session data byte for
// byte and the member list, sees the start, leaves. Each prints one PASS/FAIL line; exit status 0 = PASS.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/session.h"

using namespace std::chrono_literals;
using namespace kuf::net;
using Clock = std::chrono::steady_clock;

namespace {

Bytes Pattern() {
  Bytes b(256);
  for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t(i * 7 + 3);
  return b;
}

#if defined(_WIN32)
const char* kSystem = "windows";
#else
const char* kSystem = "linux";
#endif

int Fail(const char* role, const std::string& why) {
  std::printf("%s %s FAIL: %s\n", role, kSystem, why.c_str());
  std::fflush(stdout);
  return 1;
}

// Every event a peer reported, in order: events come in batches, and a later wait must still find an event that
// arrived with an earlier one (the session data comes right after the join).
std::vector<Event> g_history;

template <class Peer>
std::optional<Event> WaitFor(Peer& p, Event::Kind kind, std::chrono::milliseconds limit, std::string* seen) {
  const auto end = Clock::now() + limit;
  size_t checked = 0;
  for (;;) {
    for (Event& e : p.TakeEvents()) {
      *seen += std::to_string(int(e.kind)) + ":" + e.text + " ";
      g_history.push_back(e);
    }
    for (; checked < g_history.size(); ++checked) {
      if (g_history[checked].kind == kind) {
        Event e = g_history[checked];
        g_history.erase(g_history.begin() + ptrdiff_t(checked));
        return e;
      }
    }
    if (Clock::now() >= end) return std::nullopt;
    std::this_thread::sleep_for(20ms);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return Fail("usage", "kuf_lobby_peer host|join [options]");
  const std::string mode = argv[1];
  std::string address = "127.0.0.1", password, name = mode == "host" ? "Host" : "Guest";
  uint16_t port = 0, discovery = 0;
  bool expect_reject = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--address") address = next();
    else if (a == "--port") port = uint16_t(std::atoi(next().c_str()));
    else if (a == "--discovery") discovery = uint16_t(std::atoi(next().c_str()));
    else if (a == "--password") password = next();
    else if (a == "--name") name = next();
    else if (a == "--expect-reject") expect_reject = true;
  }
  std::string seen;

  if (mode == "host") {
    Host host;
    HostConfig c;
    c.lobby_name = std::string("Lobby on ") + kSystem;
    c.host_name = name;
    c.password = password;
    c.bind_address = address;
    c.port = port;
    c.discovery_port = discovery;
    std::string error;
    if (!host.Start(c, &error)) return Fail("HOST", error);
    host.SetSessionData(Pattern());
    std::printf("HOST %s READY port %u\n", kSystem, unsigned(host.Port()));
    std::fflush(stdout);
    if (!WaitFor(host, Event::kJoined, 30s, &seen)) return Fail("HOST", "no guest joined; events: " + seen);
    const LobbyState s = host.State();
    if (s.members.size() != 2) return Fail("HOST", "members: " + std::to_string(s.members.size()));
    if (!host.StartGame(&error)) return Fail("HOST", "start: " + error);
    if (!WaitFor(host, Event::kLeft, 20s, &seen)) return Fail("HOST", "the guest did not leave; events: " + seen);
    std::printf("HOST %s PASS: guest '%s' joined, game started, guest left\n", kSystem, s.members[1].name.c_str());
    return 0;
  }

  if (mode == "join") {
    // LAN discovery first (UDP): the host's lobby must answer across the two programs.
    if (discovery) {
      const auto found = Discover({address}, discovery, 1500ms);
      if (found.size() != 1) return Fail("GUEST", "discovery found " + std::to_string(found.size()) + " lobbies");
      if (found[0].info.name.rfind("Lobby on ", 0) != 0) return Fail("GUEST", "discovered " + found[0].info.name);
      std::printf("GUEST %s discovered '%s' (password %s)\n", kSystem, found[0].info.name.c_str(),
                  found[0].info.password ? "yes" : "no");
    }
    Guest guest;
    guest.Join({address, port, 0}, name, password, 10s);
    if (expect_reject) {
      auto e = WaitFor(guest, Event::kRejected, 15s, &seen);
      if (!e) return Fail("GUEST", "expected a refusal; events: " + seen);
      if (e->reason != RejectReason::kPassword) return Fail("GUEST", "refused, but not for the password: " + e->text);
      std::printf("GUEST %s PASS: refused for the wrong password\n", kSystem);
      return 0;
    }
    if (!WaitFor(guest, Event::kJoined, 15s, &seen)) return Fail("GUEST", "not joined; events: " + seen);
    if (!WaitFor(guest, Event::kSessionData, 10s, &seen)) return Fail("GUEST", "no session data; events: " + seen);
    if (guest.SessionData() != Pattern()) return Fail("GUEST", "session data differs");
    const auto state = guest.State();
    if (!state || state->members.size() != 2) return Fail("GUEST", "member list wrong");
    if (!WaitFor(guest, Event::kStarted, 15s, &seen)) return Fail("GUEST", "no start; events: " + seen);
    const std::string lobby = state->info.name, host_name = state->members[0].name;
    guest.Leave();
    std::printf("GUEST %s PASS: joined '%s' (host '%s'), session data identical, start seen, left\n", kSystem,
                lobby.c_str(), host_name.c_str());
    return 0;
  }
  return Fail("usage", "mode " + mode);
}
