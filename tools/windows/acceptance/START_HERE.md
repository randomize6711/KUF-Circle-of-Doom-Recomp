# KUF: Circle of Doom on Windows: the acceptance test (M17)

This folder tests the Windows version on your own PC, as far as a program can, and packs the results for the Linux
PC. It is the automated part of `WINDOWS_TEST.md`.

## What it touches

- **Only new folders** under `work\` here (or `-WorkDir`), which you can delete afterwards: the portable edition
  extracted into a folder with non-English letters, a test installation of the setup (its data in `work\` too),
  and every game run with its own copy of the test saves.
- **Your game files are only read** (copied into the test folders).
- **Your real saves, settings and installation are not used.** If the setup is already installed for real, the
  test installs without shortcuts and without the Settings > Apps entry, so the real ones stay as they are.
- One online request (skip it with `-NoNetwork`): the updater asks GitHub for the newest release, as the game's
  Check for Updates does.
- Windows may ask whether to allow `kuf_lobby_peer.exe` through the firewall (the lobby test). Cancel is fine:
  that test only uses this PC itself.

## Running it

1. About 30 GB free on the drive with this folder; about 1.5 hours (plus about 1 hour with `-Build`).
2. Double-click `Run-Acceptance.cmd` (a normal window: **not** "Run as administrator").
3. Drag your game folder (with `default.xex`, as dumped) or your `.iso` into the window and press Enter.
4. Press Enter to start, then **do not touch the keyboard or mouse** until it says "Done": it runs the game many
   times, presses Alt+Tab and drags a folder onto the launcher by itself.
5. Bring `results-<time>.zip` (here, next to this file) back to the Linux PC.

Options (from a PowerShell window in this folder, e.g. `.\Run-Acceptance.cmd -Controller`):

| Option | |
|---|---|
| `-Controller` | also the controller checks: plug, unplug and press when it says so (an Xbox controller) |
| `-Build` | also the native build (E1): Git for Windows, CMake 3.28+, Ninja and Python 3 must be installed |
| `-Only setup,drop` | only some parts: `tests portable scenarios restart update move drop setup controller build` |
| `-GameDir "D:\KUF"` | the game without asking |
| `-NoNetwork` | no online request |

## Still by hand afterwards (a few minutes)

- Drag your game folder **from Explorer** onto `Play KUF.exe`'s window once (the test drags from its own helper
  window: the same Windows mechanism, but not Explorer itself).
- Start the game from the Start menu entry and press Play (the test checks the entry opens the launcher).
- C3: start the game without the controller, plug it in at the title menu: it works.
- E2 for real: disable every audio output in Sound settings, start the game: it runs silently.
- E3: co-op with the Linux PC, when both are on at the same time (docs/MULTIPLAYER.md in `source\`).

## For Claude on Windows

Read `source\kuf-cod-recomp\AGENTS.md` first. Its rules apply here:
- **Never push**, never add a remote, never merge, tag, sign or publish.
- The game folder is read-only.
- Use only disposable folders.

The harness (`acceptance\`) has **never run on Windows** before this: it was written on Linux, where neither
PowerShell nor C# can run. If the harness itself fails, fix it in `source\kuf-cod-recomp\tools\windows\acceptance\`
and copy it here.

When a check fails because of the game, setup or launcher:

1. Read its log under `results\<time>\` (`logs\`, `runs\<run>\kuf_cod.log`, `shots\`).
2. Fix it in `source\kuf-cod-recomp`, on branch `m17/cross-platform-stabilization`, one commit per fix. The fix must
   not change Linux behaviour.
3. Build with `tools/windows/build.sh` in Git Bash (docs/WINDOWS_BUILD.md).
4. Make the packages with `tools/package/make_windows_portable.sh` and `make_windows_setup.sh`, put them into
   `packages\`, and run the failed part again with `-Only`.

To bring the fixes back, from Git Bash in `source/kuf-cod-recomp`:

```sh
git bundle create ../../m17-windows-fixes.bundle <the kit's commit>..HEAD
```

The kit's commit is in `KIT.txt`.
