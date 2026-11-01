// Native co-op runtime (docs/MULTIPLAYER.md "Co-op runtime"): what Xbox LIVE gave the game's own co-op code, over
// native sockets. Active while a lobby is (created or joined in the menus, or by the developer configuration kuf_coop
// = host | join); otherwise every override forwards to the SDK unchanged.
#pragma once

#include <cstdint>
#include <string>

#include <rex/ppc.h>

namespace kuf::coop {

// The developer configuration is complete and co-op is on (host or join).
bool Enabled();
// The lobby's events (the providers', Steam callbacks), its state changes to the log, a developer join's retry: every
// tick (frame_pacing) and from the game type screen's poll.
void Maintain();

class LobbyService;
// The lobby service the menus drive (coop/lobby_service.h). With a lobby open co-op is Enabled().
LobbyService& Lobby();
// The player's name in lobbies (kuf_coop_name, else derived from the address).
std::string PlayerName();

using GuestImportFn = void (*)(PPCContext&, uint8_t*);
// Import-audit builds wrap every import and ask this for ours (runtime/import_audit.cpp).
GuestImportFn ImportOverride(const char* symbol);

}  // namespace kuf::coop
