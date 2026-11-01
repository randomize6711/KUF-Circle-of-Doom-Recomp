// The Steam backend of the lobbies (docs/MULTIPLAYER.md "Steam"): Steam lobbies for listing, joining and friend
// invitations. Optional in every way: built only against a Steamworks SDK the player downloaded
// (-DKUF_STEAMWORKS_SDK=...), loaded at run time only when an App ID is configured (--kuf_steam_app_id; 480,
// Spacewar, for development tests only, never the default) from the library the player points to
// (--kuf_steam_api_library: Valve's libsteam_api.so is not redistributed by this project). Without it the game and
// the direct-connection lobbies work as before. The game thread calls RunCallbacks each tick and takes the events.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/protocol.h"

namespace kuf::net::steam {

struct SteamEvent {
  enum Kind {
    kCreated,         // lobby (our own, we are its owner)
    kCreateFailed,
    kList,            // lobbies: the compatible lobbies found
    kEntered,         // lobby
    kEnterFailed,
    kMembersChanged,  // lobby
    kInviteAccepted,  // lobby: a friend's invitation accepted (in the overlay or from the command line)
    kDataChanged,     // lobby: its data or a member's data changed (the owner's invite code may have arrived)
  } kind = kList;
  uint64_t lobby = 0;
  std::vector<LobbyInfo> lobbies;
  std::string text;
};

// True if this build includes the Steam backend (the SDK was present at build time).
bool Built();
// Starts Steam for `app_id` with the library at `library`; false with the reason (Steam not running, no library,
// not built, the App ID refused). Never needed for playing.
bool Start(uint32_t app_id, const std::string& library, std::string* error);
void Shutdown();
bool Running();
std::string PlayerName();

void RunCallbacks();
std::vector<SteamEvent> TakeEvents();

// Asynchronous; the results come as events.
bool CreateLobby(const LobbyInfo& info);
bool RequestLobbies();
bool JoinLobby(uint64_t lobby);
void LeaveLobby();
// Updates our lobby's metadata (players, started); only its owner can.
bool UpdateLobby(const LobbyInfo& info);
// The native connection behind a Steam lobby: the owner's invite code (the direct lobby's address, port and ID), kept
// as the owner's *member* data, which only the lobby's members can read (never in the lobby's listed metadata, so a
// lobby list never shows anyone's address). The owner sets it once the lobby exists; a member reads it after entering.
constexpr const char* kInviteKey = "invite";
bool SetOwnInvite(const std::string& code);
std::string OwnerInvite();  // "" until the owner's data has arrived (kDataChanged)
// Opens Steam's overlay to invite friends to our lobby; false if the overlay is not available.
bool OpenInviteDialog();
// The lobby from the command line (+connect_lobby), reported as kInviteAccepted once Steam runs.
void NoteLaunchArguments(int argc, char** argv);

}  // namespace kuf::net::steam
