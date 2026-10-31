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
cached in `build/tools/`: appimagetool 1.9.1 (github.com/AppImage/appimagetool) and the X360 font by Redge
(dafont.com/x360.font, labelled "100% Free" there; the archive has no licence file, its source is noted next to the
font in the assets). Output in `build/installer/`; nothing is published.

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
GPU drivers and Vulkan do for the game. The AppImage contains the setup, its assets, and the game package (unpacked).

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

**System requirements:** the build records the newest glibc and GLIBCXX versions the game's binaries need
(`payload/requires.txt`); the setup compares them with the running system and does not offer the installation when
they are too old (the game would not start).

**Uninstalling:** from the menu entry "Uninstall Kingdom Under Fire: Circle of Doom", by running the setup again, or
`share/setup/kuf-setup --uninstall <folder>`. Only folders with the setup's record; only the entries the setup creates
(the folder itself only when then empty); the Steam entry needs Steam closed; saves and settings only with a second,
explicit confirmation. The original game files are never touched.

## Testing

- `build/installer/kuf_setup_tests` (unit tests, 18 cases; run by the build): quoting, desktop entries validated,
  `user-dirs.dirs`, the game-file list and checks, verified copies, the record, system requirements; Steam on
  synthetic folders (round trip, other games kept, update not duplicate, removal and renumbering, Steam running,
  unknown formats left untouched, profiles); installing, repairing, uninstalling in disposable folders (failures leave
  nothing, cancelling, paths with spaces, quotes and `$`). `HOME` / `XDG_*` point into temporary folders: the real
  desktop, menu, Steam and saves are never touched.
- `build/installer/kuf_setup_e2e PAYLOAD ASSETS GAME ROOT` installs for real into a disposable folder (about 4 GB)
  and `--uninstall ROOT` removes it; run with `HOME` / `XDG_*` in a temporary folder.
- `kuf-setup --screenshot DIR` draws each page into PNGs (`GDK_BACKEND=broadway` with `broadwayd :7`: no screen).

## Limitations

- The game binaries need glibc 2.44 / GLIBCXX 3.4.36 (built on Arch Linux): today only rolling distributions run
  them; Debian, Ubuntu, Fedora and SteamOS need a build made in an older build environment (docs/STEAM_DECK.md). The
  setup itself needs glibc 2.38 and GTK 3; it refuses cleanly where the game cannot run.
- Only the original game's base version is accepted (default.xex v1); the game files are copied (about 4 GB).
- Menu artwork from the player's own game files is not used: the setup has its own artwork only.
