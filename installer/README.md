# The graphical setup (Linux)

`KingdomUnderFire-CircleOfDoom-Setup-<version>-x86_64.AppImage`: a game-themed installer for the native Linux build.
It installs the game for the current user (no administrator rights), copies and verifies the player's own game files,
optionally adds an application-menu entry, a desktop shortcut and a Steam (non-Steam game) shortcut, and repairs or
uninstalls an installation. Players' instructions: [docs/INSTALL_GUIDE.md](../docs/INSTALL_GUIDE.md).

## Building

```sh
installer/appimage/build_appimage.sh            # setup + tests + assets + game package + AppDir + AppImage
installer/appimage/build_appimage.sh --skip-package   # reuse build/installer/package (only the setup changed)
```

Needs the game built first (`build/linux-amd64-relwithdebinfo`, see the main README), GTK 3 development files,
`rsvg-convert`, `objdump`, `desktop-file-validate`, `curl` once. Downloads, each pinned and checked by SHA-256 and
cached in `build/tools/`: appimagetool 1.9.1 (github.com/AppImage/appimagetool) and the Cinzel font (SIL Open Font
License, from the Google Fonts repository, with its licence next to it in the assets). Output in `build/installer/`; nothing is published.

The setup alone: `cmake -S installer -B build/installer -G Ninja && ninja -C build/installer`. During development it
runs from the build tree with `build/installer/kuf-setup --assets build/installer/assets --payload <unpacked package>`.

## How it works

| Part | Where |
|---|---|
| The wizard (GTK 3, CSS theme; no animations) | `src/gui/main.cpp`, `assets/theme.css` |
| Installing, repairing, uninstalling (transactional) | `src/core/install.cpp` |
| The game files: the supported version's list (SHA-256, size, path of the 137 files the game reads; `$SystemUpdate` not needed) and the checks | `assets/game_v1.tsv`, `src/core/game_data.cpp` |
| Steam (installations, profiles, `shortcuts.vdf`) | `src/core/steam.cpp`, `src/core/vdf.cpp` |
| XDG folders, quoting, verified copies, system requirements | `src/core/util.cpp` |
| Original artwork (the icon: a sword in a bronze ring; the divider) | `assets/*.svg` (rendered to PNG by the build) |

**Framework:** GTK 3 in C++ (GTK 3 is a build dependency of the project already and is present on every target
desktop; no browser runtime). GTK and the other system libraries are not bundled: they come from the system, as
GPU drivers and Vulkan do for the game. The AppImage contains the setup, its assets, and the package (unpacked): no code
of the game, which is built during the installation (`src/core/localbuild.cpp`, docs/LOCAL_BUILD.md).

**Installation layout** (the updater's, `tools/package/install.sh` / `tools/updater/kuf_update.py`): `versions/<v>`,
`current -> versions/<v>`, `kuf-cod` (the stable launcher: sets `KUF_GAME_DIR` to `game/`, runs `current/kuf-cod.sh`),
`kuf-update`, `update.conf` (the GitHub repository; kept if it exists), `allowed_signers` (the release key, when the
package has one), `game/` (the verified copy of the game files), `share/` (icon; the setup and its assets for
uninstalling), `.kuf-install` (the setup's record: version, the shortcuts and Steam entries it created). Saves and
settings: `$XDG_DATA_HOME/kuf-cod-recomp/data`, outside the installation. The updater works unchanged on it.

**Transactions:** a new installation is built in a hidden staging folder next to the target (`.<name>.kuf-staging-*`)
and renamed into place at the end; every program file is checked against the package's `MANIFEST.sha256`, every game
file against the list while it is copied; a failure or Cancel removes the staging folder only. A target that is not
empty and has no `.kuf-install` is refused (a subfolder is suggested). Over an existing installation it repairs:
the program as the updater installs a version (atomic `current` switch), game files checked and only differing ones
copied again; `update.conf` and `allowed_signers` are kept.

**Shortcuts:** `kuf-cod-recomp.desktop` (and `kuf-cod-recomp-uninstall.desktop`) in `$XDG_DATA_HOME/applications`;
the desktop shortcut in `XDG_DESKTOP_DIR` from `user-dirs.dirs` (not created when that folder does not exist); paths
quoted per the Desktop Entry Specification (validated with `desktop-file-validate` in the tests); each entry carries
`X-KUF-Setup-Root=` so only the setup's own entries are ever replaced or removed. Desktops that ask before starting a
desktop launcher are explained to the player, their setting is not changed.

**Steam:** installations in `~/.local/share/Steam`, `~/.steam/steam|root`, Flatpak (`~/.var/app/com.valvesoftware.
Steam/...`), deduplicated by real path; profiles from `userdata/<id>` with names from `config/loginusers.vdf`; with
several profiles the player picks one (none is preselected). The entry is written into the profile's binary
`shortcuts.vdf`: only with Steam closed (asked; Retry / continue without Steam), after a timestamped backup,
atomically, verified by reading it back (the backup is restored if not), every other entry kept byte for byte; an
unknown value type makes it refuse the file and explain how to add the game by hand. The setup's entry has the tag
`kuf-cod-recomp`: a reinstall updates it, the uninstaller removes only it.

**System check** (`src/core/system_check.cpp`, `src/core/util.cpp`; M11), before anything else: the libraries the
game's binaries need (`payload/requires.txt`, the package's REQUIRES: glibc, and GLIBCXX unless the package carries a
new enough libstdc++ of its own) against the running system; and the graphics driver: libvulkan.so.1 opened at run
time (nothing linked), every device asked for exactly what the game's renderer requires (Vulkan 1.0;
independentBlend, fragmentStoresAndAtomics, vertexPipelineStoresAndAtomics, geometryShader, fillModeNonSolid; a
graphics + compute queue; VK_KHR_swapchain; an X11 or Wayland surface), in a child process so a crashing driver
cannot take the setup down. Software renderers (llvmpipe) do not count. No GPU names or driver versions are
checked. When something is missing the welcome page says what (in plain words, the missing feature named) and
offers no installation. The welcome page names the system (SteamOS and the Steam Deck model by its board) and the
graphics card. On SteamOS "Add to Steam" starts ticked when Steam has a profile (Game Mode starts only what is in
the library). On a small screen (the Deck's 1280 x 800) the window is maximized; pages scroll when needed.
`kuf-setup --check-system` prints the same check (exit 0: compatible).

**Uninstalling:** from the menu entry "Uninstall Kingdom Under Fire: Circle of Doom", by running the setup again, or
`share/setup/kuf-setup --uninstall <folder>`. Only folders with the setup's record; only the entries the setup creates
(the folder itself only when then empty); the Steam entry needs Steam closed; saves and settings only with a second,
explicit confirmation. The original game files are never touched.

## Testing

- `build/installer/kuf_setup_tests` (unit tests, 20 cases; run by the build): quoting, desktop entries validated,
  `user-dirs.dirs`, the game-file list and checks, verified copies, the record, system requirements; Steam on
  synthetic folders (round trip, other games kept, update not duplicate, removal and renumbering, Steam running,
  unknown formats left untouched, profiles); installing, repairing, uninstalling in disposable folders (failures leave
  nothing, cancelling, paths with spaces, quotes and `$`). `HOME` / `XDG_*` point into temporary folders: the real
  desktop, menu, Steam and saves are never touched.
- `build/installer/kuf_setup_e2e PAYLOAD ASSETS GAME ROOT` installs for real into a disposable folder (about 4 GB)
  and `--uninstall ROOT` removes it; run with `HOME` / `XDG_*` in a temporary folder.
- `kuf-setup --walkthrough DIR` goes through the window's real flow by pressing its own buttons, including the installation,
  without a person at the screen (`GDK_BACKEND=broadway`; `KUF_SETUP_SHOT_GAME`, `KUF_SETUP_WALK_ROOT`, optional
  `KUF_SETUP_WALK_REFUSED` and `KUF_SETUP_WALK_CANCEL`; `HOME` / `XDG_*` in a temporary folder): pictures and a log in DIR.
- `kuf-setup --screenshot DIR` draws each page into PNGs (`GDK_BACKEND=broadway` with `broadwayd :7`: no screen).
  With `KUF_SETUP_SHOT_SIZE=1280x709 KUF_SETUP_SHOT_SCREEN=1280x756` (the Steam Deck's screen less the desktop's
  panel and the header bar) it also writes `layout.txt`: per page whether the content fits the width and whether it
  scrolls, and whether the visible window fits the screen (M11: all pages fit, none scrolls).

## Limitations

- Releases are portable builds (docs/STEAM_DECK.md "Portable build"): the game needs glibc 2.31 or newer (it
  carries its own libstdc++), the setup glibc 2.31 and GTK 3 (its libstdc++ linked in). The setup refuses cleanly
  where the game cannot run.
- Only the original game's base version is accepted (default.xex v1, unencrypted: docs/LOCAL_BUILD.md "Decryption keys");
  the game files are copied (about 4 GB).
- Menu artwork from the player's own game files is not used: the setup has its own artwork only.
