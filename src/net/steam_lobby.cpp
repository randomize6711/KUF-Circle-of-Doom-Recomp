#include "net/steam_lobby.h"

#include <charconv>

namespace kuf::net::steam {
namespace {

std::optional<unsigned> Number(const std::map<std::string, std::string>& data, const char* key, unsigned max) {
  const auto it = data.find(key);
  if (it == data.end() || it->second.empty() || it->second.size() > 10) return std::nullopt;
  unsigned v = 0;
  const auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
  if (ec != std::errc() || end != it->second.data() + it->second.size() || v > max) return std::nullopt;
  return v;
}

}  // namespace

int LobbyTypeFor(Visibility v) {
  switch (v) {
    case Visibility::kPrivate: return 0;  // k_ELobbyTypePrivate: only by invitation
    case Visibility::kFriends: return 1;  // k_ELobbyTypeFriendsOnly
    default: return 2;                    // k_ELobbyTypePublic
  }
}

std::map<std::string, std::string> LobbyMetadata(const LobbyInfo& info) {
  return {
      {kProjectKey, kProjectValue},
      {"protocol", std::to_string(kProtocolVersion)},
      {"name", CleanName(info.name, "Lobby")},
      {"host", CleanName(info.host, "Host")},
      {"difficulty", std::to_string(info.difficulty)},
      {"max_players", std::to_string(info.max_players)},
      {"password", info.password ? "1" : "0"},  // whether there is one; never the password
      {"started", info.started ? "1" : "0"},
  };
}

std::optional<LobbyInfo> InfoFromMetadata(const std::map<std::string, std::string>& data, uint64_t lobby_id,
                                          int members, int member_limit) {
  const auto project = data.find(kProjectKey);
  if (project == data.end() || project->second != kProjectValue) return std::nullopt;  // not this project's
  const auto protocol = Number(data, "protocol", 65535);
  if (!protocol || *protocol != kProtocolVersion) return std::nullopt;
  const auto difficulty = Number(data, "difficulty", 7);
  const auto max_players = Number(data, "max_players", kMaxPlayers);
  const auto password = Number(data, "password", 1);
  const auto started = Number(data, "started", 1);
  if (!difficulty || !max_players || !password || !started || *max_players < kMinPlayers) return std::nullopt;
  if (members < 1 || members > int(*max_players) || (member_limit > 0 && member_limit > int(kMaxPlayers))) {
    return std::nullopt;
  }
  LobbyInfo info;
  info.id = lobby_id;
  const auto name = data.find("name"), host = data.find("host");
  info.name = CleanName(name == data.end() ? "" : name->second, "Lobby");
  info.host = CleanName(host == data.end() ? "" : host->second, "Host");
  info.difficulty = uint8_t(*difficulty);
  info.max_players = uint8_t(*max_players);
  info.players = uint8_t(members);
  info.password = *password == 1;
  info.started = *started == 1;
  info.backend = Backend::kSteam;
  return info;
}

std::optional<uint32_t> ParseAppId(std::string_view text) {
  if (text.empty() || text == "0") return uint32_t(0);
  uint32_t v = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc() || end != text.data() + text.size()) return std::nullopt;
  return v;
}

std::optional<uint64_t> ConnectLobbyArgument(const std::vector<std::string>& args) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] != "+connect_lobby") continue;
    uint64_t v = 0;
    const std::string& s = args[i + 1];
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc() && end == s.data() + s.size() && v != 0) return v;
  }
  return std::nullopt;
}

}  // namespace kuf::net::steam
