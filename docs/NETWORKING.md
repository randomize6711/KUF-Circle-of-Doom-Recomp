# Networking, invitations and multiplayer: assessment (M7 feature 7)

Status (2026-10-30): **blocked**. Nothing here is implemented; this file records what is known, what blocks the
feature, and the groundwork that does not depend on the blockers. ROADMAP M11-M13 hold the plan.

## What the game has
- The title list has no multiplayer entry (NEW GAME, CONTINUE, LEADERBOARD, SETTINGS, LANGUAGE, PROLOGUE, CREDITS):
  the console's co-op is reached from inside the game through Xbox Live / System Link sessions (not yet traced).
- Imports (config/v1/imports.txt): the Winsock-style `NetDll_*` sockets (18), the XNet address and QoS functions
  (`XNetStartup`, `XNetGetTitleXnAddr`, `XNetXnAddrToInAddr`, `XNetInAddrToXnAddr`, `XNetQosListen`,
  `XNetQosLookup`, ...), Xbox Live sessions (`XamSessionCreateHandle`, `XamSessionRefObjByHandle`), the
  console's friends UI (`XamShowFriendsUI`) and sign-in state (`XamUserGetSigninState`).
- In single-player only `NetDll_WSAStartup` and `NetDll_XNetStartup` are called (docs/IMPORT_AUDIT.md); both
  succeed and the port stays offline. No other network path has been reached in any test.

## Blockers
1. **Steam invitations: no authorised AppID.** Steam's invite and rich-presence API (Steamworks) works only
   for an application registered on Steam; this game is not sold there, and using another title's AppID (such as
   the Spacewar test ID 480) for a released build is not permitted. Using the Steamworks SDK also requires
   accepting Valve's SDK agreement with the player's own account. **Needs the player's decision**; ROADMAP M13
   already says invites are optional and the core never depends on them.
2. **Multiplayer: the game's network layer is not reverse-engineered.** Co-op needs the session, matchmaking and
   peer-to-peer protocol the game runs over XNet (secure addresses, QoS probes, Xbox Live sessions) reimplemented
   over LAN or IP sockets (ROADMAP M11 LAN, M12 Internet), with the game's own replication working between two
   recompiled instances. This is milestone-sized work, not a feature of this roadmap.
3. **Steam Deck "Invite Friend" without Steam:** an invite needs a session to join (blocker 2).

## Groundwork done (independent of the blockers)
- The title list can take more rows (src/runtime/title_menu.cpp, ui_layout.h AddTitleMainRows): an
  **Invite Friend** row is one more entry in its row table once an invitation service exists; it is deliberately
  not shown before then (no button that does nothing).
- The package and updater (docs/STEAM_DECK.md) run the game as a non-Steam shortcut on a Steam Deck; Steam's own
  overlay invites do not apply to non-Steam games without blocker 1.

## Next steps when unblocked
- M11: trace the in-game co-op entry (the menus that call `XamSessionCreateHandle` and the QoS functions) with
  `--kuf_trace_menu`, document the protocol, and implement XNet over UDP sockets for two instances on one machine
  (an automated two-instance test).
- M13 (only with an authorised AppID and the player's approval): a small invitation interface (create / join a
  lobby, invite a friend, join from an invite) with Steam as one implementation, used by the title list's Invite
  Friend row and the in-game co-op menu.
