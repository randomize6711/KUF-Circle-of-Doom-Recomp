// What the Steam backend puts into and reads from Steam lobbies (docs/MULTIPLAYER.md "Steam"), without Steam itself:
// the lobby's metadata (a project key and the protocol version, so Spacewar's other test lobbies are never listed;
// never a password), the App ID setting (development only: 480 = Spacewar, never the default), and the launch
// arguments of an accepted invitation. Pure: tests/net.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/protocol.h"

namespace kuf::net::steam {

constexpr const char* kProjectKey = "kuf_cod_recomp";    // metadata key every lobby of this project has
constexpr const char* kProjectValue = "1";
constexpr uint32_t kSpacewarAppId = 480;                  // Valve's public test application

// The lobby type Steam is asked for (ELobbyType's values: 0 private, 1 friends only, 2 public).
int LobbyTypeFor(Visibility v);

// Metadata written by the host (key -> value). LobbyInfo.port and .backend are not part of it.
std::map<std::string, std::string> LobbyMetadata(const LobbyInfo& info);
// A lobby's metadata back to its description, or nullopt if it is not a compatible lobby of this project (another
// application's test lobby, another protocol version, or malformed values).
std::optional<LobbyInfo> InfoFromMetadata(const std::map<std::string, std::string>& data, uint64_t lobby_id,
                                          int members, int member_limit);

// The App ID setting: "" or "0" off; a number is used as given (development: 480). nullopt when invalid.
std::optional<uint32_t> ParseAppId(std::string_view text);

// A lobby to join from the command line Steam passes when an invitation is accepted with the game closed
// ("+connect_lobby <steam lobby id>").
std::optional<uint64_t> ConnectLobbyArgument(const std::vector<std::string>& args);

}  // namespace kuf::net::steam
