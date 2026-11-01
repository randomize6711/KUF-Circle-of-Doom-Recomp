// The Steam invitation provider (docs/MULTIPLAYER.md "Steam"): Steam lobbies for listing and friend invitations, the
// native lobby for everything else. Off unless an App ID is configured and Steam starts (never needed for solo play;
// App ID 480 Spacewar only for development, never a default; Valve's library is the player's own).
//
//   host:   Announce creates a Steam lobby (its type from the visibility), then keeps its listed metadata (no address)
//           and the owner's member data "invite" (the native invite code, readable by the lobby's members only).
//   guest:  a browser entry or an accepted invitation (overlay, or +connect_lobby at launch) is entered on Steam; the
//           owner's invite code is read and handed to the service, which joins natively. Steam is not in the gameplay.
//
// SteamApi is the part of src/net/steam_backend this needs, so tests drive it with a fake.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "coop/lobby_service.h"
#include "net/steam_backend.h"

namespace kuf::coop {

class SteamApi {
 public:
  virtual ~SteamApi() = default;
  virtual bool Running() const = 0;
  virtual std::string Unavailable() const = 0;  // why not running
  virtual bool CreateLobby(const net::LobbyInfo& info) = 0;
  virtual bool UpdateLobby(const net::LobbyInfo& info) = 0;
  virtual bool SetOwnInvite(const std::string& code) = 0;
  virtual std::string OwnerInvite() = 0;
  virtual bool RequestLobbies() = 0;
  virtual bool JoinLobby(uint64_t lobby) = 0;
  virtual void LeaveLobby() = 0;
  virtual bool OpenInviteDialog() = 0;
  virtual void RunCallbacks() = 0;
  virtual std::vector<net::steam::SteamEvent> TakeEvents() = 0;
};

// The real Steam (steam_backend): started here with `app_id` and `library` (0 / "": off, the reason kept).
std::unique_ptr<SteamApi> MakeSteamApi(uint32_t app_id, const std::string& library);

std::unique_ptr<InviteProvider> MakeSteamProvider(std::unique_ptr<SteamApi> api);

}  // namespace kuf::coop
