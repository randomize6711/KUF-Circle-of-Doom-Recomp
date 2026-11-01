// The lobby layer (src/net): password hashing against the standards' vectors, the protocol's encoding and its
// refusal of malformed data, invite codes, and real host / guest sessions over the loopback interface only
// (127.0.0.1; nothing is reachable from the network).

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <random>
#include <string>
#include <thread>

#include "net/crypto.h"
#include "net/protocol.h"
#include "net/session.h"

using namespace kuf::net;
using namespace std::chrono_literals;

namespace {

std::string HexOf(const Digest& d) { return Hex(d); }

// Waits for an event of `kind` (up to 5 s), returning it.
template <typename Side>
std::optional<Event> WaitFor(Side& side, Event::Kind kind, std::chrono::milliseconds limit = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    for (const Event& e : side.TakeEvents()) {
      if (e.kind == kind) return e;
    }
    std::this_thread::sleep_for(10ms);
  }
  return std::nullopt;
}

HostConfig LocalConfig() {
  HostConfig c;
  c.lobby_name = "Test lobby";
  c.host_name = "Host";
  c.bind_address = "127.0.0.1";
  c.port = 0;            // any free port
  c.discovery_port = 0;  // no discovery unless a test asks
  c.handshake_timeout = 1500ms;
  c.idle_timeout = 3000ms;
  return c;
}

JoinTarget To(const Host& h, uint64_t lobby = 0) { return {"127.0.0.1", h.Port(), lobby}; }

}  // namespace

TEST_CASE("SHA-256, HMAC-SHA-256 and PBKDF2 match the standards' test vectors", "[net]") {
  CHECK(HexOf(Sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(HexOf(Sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(HexOf(Sha256(std::string(1000000, 'a'))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // RFC 4231, test cases 1 and 2.
  const std::string key1(20, '\x0b'), data1 = "Hi There";
  CHECK(HexOf(HmacSha256(reinterpret_cast<const uint8_t*>(key1.data()), key1.size(),
                         reinterpret_cast<const uint8_t*>(data1.data()), data1.size())) ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  const std::string key2 = "Jefe", data2 = "what do ya want for nothing?";
  CHECK(HexOf(HmacSha256(reinterpret_cast<const uint8_t*>(key2.data()), key2.size(),
                         reinterpret_cast<const uint8_t*>(data2.data()), data2.size())) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // PBKDF2-HMAC-SHA-256 (RFC 7914 section 11's inputs, 1 and 4096 iterations).
  const std::string salt = "salt";
  CHECK(HexOf(Pbkdf2Sha256("password", reinterpret_cast<const uint8_t*>(salt.data()), salt.size(), 1)) ==
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
  CHECK(HexOf(Pbkdf2Sha256("password", reinterpret_cast<const uint8_t*>(salt.data()), salt.size(), 4096)) ==
        "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
  uint8_t a[16] = {}, b[16] = {};
  REQUIRE(RandomBytes(a, sizeof(a)));
  REQUIRE(RandomBytes(b, sizeof(b)));
  CHECK_FALSE(ConstantTimeEqual(a, b, sizeof(a)));
}

TEST_CASE("protocol: messages round-trip; names are cleaned; values are range-checked", "[net]") {
  LobbyState s;
  s.info.id = 0xDEADBEEF;
  s.info.name = "Bob's \x01lobby";
  s.info.host = std::string(80, 'x');
  s.info.players = 2;
  s.info.max_players = 3;
  s.info.password = true;
  s.info.port = 36961;
  s.members = {{0, "Host"}, {1, "Guest"}};
  const Bytes frame = EncodeLobby(s);
  auto m = DecodeFrame(frame.data(), frame.size());
  REQUIRE(m);
  CHECK(m->type == MsgType::kLobby);
  CHECK(m->lobby.info.id == 0xDEADBEEF);
  CHECK(m->lobby.info.name == "Bob's lobby");          // control character removed
  CHECK(m->lobby.info.host.size() == kMaxName);        // cut
  CHECK(m->lobby.info.password);
  CHECK(m->lobby.members.size() == 2);
  // More players than the game supports, or a count that does not match the members: refused.
  LobbyState bad = s;
  bad.info.max_players = 5;
  Bytes b = EncodeLobby(bad);
  CHECK_FALSE(DecodeFrame(b.data(), b.size()));
  bad = s;
  bad.info.players = 3;
  b = EncodeLobby(bad);
  CHECK_FALSE(DecodeFrame(b.data(), b.size()));
  // A host asking for an absurd PBKDF2 effort is refused (no denial of service of the guest).
  Challenge ch;
  ch.iterations = 50000000;
  b = EncodeChallenge(ch);
  CHECK_FALSE(DecodeFrame(b.data(), b.size()));
  // The password never appears in any message: only proofs derived from it.
  Hello h;
  h.player = "P";
  b = EncodeHello(h);
  const std::string wire(b.begin(), b.end());
  CHECK(wire.find("hunter2") == std::string::npos);
}

TEST_CASE("protocol: malformed, truncated, oversized and random frames are refused", "[net]") {
  const Bytes good = EncodeSimple(MsgType::kPing, 7);
  REQUIRE(DecodeFrame(good.data(), good.size()));
  CHECK_FALSE(DecodeFrame(good.data(), good.size() - 1));  // truncated
  Bytes extra = good;
  extra[3] += 1;
  extra.push_back(0);
  CHECK_FALSE(DecodeFrame(extra.data(), extra.size()));  // trailing data in the payload
  Bytes type = good;
  type[4] = 99;
  CHECK_FALSE(DecodeFrame(type.data(), type.size()));  // unknown type
  // An oversized frame closes the connection (TakeFrames refuses it before reading it).
  Bytes huge = {0x7F, 0xFF, 0xFF, 0xFF, 1};
  std::vector<Message> out;
  CHECK_FALSE(TakeFrames(&huge, &out));
  // Partial frames wait for the rest; two frames in one buffer both come out.
  Bytes stream = good;
  stream.insert(stream.end(), good.begin(), good.end());
  Bytes partial(stream.begin(), stream.begin() + ptrdiff_t(good.size() + 2));
  out.clear();
  CHECK(TakeFrames(&partial, &out));
  CHECK(out.size() == 1);
  CHECK(partial.size() == 2);
  // Random input never crashes the decoder.
  std::mt19937 rng(42);
  for (int i = 0; i < 20000; ++i) {
    Bytes r(size_t(rng() % 64));
    for (auto& x : r) x = uint8_t(rng());
    if (r.size() >= 4 && (rng() & 1)) {  // a plausible length, so the payload decoder is reached
      const uint32_t len = uint32_t(r.size() - 4);
      r[0] = 0;
      r[1] = 0;
      r[2] = uint8_t(len >> 8);
      r[3] = uint8_t(len);
    }
    (void)DecodeFrame(r.data(), r.size());
    std::vector<Message> sink;
    (void)TakeFrames(&r, &sink);
  }
}

TEST_CASE("invite codes round-trip and catch typos", "[net]") {
  InviteCode c{0xC0A80117, 36961, 0x12345678};  // 192.168.1.23
  const std::string code = FormatInviteCode(c);
  CHECK(code.rfind("KUF-", 0) == 0);
  const auto back = ParseInviteCode(code);
  REQUIRE(back);
  CHECK(back->ipv4 == c.ipv4);
  CHECK(back->port == c.port);
  CHECK(back->lobby == c.lobby);
  std::string lower = code;
  for (auto& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  CHECK(ParseInviteCode(lower));  // case does not matter
  std::string typo = code;
  typo[5] = typo[5] == 'A' ? 'B' : 'A';
  CHECK_FALSE(ParseInviteCode(typo));  // the check digit
  CHECK_FALSE(ParseInviteCode("KUF-1234"));
}

TEST_CASE("discovery answers are tied to the query and list public lobbies only", "[net]") {
  Nonce n{};
  RandomBytes(n.data(), n.size());
  LobbyInfo pub;
  pub.id = 1;
  pub.name = "Public";
  Bytes a = EncodeDiscoveryAnswer(n, pub);
  CHECK(DecodeDiscoveryAnswer(a.data(), a.size(), n));
  Nonce other = n;
  other[0] ^= 1;
  CHECK_FALSE(DecodeDiscoveryAnswer(a.data(), a.size(), other));  // not an answer to this query
  LobbyInfo priv = pub;
  priv.visibility = Visibility::kPrivate;
  a = EncodeDiscoveryAnswer(n, priv);
  CHECK_FALSE(DecodeDiscoveryAnswer(a.data(), a.size(), n));
}

TEST_CASE("sessions: joining with the right password, refused with a wrong one, full, private", "[net][session]") {
  Host host;
  HostConfig cfg = LocalConfig();
  cfg.password = "hunter2";
  cfg.max_players = 2;
  std::string error;
  REQUIRE(host.Start(cfg, &error));
  CHECK(host.State().info.password);

  Guest wrong;
  wrong.Join(To(host), "Mallory", "letmein");
  auto e = WaitFor(wrong, Event::kRejected);
  REQUIRE(e);
  CHECK(e->reason == RejectReason::kPassword);
  CHECK(host.State().members.size() == 1);

  Guest none;
  none.Join(To(host), "Eve", "");  // no password at all
  e = WaitFor(none, Event::kRejected);
  REQUIRE(e);
  CHECK(e->reason == RejectReason::kPassword);

  Guest ok;
  ok.Join(To(host), "Alice", "hunter2");
  REQUIRE(WaitFor(ok, Event::kJoined));
  CHECK(ok.Joined());
  REQUIRE(WaitFor(host, Event::kJoined));
  CHECK(host.State().members.size() == 2);
  CHECK(ok.State()->members.size() == 2);

  Guest late;  // the lobby holds 2
  late.Join(To(host), "Bob", "hunter2");
  e = WaitFor(late, Event::kRejected);
  REQUIRE(e);
  CHECK(e->reason == RejectReason::kFull);

  // Only the host starts; the guest is told.
  REQUIRE(host.StartGame(&error));
  CHECK(WaitFor(ok, Event::kStarted));
  ok.Leave();
  host.Stop();
}

TEST_CASE("sessions: a private lobby needs its ID; starting needs two players; leaving is seen", "[net][session]") {
  Host host;
  HostConfig cfg = LocalConfig();
  cfg.visibility = Visibility::kPrivate;
  std::string error;
  REQUIRE(host.Start(cfg, &error));
  CHECK_FALSE(host.StartGame(&error));  // alone

  Guest stranger;
  stranger.Join(To(host), "Stranger", "");
  auto e = WaitFor(stranger, Event::kRejected);
  REQUIRE(e);
  CHECK(e->reason == RejectReason::kNotInvited);

  // With the invite code's lobby ID.
  const auto code = ParseInviteCode(FormatInviteCode({0x7F000001, host.Port(), uint32_t(host.State().info.id)}));
  REQUIRE(code);
  Guest friendly;
  friendly.Join({"127.0.0.1", code->port, code->lobby}, "Friend", "");
  REQUIRE(WaitFor(friendly, Event::kJoined));
  REQUIRE(WaitFor(host, Event::kJoined));
  friendly.Leave();
  e = WaitFor(host, Event::kLeft);
  REQUIRE(e);
  CHECK(e->text == "Friend");
  CHECK(host.State().members.size() == 1);
  host.Stop();
}

TEST_CASE("sessions: wrong versions, silent and malformed connections are dropped; the host closing is seen",
          "[net][session]") {
  Host host;
  std::string error;
  REQUIRE(host.Start(LocalConfig(), &error));
  auto raw = [&] {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(host.Port());
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    return fd;
  };
  auto read_reject = [](int fd) -> std::optional<RejectReason> {
    Bytes in;
    pollfd p{fd, POLLIN, 0};
    for (int i = 0; i < 50 && poll(&p, 1, 100) >= 0; ++i) {
      uint8_t buf[256];
      const ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
      if (n > 0) in.insert(in.end(), buf, buf + n);
      std::vector<Message> m;
      if (TakeFrames(&in, &m)) {
        for (auto& x : m) {
          if (x.type == MsgType::kReject) return x.reject.reason;
        }
      }
      if (n == 0) break;
    }
    return std::nullopt;
  };
  // Another protocol version.
  int fd = raw();
  Hello h;
  h.version = kProtocolVersion + 1;
  Bytes b = EncodeHello(h);
  send(fd, b.data(), b.size(), 0);
  CHECK(read_reject(fd) == RejectReason::kVersion);
  close(fd);
  // Garbage.
  fd = raw();
  const uint8_t junk[] = {0, 0, 0, 3, 77, 1, 2};
  send(fd, junk, sizeof(junk), 0);
  CHECK(read_reject(fd) == RejectReason::kMalformed);
  close(fd);
  // Silence past the handshake timeout.
  fd = raw();
  std::this_thread::sleep_for(2000ms);
  CHECK(read_reject(fd) == RejectReason::kTimeout);
  close(fd);
  // A member sees the host close the lobby.
  Guest g;
  g.Join(To(host), "Guest", "");
  REQUIRE(WaitFor(g, Event::kJoined));
  host.Stop();
  auto e = WaitFor(g, Event::kDisconnected);
  REQUIRE(e);
  CHECK(e->reason == RejectReason::kClosed);
}

TEST_CASE("LAN discovery on the loopback interface: public lobbies answer, private ones do not", "[net][session]") {
  const uint16_t port = uint16_t(40000 + (getpid() % 20000));
  Host pub, priv;
  HostConfig a = LocalConfig();
  a.discovery_port = port;
  a.lobby_name = "Open";
  std::string error;
  REQUIRE(pub.Start(a, &error));
  const auto found = Discover({"127.0.0.1"}, port, 500ms);
  REQUIRE(found.size() == 1);
  CHECK(found[0].info.name == "Open");
  CHECK(found[0].info.port == pub.Port());
  CHECK(found[0].address == "127.0.0.1");
  pub.Stop();
  HostConfig b = a;
  b.visibility = Visibility::kPrivate;
  REQUIRE(priv.Start(b, &error));
  CHECK(Discover({"127.0.0.1"}, port, 300ms).empty());
  priv.Stop();
}

#include "net/steam_lobby.h"

TEST_CASE("Steam lobbies: metadata round-trips; other applications' lobbies and versions are not listed", "[net][steam]") {
  namespace st = kuf::net::steam;
  LobbyInfo info;
  info.name = "Friends' run";
  info.host = "Host";
  info.difficulty = 2;
  info.max_players = 3;
  info.password = true;
  auto md = st::LobbyMetadata(info);
  CHECK(md.at("password") == "1");
  for (const auto& [k, v] : md) CHECK(v.find("hunter2") == std::string::npos);  // never the password itself
  auto back = st::InfoFromMetadata(md, 109775243424972800ull, 2, 3);
  REQUIRE(back);
  CHECK(back->name == "Friends' run");
  CHECK(back->difficulty == 2);
  CHECK(back->players == 2);
  CHECK(back->max_players == 3);
  CHECK(back->password);
  CHECK(back->backend == Backend::kSteam);
  // Another application's (Spacewar's own) lobby: no project key.
  CHECK_FALSE(st::InfoFromMetadata({{"name", "spacewar game"}}, 1, 1, 4));
  // Another protocol version, malformed values, more members than slots, more than 4 players.
  auto other = md;
  other["protocol"] = "999";
  CHECK_FALSE(st::InfoFromMetadata(other, 1, 1, 3));
  other = md;
  other["difficulty"] = "x";
  CHECK_FALSE(st::InfoFromMetadata(other, 1, 1, 3));
  CHECK_FALSE(st::InfoFromMetadata(md, 1, 4, 3));
  other = md;
  other["max_players"] = "8";
  CHECK_FALSE(st::InfoFromMetadata(other, 1, 1, 8));
  // Visibility to Steam's lobby types.
  CHECK(st::LobbyTypeFor(Visibility::kPrivate) == 0);
  CHECK(st::LobbyTypeFor(Visibility::kFriends) == 1);
  CHECK(st::LobbyTypeFor(Visibility::kPublic) == 2);
}

TEST_CASE("Steam settings: the App ID is off unless given; invitations from the command line", "[net][steam]") {
  namespace st = kuf::net::steam;
  CHECK(st::ParseAppId("") == 0u);
  CHECK(st::ParseAppId("0") == 0u);
  CHECK(st::ParseAppId("480") == 480u);
  CHECK_FALSE(st::ParseAppId("480x"));
  CHECK(st::ConnectLobbyArgument({"kuf_cod", "+connect_lobby", "109775243424972800"}) == 109775243424972800ull);
  CHECK_FALSE(st::ConnectLobbyArgument({"kuf_cod", "+connect_lobby"}));
  CHECK_FALSE(st::ConnectLobbyArgument({"+connect_lobby", "abc"}));
}

#include "coop/coop_addr.h"

TEST_CASE("co-op addresses: an XNADDR per address that translates back; ports; identities", "[net][coop]") {
  namespace co = kuf::coop;
  const uint32_t a = *co::ParseIpv4("127.0.0.10"), b = *co::ParseIpv4("192.168.1.23");
  const auto xa = co::MakeXnAddr(a), xb = co::MakeXnAddr(b);
  CHECK(xa != xb);
  CHECK(co::IpOfXnAddr(xa.data()) == a);
  CHECK(co::IpOfXnAddr(xb.data()) == b);
  CHECK(co::FormatIpv4(b) == "192.168.1.23");
  CHECK(xa[0] == 127);  // ina in network byte order, as the console stores it
  auto foreign = xa;
  foreign[11] = 'X';    // another scheme's XNADDR: not translated
  CHECK_FALSE(co::IpOfXnAddr(foreign.data()));
  CHECK(co::MachineIdOf(a) != co::MachineIdOf(b));
  // The game's ports (TCP 1000, UDP 1001) to an unprivileged base and back; others untouched.
  CHECK(co::ToHostPort(1000, 37000) == 37000);
  CHECK(co::ToHostPort(1001, 37000) == 37001);
  CHECK(co::ToHostPort(80, 37000) == 80);
  CHECK(co::ToGamePort(37001, 37000) == 1001);
  CHECK(co::ToGamePort(50000, 37000) == 50000);
  CHECK(co::XuidOf(a) != co::XuidOf(b));
  CHECK(co::DefaultName(b).size() <= 15);
  CHECK_FALSE(co::ParseIpv4("300.1.1.1"));
  CHECK_FALSE(co::ParseIpv4("1.2.3"));
}

TEST_CASE("co-op session advertisement: round trip, limits", "[net][coop]") {
  namespace co = kuf::coop;
  co::SessionAd ad;
  ad.id = {1, 2, 3, 4, 5, 6, 7, 8};
  ad.host = co::MakeXnAddr(*co::ParseIpv4("127.0.0.10"));
  ad.key[0] = 9;
  ad.public_slots = 4;
  ad.filled_public = 1;
  ad.qos = {0xAA, 0xBB, 0xCC};
  const auto bytes = co::EncodeAd(ad);
  const auto back = co::DecodeAd(bytes.data(), bytes.size());
  REQUIRE(back);
  CHECK(back->id == ad.id);
  CHECK(back->host == ad.host);
  CHECK(back->key == ad.key);
  CHECK(back->public_slots == 4);
  CHECK(back->qos == ad.qos);
  CHECK_FALSE(co::DecodeAd(bytes.data(), bytes.size() - 1));  // truncated
  auto many = ad;
  many.public_slots = 8;  // more than the game's 4
  auto m = co::EncodeAd(many);
  CHECK_FALSE(co::DecodeAd(m.data(), m.size()));
}

TEST_CASE("sessions: the host's session data reaches guests, also one joining later", "[net][session][coop]") {
  Host host;
  std::string error;
  REQUIRE(host.Start(LocalConfig(), &error));
  Guest early;
  early.Join(To(host), "Early", "");
  REQUIRE(WaitFor(early, Event::kJoined));
  host.SetSessionData({1, 2, 3});
  REQUIRE(WaitFor(early, Event::kSessionData));
  CHECK(early.SessionData() == Bytes{1, 2, 3});
  host.SetSessionData({4, 5});
  REQUIRE(WaitFor(early, Event::kSessionData));
  CHECK(early.SessionData() == Bytes{4, 5});
  Guest late;
  late.Join(To(host), "Late", "");
  REQUIRE(WaitFor(late, Event::kSessionData));
  CHECK(late.SessionData() == Bytes{4, 5});
  host.Stop();
}

#include <cerrno>

#include "coop/coop_sock.h"
#include "coop/coop_xsession.h"

namespace {
uint32_t BE32(const std::vector<uint8_t>& m, uint32_t a) {
  return uint32_t(m[a]) << 24 | uint32_t(m[a + 1]) << 16 | uint32_t(m[a + 2]) << 8 | m[a + 3];
}
void SetBE32(std::vector<uint8_t>& m, uint32_t a, uint32_t v) {
  for (int i = 0; i < 4; ++i) m[a + i] = uint8_t(v >> (24 - 8 * i));
}
kuf::coop::SessionAd TestAd(const char* ip, uint8_t id, std::vector<uint8_t> qos = {}) {
  kuf::coop::SessionAd ad;
  ad.id[0] = 0x80;
  ad.id[7] = id;
  ad.host = kuf::coop::MakeXnAddr(*kuf::coop::ParseIpv4(ip));
  ad.key[15] = id;
  ad.public_slots = 4;
  ad.filled_public = 1;
  ad.qos = std::move(qos);
  return ad;
}
}  // namespace

TEST_CASE("co-op sockets: Winsock conventions on Linux", "[net][coop]") {
  namespace co = kuf::coop;
  CHECK(co::WsaErrorOf(EAGAIN) == co::kWsaEWouldBlock);
  CHECK(co::WsaErrorOf(EINPROGRESS) == co::kWsaEWouldBlock);  // Winsock: a connect in progress
  CHECK(co::WsaErrorOf(ECONNREFUSED) == co::kWsaEConnRefused);
  CHECK(co::WsaErrorOf(EPIPE) == co::kWsaEShutdown);
  CHECK(co::MapSockOpt(0xFFFF, 0x0004).kind == co::SockOpt::kInt);   // SO_REUSEADDR
  CHECK(co::MapSockOpt(0xFFFF, 0x0080).kind == co::SockOpt::kLinger);
  CHECK(co::MapSockOpt(0xFFFF, 0x1006).kind == co::SockOpt::kTimeout);
  CHECK(co::MapSockOpt(0xFFFF, 0x5801).kind == co::SockOpt::kAccept);
  CHECK(co::MapSockOpt(6, 1).kind == co::SockOpt::kInt);             // TCP_NODELAY
  CHECK(co::MapSockOpt(0xFFFF, 0x9999).kind == co::SockOpt::kUnknown);
  uint8_t sa[16];
  co::WriteGuestSockaddr(sa, {0x7F00000A, 37000});
  CHECK(sa[0] == 0);
  CHECK(sa[1] == 2);  // AF_INET, big-endian
  CHECK(sa[4] == 127);
  const auto back = co::ReadGuestSockaddr(sa, 16);
  REQUIRE(back);
  CHECK(back->ip == 0x7F00000A);
  CHECK(back->port == 37000);
  CHECK_FALSE(co::ReadGuestSockaddr(sa, 4));
  sa[1] = 23;  // another family
  CHECK_FALSE(co::ReadGuestSockaddr(sa, 16));
  std::vector<uint8_t> set(4 + 4 * 64);
  co::WriteFdSet(set.data(), {0x10, 0x20});
  CHECK(co::ReadFdSet(set.data()) == std::vector<uint32_t>{0x10, 0x20});
  SetBE32(set, 0, 1000);  // a corrupt count is bounded
  CHECK(co::ReadFdSet(set.data()).size() == co::kFdSetMax);
  std::vector<uint8_t> tv(8);
  SetBE32(tv, 0, 1);
  SetBE32(tv, 4, 500);
  CHECK(co::TimeoutMs(tv.data()) == 1001);
  CHECK(co::TimeoutMs(nullptr) == -1);
}

TEST_CASE("co-op XSession: session info and search results in guest memory", "[net][coop]") {
  namespace co = kuf::coop;
  std::vector<uint8_t> m(8192, 0xEE);
  const auto ad = TestAd("127.0.0.10", 7);
  co::WriteSessionInfo(m.data(), 0x100, ad);
  const auto info = co::ReadSessionInfo(m.data(), 0x100);
  CHECK(info.id == ad.id);
  CHECK(info.host == ad.host);
  CHECK(info.key == ad.key);
  // The query: two contexts, a numeric property and a string one (not given back).
  co::SearchQuery q{2, 0x200, 2, 0x300};
  std::fill(m.begin() + 0x200, m.begin() + 0x200 + 48, 0);
  m[0x200 + 8] = 1;   // INT32
  m[0x218 + 8] = 4;   // UNICODE: points into the game's memory
  SetBE32(m, 0x300, 0x11);
  SetBE32(m, 0x304, 2);
  SetBE32(m, 0x308, 0x12);
  SetBE32(m, 0x30C, 3);
  const uint32_t need = co::SearchResultsSize(m.data(), 1, q);
  CHECK(need == co::kSearchHeaderSize + co::kSearchResultSize + 2 * co::kContextSize + co::kPropertySize);
  CHECK(co::WriteSearchResults(m.data(), 0x400, need - 1, {ad}, q) == co::kErrorInsufficientBuffer);
  CHECK(m[0x400] == 0xEE);  // nothing written
  REQUIRE(co::WriteSearchResults(m.data(), 0x400, need, {ad}, q) == 0);
  CHECK(BE32(m, 0x400) == 1);
  const uint32_t r = BE32(m, 0x404);
  CHECK(r == 0x408);
  CHECK(co::ReadSessionInfo(m.data(), r).host == ad.host);
  CHECK(BE32(m, r + 60) == 3);  // open public slots
  CHECK(BE32(m, r + 68) == 1);  // filled
  CHECK(BE32(m, r + 76) == 1);  // the numeric property only
  CHECK(BE32(m, r + 80) == 2);
  const uint32_t ctx = BE32(m, r + 88);
  CHECK(BE32(m, ctx) == 0x11);
  CHECK(BE32(m, ctx + 12) == 3);
  CHECK(ctx + 2 * co::kContextSize <= 0x400 + need);
  // No sessions: a header with no results.
  REQUIRE(co::WriteSearchResults(m.data(), 0x800, 8, {}, q) == 0);
  CHECK(BE32(m, 0x800) == 0);
  CHECK(BE32(m, 0x804) == 0);
}

TEST_CASE("co-op QoS and arbitration answers", "[net][coop]") {
  namespace co = kuf::coop;
  std::vector<uint8_t> m(4096, 0xEE);
  const auto ad = TestAd("127.0.0.10", 1, {1, 2, 3, 4, 5});
  auto foreign = co::MakeXnAddr(1);
  foreign[16] = 'Z';
  const std::vector<co::XnAddr> targets = {ad.host, co::MakeXnAddr(*co::ParseIpv4("127.0.0.12")), foreign};
  const uint32_t size = co::QosSize(targets, {ad});
  CHECK(size == 8 + 3 * co::kQosInfoSize + 5);
  co::WriteQos(m.data(), 0x100, targets, {ad}, 20);
  CHECK(BE32(m, 0x100) == 3);
  CHECK(BE32(m, 0x104) == 0);
  const uint32_t q0 = 0x108, q1 = q0 + co::kQosInfoSize, q2 = q1 + co::kQosInfoSize;
  CHECK(m[q0] == (co::kQosComplete | co::kQosContacted | co::kQosDataReceived));
  CHECK((m[q0 + 6] << 8 | m[q0 + 7]) == 5);
  const uint32_t data = BE32(m, q0 + 8);
  CHECK(data == q2 + co::kQosInfoSize);
  CHECK(m[data + 4] == 5);
  CHECK(m[q1] == (co::kQosComplete | co::kQosContacted));
  CHECK(m[q2] == co::kQosComplete);  // not an address of this scheme: not reachable
  const std::vector<uint32_t> ips = {*co::ParseIpv4("127.0.0.10"), *co::ParseIpv4("127.0.0.11")};
  CHECK(co::WriteRegistration(m.data(), 0x400, 8, ips) == co::kErrorInsufficientBuffer);
  REQUIRE(co::WriteRegistration(m.data(), 0x400, co::RegistrationSize(2), ips) == 0);
  CHECK(BE32(m, 0x400) == 2);
  const uint32_t reg = BE32(m, 0x404);
  CHECK((uint64_t(BE32(m, reg)) << 32 | BE32(m, reg + 4)) == co::MachineIdOf(ips[0]));
  const uint32_t users = BE32(m, reg + co::kRegistrantSize + 16);
  CHECK((uint64_t(BE32(m, users)) << 32 | BE32(m, users + 4)) == co::XuidOf(ips[1]));
}

TEST_CASE("co-op on one machine: distinct loopback addresses, the advertisement over the lobby", "[net][coop]") {
  namespace co = kuf::coop;
  // The host's lobby on 127.0.0.10; the guest's game connection from 127.0.0.11, as the runtime binds it.
  HostConfig cfg = LocalConfig();
  cfg.bind_address = "127.0.0.10";
  Host host;
  std::string error;
  REQUIRE(host.Start(cfg, &error));
  Guest guest;
  guest.Join({"127.0.0.10", host.Port(), 0}, "Guest", "");
  REQUIRE(WaitFor(guest, Event::kJoined));
  const auto ad = TestAd("127.0.0.10", 3, {9, 8, 7});
  host.SetSessionData(co::EncodeAd(ad));
  REQUIRE(WaitFor(guest, Event::kSessionData));
  const auto data = guest.SessionData();
  const auto got = co::DecodeAd(data.data(), data.size());
  REQUIRE(got);
  CHECK(got->id == ad.id);
  CHECK(co::IpOfXnAddr(got->host.data()) == *co::ParseIpv4("127.0.0.10"));
  CHECK(got->qos == ad.qos);
  host.Stop();

  // A game-port listener on the host's address; the guest's socket bound to its own address first.
  const int ls = socket(AF_INET, SOCK_STREAM, 0), cs = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(ls >= 0);
  REQUIRE(cs >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(*co::ParseIpv4("127.0.0.10"));
  a.sin_port = 0;
  REQUIRE(bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
  REQUIRE(listen(ls, 5) == 0);
  socklen_t len = sizeof(a);
  getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
  sockaddr_in own{};
  own.sin_family = AF_INET;
  own.sin_addr.s_addr = htonl(*co::ParseIpv4("127.0.0.11"));
  REQUIRE(bind(cs, reinterpret_cast<sockaddr*>(&own), sizeof(own)) == 0);
  REQUIRE(connect(cs, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
  sockaddr_in from{};
  len = sizeof(from);
  const int as = accept(ls, reinterpret_cast<sockaddr*>(&from), &len);
  REQUIRE(as >= 0);
  CHECK(ntohl(from.sin_addr.s_addr) == *co::ParseIpv4("127.0.0.11"));  // the host sees the guest's own address
  close(as);
  close(cs);
  close(ls);
}
