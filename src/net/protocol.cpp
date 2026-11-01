#include "net/protocol.h"

#include <cctype>
#include <cstring>

namespace kuf::net {
namespace {

struct Writer {
  Bytes out;
  void U8(uint8_t v) { out.push_back(v); }
  void U16(uint16_t v) {
    U8(uint8_t(v >> 8));
    U8(uint8_t(v));
  }
  void U32(uint32_t v) {
    U16(uint16_t(v >> 16));
    U16(uint16_t(v));
  }
  void U64(uint64_t v) {
    U32(uint32_t(v >> 32));
    U32(uint32_t(v));
  }
  void Raw(const uint8_t* d, size_t n) { out.insert(out.end(), d, d + n); }
  void Str(std::string_view s) {
    const size_t n = std::min(s.size(), kMaxName);
    U8(uint8_t(n));
    Raw(reinterpret_cast<const uint8_t*>(s.data()), n);
  }
};

struct Reader {
  const uint8_t* d;
  size_t n, i = 0;
  bool ok = true;
  bool Need(size_t k) {
    if (!ok || n - i < k) ok = false;
    return ok;
  }
  uint8_t U8() { return Need(1) ? d[i++] : 0; }
  uint16_t U16() {
    const uint16_t hi = U8();
    return uint16_t(hi << 8 | U8());
  }
  uint32_t U32() {
    const uint32_t hi = U16();
    return hi << 16 | U16();
  }
  uint64_t U64() {
    const uint64_t hi = U32();
    return hi << 32 | U32();
  }
  template <size_t N>
  void Raw(std::array<uint8_t, N>* out) {
    if (Need(N)) {
      std::memcpy(out->data(), d + i, N);
      i += N;
    }
  }
  std::string Str() {
    const size_t len = U8();
    if (len > kMaxName || !Need(len)) {
      ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(d + i), len);
    i += len;
    return s;
  }
  bool Done() const { return ok && i == n; }
};

void PutInfo(Writer& w, const LobbyInfo& l) {
  w.U64(l.id);
  w.Str(l.name);
  w.Str(l.host);
  w.U8(l.difficulty);
  w.U8(l.players);
  w.U8(l.max_players);
  w.U8(uint8_t(l.visibility));
  w.U8(l.password ? 1 : 0);
  w.U8(uint8_t(l.backend));
  w.U16(l.port);
  w.U8(l.started ? 1 : 0);
}

bool GetInfo(Reader& r, LobbyInfo* l) {
  l->id = r.U64();
  l->name = CleanName(r.Str(), "Lobby");
  l->host = CleanName(r.Str(), "Host");
  l->difficulty = r.U8();
  l->players = r.U8();
  l->max_players = r.U8();
  const uint8_t vis = r.U8(), pw = r.U8(), backend = r.U8();
  l->port = r.U16();
  const uint8_t started = r.U8();
  if (!r.ok || l->difficulty > 7 || l->max_players < kMinPlayers || l->max_players > kMaxPlayers ||
      l->players < 1 || l->players > l->max_players || vis > 2 || pw > 1 || backend > 1 || started > 1) {
    return false;
  }
  l->visibility = Visibility(vis);
  l->password = pw;
  l->backend = Backend(backend);
  l->started = started;
  return true;
}

Bytes Frame(MsgType type, const Bytes& payload) {
  Writer w;
  w.U32(uint32_t(payload.size() + 1));
  w.U8(uint8_t(type));
  w.Raw(payload.data(), payload.size());
  return w.out;
}

Digest Proof(const char* who, const Digest& key, const Nonce& client, const Nonce& server, uint64_t lobby) {
  Writer w;
  w.Raw(reinterpret_cast<const uint8_t*>(who), std::strlen(who));
  w.Raw(client.data(), client.size());
  w.Raw(server.data(), server.size());
  w.U64(lobby);
  return HmacSha256(key.data(), key.size(), w.out.data(), w.out.size());
}

// Crockford base32 (no I, L, O, U).
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

}  // namespace

const char* RejectText(RejectReason r) {
  switch (r) {
    case RejectReason::kVersion: return "The lobby uses another version of the game.";
    case RejectReason::kFull: return "The lobby is full.";
    case RejectReason::kPassword: return "Wrong password.";
    case RejectReason::kStarted: return "The game in this lobby has already started.";
    case RejectReason::kClosed: return "The host closed the lobby.";
    case RejectReason::kMalformed: return "The connection sent invalid data.";
    case RejectReason::kTimeout: return "The connection timed out.";
    case RejectReason::kNotInvited: return "This lobby is private: join it with its invite code.";
  }
  return "The lobby refused the connection.";
}

std::string CleanName(std::string_view name, std::string_view fallback) {
  std::string out;
  for (size_t i = 0; i < name.size() && out.size() < kMaxName;) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    bool valid = len > 0 && i + len <= name.size();
    for (size_t k = 1; valid && k < len; ++k) valid = (static_cast<unsigned char>(name[i + k]) >> 6) == 0x2;
    if (!valid) {
      ++i;
      continue;
    }
    if (len == 1 && (c < 0x20 || c == 0x7F)) {
      ++i;
      continue;
    }
    if (out.size() + len > kMaxName) break;
    out.append(name.substr(i, len));
    i += len;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  return out.empty() ? std::string(fallback) : out;
}

Bytes EncodeHello(const Hello& h) {
  Writer w;
  w.U32(kMagic);
  w.U16(h.version);
  w.Str(h.player);
  w.Raw(h.client_nonce.data(), h.client_nonce.size());
  w.U64(h.lobby_id);
  return Frame(MsgType::kHello, w.out);
}
Bytes EncodeChallenge(const Challenge& c) {
  Writer w;
  w.U64(c.lobby_id);
  w.Raw(c.server_nonce.data(), c.server_nonce.size());
  w.Raw(c.salt.data(), c.salt.size());
  w.U32(c.iterations);
  w.U8(c.password ? 1 : 0);
  return Frame(MsgType::kChallenge, w.out);
}
Bytes EncodeAuth(const Auth& a) {
  Writer w;
  w.Raw(a.proof.data(), a.proof.size());
  return Frame(MsgType::kAuth, w.out);
}
Bytes EncodeWelcome(const Welcome& m) {
  Writer w;
  w.Raw(m.proof.data(), m.proof.size());
  w.U8(m.index);
  return Frame(MsgType::kWelcome, w.out);
}
Bytes EncodeReject(RejectReason r) {
  Writer w;
  w.U8(uint8_t(r));
  return Frame(MsgType::kReject, w.out);
}
Bytes EncodeLobby(const LobbyState& s) {
  Writer w;
  PutInfo(w, s.info);
  w.U8(uint8_t(std::min<size_t>(s.members.size(), kMaxPlayers)));
  for (size_t i = 0; i < s.members.size() && i < kMaxPlayers; ++i) {
    w.U8(s.members[i].index);
    w.Str(s.members[i].name);
  }
  return Frame(MsgType::kLobby, w.out);
}
Bytes EncodeSession(const Bytes& data) {
  Writer w;
  const size_t n = std::min(data.size(), kMaxSessionData);
  w.U16(uint16_t(n));
  w.Raw(data.data(), n);
  return Frame(MsgType::kSession, w.out);
}

Bytes EncodeSimple(MsgType type, uint32_t seq) {
  Writer w;
  if (type == MsgType::kPing || type == MsgType::kPong) w.U32(seq);
  return Frame(type, w.out);
}

std::optional<Message> DecodeFrame(const uint8_t* data, size_t size) {
  if (size < 5) return std::nullopt;
  Reader head{data, size};
  const uint32_t len = head.U32();
  if (len < 1 || len > kMaxFrame || size_t(len) + 4 != size) return std::nullopt;
  const auto type = MsgType(data[4]);
  Reader r{data + 5, size - 5};
  Message m;
  m.type = type;
  switch (type) {
    case MsgType::kHello: {
      if (r.U32() != kMagic) return std::nullopt;
      m.hello.version = r.U16();
      m.hello.player = CleanName(r.Str(), "Player");
      r.Raw(&m.hello.client_nonce);
      m.hello.lobby_id = r.U64();
      break;
    }
    case MsgType::kChallenge: {
      m.challenge.lobby_id = r.U64();
      r.Raw(&m.challenge.server_nonce);
      r.Raw(&m.challenge.salt);
      m.challenge.iterations = r.U32();
      const uint8_t pw = r.U8();
      // A host may not ask a guest's machine for an unreasonable amount of work.
      if (pw > 1 || m.challenge.iterations < 1000 || m.challenge.iterations > 1000000) return std::nullopt;
      m.challenge.password = pw;
      break;
    }
    case MsgType::kAuth: r.Raw(&m.auth.proof); break;
    case MsgType::kWelcome: {
      r.Raw(&m.welcome.proof);
      m.welcome.index = r.U8();
      if (m.welcome.index >= kMaxPlayers) return std::nullopt;
      break;
    }
    case MsgType::kReject: {
      const uint8_t reason = r.U8();
      if (reason < 1 || reason > 8) return std::nullopt;
      m.reject.reason = RejectReason(reason);
      break;
    }
    case MsgType::kLobby: {
      if (!GetInfo(r, &m.lobby.info)) return std::nullopt;
      const uint8_t count = r.U8();
      if (count < 1 || count > kMaxPlayers || count != m.lobby.info.players) return std::nullopt;
      for (uint8_t i = 0; i < count; ++i) {
        Member mem;
        mem.index = r.U8();
        mem.name = CleanName(r.Str(), "Player");
        if (mem.index >= kMaxPlayers) return std::nullopt;
        m.lobby.members.push_back(std::move(mem));
      }
      break;
    }
    case MsgType::kPing:
    case MsgType::kPong: m.ping.seq = r.U32(); break;
    case MsgType::kSession: {
      const uint16_t n = r.U16();
      if (n > kMaxSessionData || !r.Need(n)) return std::nullopt;
      m.session.assign(r.d + r.i, r.d + r.i + n);
      r.i += n;
      break;
    }
    case MsgType::kLeave:
    case MsgType::kStart: break;
    default: return std::nullopt;
  }
  if (!r.Done()) return std::nullopt;
  return m;
}

bool TakeFrames(Bytes* buffer, std::vector<Message>* out) {
  size_t at = 0;
  while (buffer->size() - at >= 4) {
    const uint8_t* p = buffer->data() + at;
    const uint32_t len = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    if (len < 1 || len > kMaxFrame) return false;
    if (buffer->size() - at < size_t(len) + 4) break;
    auto m = DecodeFrame(p, size_t(len) + 4);
    if (!m) return false;
    out->push_back(std::move(*m));
    at += size_t(len) + 4;
  }
  buffer->erase(buffer->begin(), buffer->begin() + ptrdiff_t(at));
  return true;
}

Digest PasswordKey(std::string_view password, const Salt& salt, uint32_t iterations) {
  return Pbkdf2Sha256(password, salt.data(), salt.size(), iterations);
}
Digest GuestProof(const Digest& key, const Nonce& client, const Nonce& server, uint64_t lobby_id) {
  return Proof("kuf-guest", key, client, server, lobby_id);
}
Digest HostProof(const Digest& key, const Nonce& client, const Nonce& server, uint64_t lobby_id) {
  return Proof("kuf-host", key, client, server, lobby_id);
}

Bytes EncodeDiscoveryQuery(const Nonce& nonce) {
  Writer w;
  w.U32(kMagic);
  w.U16(kProtocolVersion);
  w.U8('Q');
  w.Raw(nonce.data(), nonce.size());
  return w.out;
}
std::optional<Nonce> DecodeDiscoveryQuery(const uint8_t* data, size_t size) {
  Reader r{data, size};
  Nonce n{};
  if (r.U32() != kMagic || r.U16() != kProtocolVersion || r.U8() != 'Q') return std::nullopt;
  r.Raw(&n);
  if (!r.Done()) return std::nullopt;
  return n;
}
Bytes EncodeDiscoveryAnswer(const Nonce& nonce, const LobbyInfo& info) {
  Writer w;
  w.U32(kMagic);
  w.U16(kProtocolVersion);
  w.U8('R');
  w.Raw(nonce.data(), nonce.size());
  PutInfo(w, info);
  return w.out;
}
std::optional<LobbyInfo> DecodeDiscoveryAnswer(const uint8_t* data, size_t size, const Nonce& nonce) {
  Reader r{data, size};
  Nonce n{};
  if (r.U32() != kMagic || r.U16() != kProtocolVersion || r.U8() != 'R') return std::nullopt;
  r.Raw(&n);
  if (!r.ok || !ConstantTimeEqual(n.data(), nonce.data(), n.size())) return std::nullopt;  // an answer to our query
  LobbyInfo info;
  if (!GetInfo(r, &info) || !r.Done() || info.visibility != Visibility::kPublic) return std::nullopt;
  return info;
}

std::string FormatInviteCode(const InviteCode& c) {
  // 80 bits: address (32), port (16), lobby (32); 16 base32 digits, then a check digit (sum mod 32).
  uint8_t bytes[10] = {uint8_t(c.ipv4 >> 24), uint8_t(c.ipv4 >> 16), uint8_t(c.ipv4 >> 8), uint8_t(c.ipv4),
                       uint8_t(c.port >> 8),  uint8_t(c.port),        uint8_t(c.lobby >> 24), uint8_t(c.lobby >> 16),
                       uint8_t(c.lobby >> 8), uint8_t(c.lobby)};
  std::string digits;
  uint32_t acc = 0;
  int bits = 0, sum = 0;
  for (const uint8_t b : bytes) {
    acc = acc << 8 | b;
    bits += 8;
    while (bits >= 5) {
      const int v = int((acc >> (bits - 5)) & 31);
      digits += kAlphabet[v];
      sum += v;
      bits -= 5;
    }
  }
  digits += kAlphabet[sum % 32];
  std::string out = "KUF";
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i % 4 == 0) out += '-';
    out += digits[i];
  }
  return out;
}

std::optional<InviteCode> ParseInviteCode(std::string_view text) {
  std::string digits;
  for (const char ch : text) {
    char c = char(std::toupper(static_cast<unsigned char>(ch)));
    if (c == '-' || c == ' ') continue;
    if (c == 'O') c = '0';  // Crockford's look-alikes
    if (c == 'I' || c == 'L') c = '1';
    digits += c;
  }
  if (digits.rfind("KUF", 0) == 0) digits = digits.substr(3);
  if (digits.size() != 17) return std::nullopt;
  int sum = 0;
  std::vector<int> values;
  for (const char c : digits) {
    const char* at = std::strchr(kAlphabet, c);
    if (!at || c == '\0') return std::nullopt;
    values.push_back(int(at - kAlphabet));
  }
  for (size_t i = 0; i < 16; ++i) sum += values[i];
  if (values[16] != sum % 32) return std::nullopt;
  uint8_t bytes[10] = {};
  uint32_t acc = 0;
  int bits = 0;
  size_t k = 0;
  for (size_t i = 0; i < 16; ++i) {
    acc = acc << 5 | uint32_t(values[i]);
    bits += 5;
    if (bits >= 8) {
      bytes[k++] = uint8_t(acc >> (bits - 8));
      bits -= 8;
    }
  }
  InviteCode c;
  c.ipv4 = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
  c.port = uint16_t(bytes[4] << 8 | bytes[5]);
  c.lobby = uint32_t(bytes[6]) << 24 | uint32_t(bytes[7]) << 16 | uint32_t(bytes[8]) << 8 | bytes[9];
  if (c.port == 0 || c.ipv4 == 0) return std::nullopt;
  return c;
}

}  // namespace kuf::net
