# Native PC co-op: lobbies, invitations, networking

Status (2026-10-31):
- **Lobby layer, lobby service, invitation providers, menu model:** IMPLEMENTED and tested in isolation (loopback
  lobbies, a fake Steam; `kuf_net_tests`).
- **Co-op runtime:** IMPLEMENTED; in the game two loopback instances created, found and joined a session and both
  reached gameplay (PARTIAL: whether they saw each other is not confirmed; the host crashed when the guest left).
- **Menus in the game:** IMPLEMENTED, **on by default** since 2026-11-01 (`--kuf_lobby_menus`, `=false` hides them):
  the player saw and used them in the game (sandboxed, without a network); hosting and joining over a real network
  are not yet tested.
- **Steam:** IMPLEMENTED against SDK 1.65, **not tested with Steam** (only with a fake); off without an App ID.

## What the original game does (Stage A findings, from the recompiled code)

- Co-op is **online Xbox LIVE co-op only**, up to **4 players** (the game's own store text: "4 players co-op").
  There is **no System Link / LAN** mode: no broadcast, `XNetSetSystemLinkPort` is not imported, and the four
  game-type rows (Host Private, Host Public, Quick Match, Co-op Lobby; texts 840/841/842/848) sit behind the LIVE
  privilege gate (`sub_82669770`, MENU_ARCHITECTURE 15).
- Discovery and joining go through **Xbox LIVE sessions** (statically linked XDK wrappers sending XAM messages
  0xB0010 Create, 0xB0011 Delete, 0xB0012 Join, 0xB0013 Leave, 0xB0018 Modify, 0xB0021 Search) and **QoS probes**
  (`XNetQosListen` / `XNetQosLookup`); peers are addressed by **XNet secure addresses** (`XNetXnAddrToInAddr`).
- Gameplay traffic: the game's own **custom engine** (Blueside; no middleware) over **TCP**; the host listens
  (backlog 5) and the others connect; UDP carries voice only. Library code `0x827E9000`-`0x827F6000`; classes
  without RTTI (TCP connection vtable `0x8208FA64`, listener `0x8208FD00`, XNet peer `0x8208FD7C`, session manager
  `0x8208FBB8`, voice `0x8208FEC8`). Lua scripts expose `AmITheHost`, `SendEvent`, network flags: host-centred.
- **Not yet known:** the gameplay packet format, what is replicated (players, enemies, combat, loot), and how a
  guest's character and progress are saved.

The runtime today (ReXGlue SDK): the `NetDll_*` sockets are real host sockets; everything above them is a stub or a
fake: `XNetGetTitleXnAddr` returns loopback, the address conversions and `XNetQosListen` fail, XSession calls do
nothing and find nothing, and `XamUserCheckPrivilege` denies everything (which is why the co-op rows are greyed).

## Architecture chosen

One lobby protocol for every backend; the game's own co-op code for the gameplay (it already synchronises players,
enemies and combat on the console). The port's job is to give that code what Xbox LIVE gave it:

| Layer | Direct connection / LAN | Steam (development: App ID 480) |
|---|---|---|
| Finding a lobby | LAN discovery (UDP broadcast, public lobbies only), direct address, invite code | Steam lobby list (filtered by this project's key and protocol version) |
| Invitations | Invite code `KUF-XXXX-...` (address, port, lobby ID) | Steam overlay invite dialog; accepted invites while running or `+connect_lobby` at launch |
| Joining (password, capacity, version) | the lobby protocol (src/net/session.cpp) | the same protocol, once the transport over Steam exists (not yet) |
| Gameplay | the game's own TCP traffic, through the co-op runtime (src/coop; not yet verified in the game) | the same, once the transport exists |

## The lobby layer (src/net; tests: `kuf_net_tests`)

- **Protocol** (src/net/protocol.cpp): framed messages of at most 16 KB, every length and value checked (2-4
  players, names of at most 32 bytes cleaned of control characters, bounded PBKDF2 effort); a malformed, truncated
  or oversized frame closes the connection.
- **Sessions** (src/net/session.cpp): the host (TCP lobby port 36961, UDP discovery 36962) decides who joins:
  protocol version, capacity, a private lobby only with its ID (from its invite code), and the password. Handshake
  and idle timeouts, heartbeats; only the host starts the game, with at least two players.
- **Passwords:** optional. The host keeps only PBKDF2-HMAC-SHA-256(password, a random salt, 100,000 iterations);
  the guest proves knowledge with HMAC over both sides' nonces and the lobby ID, and the host proves it too. The
  password is never sent, logged or put into lobby metadata. Failed attempts are limited to 5 per address per
  minute. **Limitation:** there is no encryption of the connection; someone who records a join can try passwords
  offline against the recorded proof (slowed by PBKDF2). Use a strong password for a lobby on an untrusted network.
- **LAN discovery** answers only for public lobbies that have not started; answers are tied to the query's nonce.
- **Nothing changes the network:** no router port mapping, no UPnP, no firewall rules. Hosting over the internet
  needs the host to forward TCP port 36961 themselves; the interface will say so.

## Steam (src/net/steam_lobby.cpp, steam_backend.cpp)

- Built only against a Steamworks SDK the player downloaded (`build/tools/steamworks_sdk_*/sdk`, or
  `-DKUF_STEAMWORKS_SDK=...`); never committed or redistributed. Loaded at run time only when an App ID is
  configured, from the `libsteam_api.so` the player points to; the game has no link to Steam and works without it.
- **App ID:** off by default. **480 (Spacewar) only for development tests**, set explicitly; Steam reads it from the
  environment (`SteamAppId`), no `steam_appid.txt` is written. A project App ID, if one is ever obtained, replaces
  it with no other change. Steam is never needed for solo play.
- Lobbies carry `kuf_cod_recomp=1` and the protocol version; lists are filtered by them on Steam and checked again
  locally, so Spacewar's other test lobbies are never shown. Metadata never includes a password.
- **Limitations:** with App ID 480, Steam shows the game as "Spacewar"; an invitation accepted while the game is
  closed makes Steam launch Spacewar, not this game (non-Steam shortcuts are not started by invitations), so that
  path works only when the game is already running. Steam lobbies list and invite; joining the actual game still goes
  through the lobby protocol, which needs a transport over Steam (Steam Networking) that is not yet written.

## Co-op runtime (src/coop; tests: `kuf_net_tests` [coop])

The game's own co-op code runs unchanged; the runtime answers the Xbox LIVE calls it makes (src/coop/coop_runtime.cpp):

| Xbox LIVE piece | Native replacement |
|---|---|
| Player and host identity | XNADDR derived from the instance's IPv4 address (`ina`, a "KUF" mark and the address), so every conversion is a computation, never a table; the "secure" IN_ADDR is the real address; machine ID and XUID (offline range) also follow from the address; the name from `kuf_coop_name` |
| Session IDs, create / join / end | the host's XSessionCreate gets a random session ID (online peer), key and nonce and is **advertised through the lobby** (opaque session data, `MsgType::kSession`) with the host's XNetQosListen data; a guest's XSessionSearch / SearchByID return the advertisement; XSessionDelete withdraws it; Join / Leave / Start / End go to the SDK (which succeeds) |
| QoS, NAT | XNetQosLookup answers from the advertisement (contacted, the host's data, a nominal 20 ms); no NAT traversal: addresses are direct (LAN or loopback) |
| Membership and arbitration | arbitration lists the machines this instance knows (host first, one user each) |
| Privilege and sign-in | multiplayer privilege 254 and the LIVE sign-in bit of the active controller pass, and the two co-op gates (`sub_82669770`, `sub_82668698`); the network library's own sign-in reader (`sub_82827E68`, XUserGetSigninState) reports user 0 as signed in to LIVE, so its sign-in handler counts a LIVE user and session requests go to the real session manager instead of the offline stub (vtable `0x8208F9C0`, which fails a host request with "cannot connect to Xbox LIVE"); the runtime's own sign-in state is unchanged |
| Connection status, sockets | the game's Winsock calls run on the native descriptors with Winsock's conventions (src/coop/coop_sock.cpp): WSAE* errors (would-block, connect in progress), FIONBIO / FIONREAD, socket options, select over handle sets (a failed connect in the exception set), an accepted socket keeps the listener's blocking mode, no SIGPIPE; the SDK's Linux path cannot carry the game's traffic (it passes Winsock codes to Linux, writes sendto's address in host byte order and ignores select's sets) |

Ports: the game's fixed ports 1000-1015 (TCP 1000, UDP 1001) move to `kuf_coop_port_base` (37000 by default; one
base for everyone, because each instance has its own address). Sockets bind to the instance's address, so a peer sees
connections and datagrams coming from it; a broadcast goes to each known peer instead. Robustness: partial reads and
writes, disconnects and timeouts are the game's own (it runs over non-blocking TCP and now gets the right error
codes); malformed lobby frames and advertisements are refused with bounds on every field; a guest that loses the
lobby joins it again every 5 s.

With `kuf_coop` empty (the default) every override forwards to the SDK unchanged. Windows: not available yet (the
socket layer is POSIX); Steam: not used.

### Developer configuration (for tests; not a player setting, never shown in the menus)

| Option | Meaning |
|---|---|
| `--kuf_coop=host` / `join` | the role (empty: off) |
| `--kuf_coop_ip=A.B.C.D` | this instance's address (two instances on one machine: 127.0.0.10 and 127.0.0.11) |
| `--kuf_coop_peer=A.B.C.D` | join: the host's address |
| `--kuf_coop_port_base=37000` | where ports 1000-1015 go |
| `--kuf_coop_lobby_port=36961` | the host's lobby port |
| `--kuf_coop_name=...` | the player name (15 characters at most) |

### Two-instance test (only with the player's permission)

`tools/run_coop_pair.sh -n <prefix> -t <seconds> --gpu --window [--host-saves dir] [--guest-saves dir]` starts a
host (127.0.0.10) and, 5 s later, a guest (127.0.0.11), each in the usual sandbox, both inside one network namespace
of their own that has **only loopback** (an outer bubblewrap with `--unshare-net`; `run_sandboxed.sh --share-net`
refuses any namespace with another interface). The machine's network, router and firewall are never reachable or
changed. Each instance has its own run directory (`build/runs/<prefix>-host`, `-guest`) and its own saves, copied
in from the given directories (sources only read, nothing written back). Results: the `[kuf-coop]` lines of each
`kuf_cod.log`. A TCP connection between the two is **not** proof of co-op: the test is passed only when both
players are in the same world and see each other move and fight.

### Test results

- **Two-instance test 1 (2026-10-31, coop-2, the player at both windows, 2 minutes):** both instances launched in
  co-op mode; the guest joined the host's lobby; the co-op rows were available; **Host failed with the game's
  "cannot connect to Xbox LIVE" box** before any session or socket call: the Host action's request 0x4110C (the
  0x411xx handler `sub_826EFA38`) goes to the object from `sub_827EAD10`, the offline stub when the library counts no
  LIVE user. Fixed statically (the sign-in reader above); not yet re-tested. No crash; source saves unchanged; each
  instance wrote a new character file in its own copy (KD_1392, RG_1843).

- **Two-instance test 2 (2026-10-31, coop-3, after 6f823a5):** the host created, started and advertised its session
  (QoS data 160 bytes) and listened on game port 1000; the guest's search found it, its QoS lookup answered, it joined
  and connected (the host accepted 127.0.0.11); both were in gameplay for about 2.5 minutes (whether they saw each
  other: not confirmed). When the guest closed its window the host **crashed** (a texture slot read through null in
  `sub_82505BF0`, the render thread) right after the session's Leave (0xB0013, not implemented by the SDK);
  XLIVEBASE message 0x5800E at each join is not implemented either. Source saves unchanged.

### Saves in co-op

Every instance saves to its own package in its own content directory, so one instance can never write into another's
(docs/SAVES.md). Not yet known (needs the two-instance test): whether a guest's character keeps what it earned in the
host's world (on the console, each player's own console saves their own character), and whether the host writes any
file for a remote player (character files are named per class and number, `KD_1350`, so a host-side write could
collide with the host's own character: tests use disposable copies only, and `tools/kuf_save.py` compares the
packages afterwards).

## Lobby service (src/coop/lobby_service.cpp)

One interface between the menus, the native lobby (src/net) and the co-op runtime; invitation backends are
providers behind it, so the menus never depend on one:

```
menus (lobby_menu model, lobby_screen renderer)
   -> LobbyService: Create, Join by code, Browse, Join, Leave, Invite Friends, Start Game
        -> net::Host / net::Guest: the native lobby (joining, password, capacity, the game's session data)
        -> InviteProvider: Direct (LAN discovery, invite codes; always), Steam (only when configured)
   <-> co-op runtime: the role (a lobby open = co-op active), the host's session advertisement, the host's address
```

- **Create:** name, the game's own difficulty (it follows the character, KNOWN_ISSUES K61), 2-4 players,
  visibility (Public: listed on the LAN / Steam; Friends Only: Steam only; Private: only with the invite code),
  optional password (never shown, sent or logged), the invitation backend.
- **Start Game** asks the game for its own co-op session (the game's Host Public / Private row); the lobby counts as
  started only when that session exists (published by the runtime), else the page shows that it failed. A guest is
  handed to the game's own join (its Quick Match: the search returns the host's session) when that session arrives.
- **Browse** shows only lobbies a working provider found (LAN: public lobbies that answered; Steam: compatible
  lobbies); unavailable providers are listed with their reason; nothing is invented.

## Invite codes

`KUF-XXXX-XXXX-...` (Crockford base32 with a check digit): the host's IPv4 address, its lobby port, the lobby's ID.
- **Reach:** a code says where it works: *This computer only* (loopback), *LAN only* (a private address: the
  default), *Internet* only when the player sets `--kuf_public_address` to their own public address and forwards the
  lobby port (TCP 36961) and the game ports (`kuf_coop_port_base`, default 37000 and 37001, TCP and UDP) themselves.
  There is no discovery service and no NAT traversal: a code never claims more.
- **Validation:** a malformed code (wrong letters, length, check digit) is refused before anything is sent; a code of
  a closed lobby is refused by the host (another lobby ID) or finds nothing ("no longer valid").

## How an invitation reaches the game

1. **Steam (host):** the lobby service announces the lobby to the Steam provider, which creates a Steam lobby: its
   listed metadata has the project key, protocol version, name, host, difficulty, players and whether there is a
   password (**never an address or a password**); the native invite code is the owner's **member data**, which only
   the lobby's members can read.
2. **Steam (guest):** an accepted invitation (the overlay's GameLobbyJoinRequested, or `+connect_lobby <id>` at
   launch) or a Steam browser entry is entered on Steam; once the owner's member data arrives (LobbyDataUpdate) the
   code is handed to the service, which joins the **native** lobby with it. Steam is not in the gameplay.
3. **Native lobby:** the guest is admitted (password, capacity, version); it receives the host's session data.
4. **Game:** the guest's menu selects the game's own Quick Match; the co-op runtime answers its XSessionSearch with
   the host's session (the host's address replaced by the one the lobby was reached at), and the game connects.
Rejections (password, full, closed, started) and Steam failures are shown on the page; a failed enter or a lost
connection never counts as a join.

## Steam requirements

- An **authorized App ID** (`--kuf_steam_app_id`; empty = off). 480 (Spacewar) only for development tests; never a
  default and never in a release configuration. No other title's identity, no emulator, no authentication bypass.
- The player's own `libsteam_api.so` (`--kuf_steam_api_library`) from the Steamworks SDK; this project ships no
  Steamworks files. Built only when an SDK is present at build time.
- Without them the Create page shows Invitations: *invite code* with the reason, Friends Only is not offered, Invite
  Friends is not shown, and the browser lists Steam as off with the reason. Solo play never needs Steam.

## Menus (src/coop/lobby_menu.cpp, src/runtime/lobby_screen.cpp)

Drawn in the game type screen's own six rows (scene 0x02F), in the game's style: row 0 is the game's Difficulty on
Home, every other page uses all six rows (scrolling with the selection); the description line shows a short
description of the selected row, the last result or error, or while typing the editing hint. A row with a value
(Lobby name:, Max players:, Visibility:, Password:, ...) draws its label and value in two fixed columns: text controls
added to the screen's layout as read (ui_layout.h AddLobbyColumns: copies of the description line, left-aligned
FA_INFOTEXT_L, on each row's line), so typing never moves a label (the rows' own buttons use the right-aligned
FA_BUTTON_R fonts: a "label value" string in them slid with the value); the buttons keep their frames, highlights and
hit boxes; plain actions (Create Lobby, Back) keep the game's own button text. Keyboard, controller and mouse (the menu pointer) as on any menu. A text row (name, password,
code) is edited after a click, Enter or A: all of it selected (typing replaces it), a blinking caret, Backspace,
Delete, Left/Right, Home/End, Ctrl+A, Ctrl+C, Ctrl+V, Enter keeps it, Escape (or B) cancels; the host's password is
shown as typed on Create Lobby (only there: never listed, advertised, logged or copied), a guest's is masked; with a controller Up/Down step the letter at the caret, Right adds one, Left deletes. Messages belong to the
page they were shown on.
Pages: Home (Continue Solo Game, Create Lobby, Browse Lobbies, Join by Code, Back), Create, the lobby (players,
open slots, code and reach, Copy Invite Code, Invite Friends, Start Game, Leave Lobby), Browse, Join by Code,
Password. Copy Invite Link is not offered (no link scheme exists). **On by default since 2026-11-01** (seen working in the game by
the player; `--kuf_lobby_menus=false` hides them).

## What needs two computers (checklist)

1. Both on one LAN: the host creates a public lobby; the guest sees it in Browse Lobbies, joins; a private lobby by
   code; a password lobby (wrong, then right password).
2. Start Game: the host reaches its own co-op session; the guest is handed over and enters the same world; both see
   each other move and fight; enemies and damage agree.
3. A guest leaves and joins again; the host leaves (guests are told); the earlier host crash on a guest's leave.
4. Four players (three guests).
5. Saves: each side's character file before and after (`tools/kuf_save.py`); nothing written for another player.
6. Internet (optional): `--kuf_public_address`, the host's port forwarding; an internet code from outside.
7. Steam (only with an authorized App ID): a lobby listed, a friend invited, the invitation accepted with the game
   running and from a closed game (`+connect_lobby`), the native join after it.

## What is missing (in order)

1. **Co-op gameplay: verification.** The runtime above is built and unit-tested; the two-instance test has not been
   run. Unknown until it is: the game's own message framing and types, who simulates enemies, whether guests connect
   to each other (the library exchanges XNADDRs, type 6), the search procedure and contexts the game uses, the
   layout of its QoS data, the listener's messages 2100-2119, and guests' saves. Then up to 4 players.
2. **Menus in the game:** implemented, opt-in; to be seen working (one instance) and then turned on by default.
3. **Transport over Steam:** the lobby protocol over Steam Networking Messages (relayed, no port forwarding).
