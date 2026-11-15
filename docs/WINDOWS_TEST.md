# Windows test (M17): what only a real Windows PC can confirm

**Postponed (2026-11-10):** the player has no Windows PC for now; every row below is unverified until this test
runs. M16 showed the game playing and saving on Windows. M17 fixed and automated a lot from the Linux side; these checks
confirm it on Windows. Everything stays in disposable folders: your game dump is only read, and your real saves
are not used (each portable folder has its own `Data\`).

Packages: `KUF-Circle-of-Doom-Portable-<v>-windows-x86_64.zip` and `KUF-Circle-of-Doom-Setup-<v>-windows-x86_64.exe`.
Mark each row ok / problem (with what you saw); send back the table and the logs listed at the end.

**Automated (the acceptance kit, tools/windows/make_acceptance_kit.sh):** `Run-Acceptance.cmd` runs A1-A5, B1-B6,
D1-D6, E2 (SDL's audio unavailable) and, with `-Controller` and `-Build`, C1-C5 (you plug and press when told) and E1,
in disposable folders, then packs `results-<time>.zip`. On the Linux PC, `tools/windows/verify_acceptance.py` checks
its game runs with the same rules as the Linux tests. Left by hand: one drag from Explorer itself, Play from the
Start menu entry, C3, E2 with the outputs really disabled, E3 (see the kit's START_HERE.md).

## A. Portable edition in a Unicode path (M17 fix: the UTF-8 code page)

| # | Check | Result |
|---|---|---|
| A1 | Extract the ZIP into a folder with non-English letters and spaces, e.g. `D:\Spiel Übung\Тест KUF\`. Put your game files (the retail dump as it is) into `GameFiles`. `Play KUF.exe`: "Game files detected" | |
| A2 | Play: the preparation runs to the end (a few minutes) and the game starts | |
| A3 | Quit, start `Play KUF.exe` again: the game starts **without** preparing | |
| A4 | Move the whole folder to another drive or folder (also with Unicode letters), start again: no preparation, your save is there | |
| A5 | Drag your game folder from Explorer onto the launcher window (with an empty `GameFiles`): it is detected; nothing is copied before you confirm | |

## B. The game (one session from the portable folder)

| # | Check | Result |
|---|---|---|
| B1 | In-game Settings (pause menu) open; the Controls page opens; mouse hover and click work on it (M16 crashed here; fixed) | |
| B2 | Save in the game, Quit Game, start again, load the save: everything is there | |
| B3 | Fullscreen and windowed (PC Settings), and back; the picture is right each time | |
| B4 | Alt+Tab out and back, in the menu and in gameplay: the game continues, the mouse is released outside and captured again inside | |
| B5 | Another display resolution in PC Settings, then back | |
| B6 | Start and quit the game 3 times in a row from `Play KUF.exe`: each start and quit is clean | |

## C. Controller (Xbox Elite Series 2 or another Xbox pad)

| # | Check | Result |
|---|---|---|
| C1 | Plugged in before the start: menus and gameplay (sticks, triggers, A/B/X/Y, bumpers, Start/Back) | |
| C2 | Unplug it in gameplay, plug it in again: it works again (no restart) | |
| C3 | Plugged in only after the game started (in the title menu): it works | |
| C4 | Vibration in a fight (and the vibration setting off: none) | |
| C5 | Switching between the controller and keyboard/mouse in a menu: the prompts follow the device used | |

## D. The setup (`...Setup-<v>-windows-x86_64.exe`)

| # | Check | Result |
|---|---|---|
| D1 | Start it: **no** administrator prompt (fixed in M17: the setup's manifest asks for none) | |
| D2 | Drag your game folder (or the ISO, or `default.xex`) from Explorer onto its window: the detected game and the destination are shown; nothing happens before Install | |
| D3 | Install, Play now: the game starts | |
| D4 | The Start menu entry and the Desktop shortcut start the game | |
| D5 | Run the setup again over the installation (repair): it completes, saves kept | |
| D6 | "Uninstall Kingdom Under Fire - Circle of Doom" (Settings > Apps, entry added in M17, or the Start menu entry): with "keep saves", the saves stay; the program, its shortcuts and its Settings > Apps entry are gone | |

## E. Optional

| # | Check | Result |
|---|---|---|
| E1 | Developers only: in Git Bash, a fresh clone, then `tools/windows/build.sh --xex <portable folder>/Data/game/default.xex` (docs/WINDOWS_BUILD.md): `BUILD DONE` | |
| E2 | Start the game with every audio output disabled (Sound settings): it runs silently instead of crashing (M17 fix) | |
| E3 | Co-op between this Windows PC and the Linux PC on the same network (docs/MULTIPLAYER.md): Windows may ask to allow the game through the firewall the first time you host: allow private networks only. Host on one, join on the other, then the other way round | |

## Send back

The tables, `winver`, and from the portable folder's `Data\`: `logs\kuf_cod.log`, `logs\launcher.log`,
`build-cache\build.log` (if the preparation failed), `user\crash-report.txt` (after a crash),
`user\run-summary.json`. For the setup: `%LOCALAPPDATA%\kuf-cod-recomp\data\logs\`.

## Known gaps (not to test yet)

- Check for Updates on Windows: implemented (docs/UPDATES_WINDOWS.md) but no Windows release is signed or published
  yet, so it reports "up to date". A local test release can be made for you on request.
- Steam invitations need an App ID; lobbies by code or LAN browsing are the way to play together.
