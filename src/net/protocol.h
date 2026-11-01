// The lobby protocol (docs/MULTIPLAYER.md), shared by every backend (direct connection / LAN, Steam): the lobby's
// description, the messages between a host and its guests, and their encoding. Every message is a frame
// [u32 length][u8 type][payload], at most kMaxFrame bytes; decoding checks every length and value and refuses
// anything else (a malformed or oversized message closes the connection). Pure: tests/net.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/crypto.h"

namespace kuf::net {

constexpr uint32_t kMagic = 0x4B554643;  // "KUFC": a Kingdom Under Fire: Circle of Doom recompilation lobby
constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kMaxFrame = 16 * 1024;
constexpr size_t kMaxName = 32;          // lobby and player names (UTF-8 bytes)
constexpr uint8_t kMaxPlayers = 4;       // the game's co-op limit (its own store text: "4 players co-op")
constexpr uint8_t kMinPlayers = 2;
constexpr size_t kNonceSize = 16;
constexpr size_t kSaltSize = 16;
constexpr uint32_t kPbkdf2Iterations = 100000;

enum class Visibility : uint8_t { kPublic = 0, kFriends = 1, kPrivate = 2 };
enum class Backend : uint8_t { kDirect = 0, kSteam = 1 };

// What a lobby shows about itself (never its password; only whether it has one).
struct LobbyInfo {
  uint64_t id = 0;
  std::string name;
  std::string host;
  uint8_t difficulty = 0;  // the game's difficulty index
  uint8_t players = 1;
  uint8_t max_players = kMaxPlayers;
  Visibility visibility = Visibility::kPublic;
  bool password = false;
  Backend backend = Backend::kDirect;
  uint16_t port = 0;       // direct connections: the host's lobby port
  bool started = false;    // the game has begun (no more joining)
};

struct Member {
  uint8_t index = 0;  // 0: the host
  std::string name;
};

enum class MsgType : uint8_t {
  kHello = 1,      // guest -> host
  kChallenge = 2,  // host -> guest
  kAuth = 3,       // guest -> host
  kWelcome = 4,    // host -> guest
  kReject = 5,     // host -> guest, then closed
  kLobby = 6,      // host -> guests: the lobby and its members
  kLeave = 7,      // either way
  kPing = 8,
  kPong = 9,
  kStart = 10,     // host -> guests
  kSession = 11,   // host -> guests: the game's session data (opaque to the lobby; docs/MULTIPLAYER.md)
};
constexpr size_t kMaxSessionData = 4096;

enum class RejectReason : uint8_t {
  kVersion = 1,     // another protocol version
  kFull = 2,
  kPassword = 3,    // wrong password
  kStarted = 4,     // the game has begun
  kClosed = 5,      // the host closed the lobby
  kMalformed = 6,
  kTimeout = 7,
  kNotInvited = 8,  // a private lobby needs its code
};
const char* RejectText(RejectReason r);

using Nonce = std::array<uint8_t, kNonceSize>;
using Salt = std::array<uint8_t, kSaltSize>;

struct Hello {
  uint16_t version = kProtocolVersion;
  std::string player;
  Nonce client_nonce{};
  uint64_t lobby_id = 0;  // the lobby the guest means (from the browser or the invite code); 0: whichever
};
struct Challenge {
  uint64_t lobby_id = 0;  // the lobby's ID: both proofs are bound to it
  Nonce server_nonce{};
  Salt salt{};
  uint32_t iterations = kPbkdf2Iterations;
  bool password = false;
};
struct Auth {
  Digest proof{};
};
struct Welcome {
  Digest proof{};  // the host's proof (it knows the password too)
  uint8_t index = 0;
};
struct Reject {
  RejectReason reason = RejectReason::kMalformed;
};
struct LobbyState {
  LobbyInfo info;
  std::vector<Member> members;
};
struct Ping {
  uint32_t seq = 0;
};

// A decoded frame.
struct Message {
  MsgType type = MsgType::kLeave;
  Hello hello;
  Challenge challenge;
  Auth auth;
  Welcome welcome;
  Reject reject;
  LobbyState lobby;
  Ping ping;
  Bytes session;  // kSession
};

Bytes EncodeHello(const Hello& h);
Bytes EncodeChallenge(const Challenge& c);
Bytes EncodeAuth(const Auth& a);
Bytes EncodeWelcome(const Welcome& w);
Bytes EncodeReject(RejectReason r);
Bytes EncodeLobby(const LobbyState& s);
Bytes EncodeSimple(MsgType type, uint32_t seq = 0);  // kLeave, kPing, kPong, kStart
Bytes EncodeSession(const Bytes& data);              // at most kMaxSessionData bytes

// Splits complete frames off the front of `buffer`. Returns false if the buffer holds a frame that is too large or
// malformed (the connection must be closed); `out` gets the frames decoded so far.
bool TakeFrames(Bytes* buffer, std::vector<Message>* out);
std::optional<Message> DecodeFrame(const uint8_t* data, size_t size);  // one whole frame

// The password proofs: key = PBKDF2(password, salt); guest proof = HMAC(key, "guest" | client nonce | server nonce |
// lobby id), host proof the same with "host". The password itself is never sent.
Digest PasswordKey(std::string_view password, const Salt& salt, uint32_t iterations);
Digest GuestProof(const Digest& key, const Nonce& client, const Nonce& server, uint64_t lobby_id);
Digest HostProof(const Digest& key, const Nonce& client, const Nonce& server, uint64_t lobby_id);

// LAN discovery (UDP): a query and the answers of public lobbies on the local network.
Bytes EncodeDiscoveryQuery(const Nonce& nonce);
std::optional<Nonce> DecodeDiscoveryQuery(const uint8_t* data, size_t size);
Bytes EncodeDiscoveryAnswer(const Nonce& nonce, const LobbyInfo& info);
std::optional<LobbyInfo> DecodeDiscoveryAnswer(const uint8_t* data, size_t size, const Nonce& nonce);

// Invite codes for direct connections: the host's IPv4 address, its port and the lobby's ID, as "KUF-XXXX-XXXX-..."
// (Crockford base32, a check digit). Private lobbies are joined only with their code.
struct InviteCode {
  uint32_t ipv4 = 0;  // host byte order
  uint16_t port = 0;
  uint32_t lobby = 0;  // the lobby ID's low 32 bits
};
std::string FormatInviteCode(const InviteCode& c);
std::optional<InviteCode> ParseInviteCode(std::string_view text);

// A name as shown: valid UTF-8 cut to kMaxName bytes, control characters removed; empty becomes `fallback`.
std::string CleanName(std::string_view name, std::string_view fallback);

}  // namespace kuf::net
