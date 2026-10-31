# Steam Deck and other Linux PCs: packages, installation, updates

Status (2026-10-30): the package, installer, launcher and updater are built and tested on this development PC with
synthetic packages (`tools.updater_security`, 18 cases). **Nothing here has been run on a Steam Deck yet**, and no
release has been published (see "Blockers").

## 1. What you need
- Your own copy of the original Xbox 360 game, extracted to a folder: the folder that contains `default.xex` (the
  same folder this port uses on the PC). The port only reads it; it never writes there.
- A package of the port (section 2). It contains no game data, but it does contain code recompiled from the game's
  executable: keep it for your own use with your own copy.

## 2. Make a package (on the development PC)
```sh
ninja -C build/linux-amd64-relwithdebinfo -j8         # the build (AGENTS.md)
tools/package/package_linux.sh                        # -> build/packages/kuf-cod-recomp-<version>-linux-x86_64.tar.gz
tools/package/package_linux.sh --strip                # smaller; crash reports then name fewer host functions
```
The package holds `bin/` (the game and its three libraries, found through `$ORIGIN`), `kuf-cod.sh` (launcher),
`install.sh`, `kuf_update.py`, `VERSION`, `MANIFEST.sha256` (every file's SHA-256) and licenses. About 76 MB.

**glibc:** packages built on this PC need glibc 2.44 (and libstdc++ GLIBCXX 3.4.36). SteamOS ships an older
glibc, so the package runs on a Steam Deck only inside a container with a current userland (section 3, option A),
until a build environment with an older glibc is approved (option B).

## 3. Install on a Steam Deck
Copy the package to the Deck (USB stick, `scp`, a network share), then in Desktop Mode open Konsole.

### Option A: in an Arch Linux distrobox (works with today's packages; untested on a Deck)
SteamOS 3.5 and later include distrobox and podman.
```sh
distrobox create --name kuf --image docker.io/library/archlinux:latest
distrobox enter kuf
sudo pacman -Syu --needed vulkan-radeon vulkan-icd-loader libx11 libxcb wayland libpulse alsa-lib zenity
tar -xzf ~/Downloads/kuf-cod-recomp-*-linux-x86_64.tar.gz -C ~/Downloads
~/Downloads/kuf-cod-recomp-*-linux-x86_64/install.sh
~/.local/share/kuf-cod-recomp/app/kuf-cod --game-dir /path/to/your/game   # first start; the folder is remembered
exit
```
The container shares your home folder, so the install, saves and settings are in your normal home
(`~/.local/share/kuf-cod-recomp/`), and the game folder can stay on the SD card.

**Game Mode:** Steam > Add a Non-Steam Game > Browse, any program (e.g. Konsole), then edit the shortcut:
- Target: `/usr/bin/distrobox-enter`
- Launch options: `-n kuf -- /home/deck/.local/share/kuf-cod-recomp/app/kuf-cod`
- Controller: Steam Input's "Gamepad" template (Xbox layout); the port reads it as an Xbox controller.

### Option B: native SteamOS package (blocked: approval)
A package that runs directly on SteamOS needs a build against an older glibc: inside Valve's Steam Runtime
"sniper" SDK image (Docker is installed on the development PC) or a Flatpak SDK. Both download an environment
from outside the repository, which needs the player's approval (AGENTS.md rule 8). Then the launcher would be the
shortcut's target directly.

### Settings for the Deck
- Display: the port fills 16:10 (1280 x 800) correctly (M6C, Hor+), menus and HUD in the centred 16:9 area.
- Frame rate (PC Settings > Frame Rate): the LCD Deck's screen runs at 60 Hz, the OLED's at up to 90 Hz. Start
  with 60; whether the Deck holds 60 is not measured yet. 30 (Original) is the original game.

## 4. Updates
```sh
~/.local/share/kuf-cod-recomp/app/kuf-update check      # is there a newer release?
~/.local/share/kuf-cod-recomp/app/kuf-update update     # download, verify, install, switch (asks first)
~/.local/share/kuf-cod-recomp/app/kuf-update rollback   # back to the version before
~/.local/share/kuf-cod-recomp/app/kuf-update status
```
In the game: the title list's CHECK FOR UPDATES row runs the same check and shows the result in a dialog (installed
and available version, release date and notes); Install downloads with progress (cancellable), verifies and installs,
then offers Restart (docs/SETTINGS.md "Title menu rows"). With `--json` the updater prints one result line, and for
`update` progress lines before it (`{"progress": bytes, "total": bytes}`, `{"stage": "verifying"}`); SIGTERM / SIGINT
during the download cancel it (exit 5, nothing changed; leftovers of a killed run are removed at the next run).
Restart: the launcher sets `KUF_RESTART_FILE` (or `--kuf_restart_file`); the game writes it and quits; the launcher
then starts the install's stable `kuf-cod` again, which runs the version now current.

Trying the whole flow without GitHub (done 2026-10-31): build two versions (`KUFCOD_RELEASE_VERSION=0.7.1` /
`0.7.2`, `package_linux.sh`), install the older into a disposable root with a throwaway key's `allowed_signers`, put
the newer and its signature with a `releases.json` in a local folder, and start the game in the sandbox with
`--copy-dir <that folder>` and `--kuf_updater=` a wrapper running `kuf_update.py --source-dir ... --source-rate 3072`
(slow enough to see progress and cancel) plus `--kuf_restart_file=...`.

Updates are off until the install knows where releases come from and which key signs them:
`install.sh --repo owner/name --signers allowed_signers` (or edit `app/update.conf` and copy `allowed_signers`).

How an update is checked (tools/updater/kuf_update.py):
- releases come only from the repository in `update.conf`, over HTTPS with certificate checks; redirects only to
  GitHub's download hosts;
- the package is verified before it is opened: an `ssh-keygen -Y verify` signature (ed25519, namespace
  `kuf-cod-recomp-release`) by a key in `allowed_signers`; then its `VERSION` must equal the release's and every file
  must match `MANIFEST.sha256`;
- never a downgrade or a reinstall of the running version unless asked (`--allow-downgrade`); an old signed package
  re-published under a newer number is refused by its `VERSION`;
- unpacking refuses absolute paths, `..`, links, devices, set-uid files and anything outside the package's one
  directory, with size limits; it unpacks next to the installed versions and switches with an atomic rename;
- the version in use, the one before and one more are kept; `rollback` switches back;
- saves, settings and logs (`~/.local/share/kuf-cod-recomp/data`) are never touched.

Making a release (when the player decides to publish, see "Blockers"):
```sh
tools/package/sign_package.sh --new-key ~/kuf-release-key            # once; keep the private key safe and offline
KUFCOD_RELEASE_VERSION=0.7.0 ninja -C build/linux-amd64-relwithdebinfo   # the version shown and checked
tools/package/package_linux.sh --version 0.7.0
tools/package/sign_package.sh --key ~/kuf-release-key/kuf-cod-release build/packages/kuf-cod-recomp-0.7.0-linux-x86_64.tar.gz
# release v0.7.0 with both files: kuf-cod-recomp-0.7.0-linux-x86_64.tar.gz and .tar.gz.sig
```

## 5. Logs and bug reports
- Logs: `~/.local/share/kuf-cod-recomp/data/logs/kuf_cod.log` (64 MB, 3 files kept); a crash writes
  `data/user/crash-report.txt`; each session's statistics: `data/user/run-summary.json`.
- `kuf-cod --collect-logs` packs those (no saves, no game files) into `~/kuf-cod-logs-<date>.tar.gz`.

## 6. Blockers
- **Publishing:** a package contains code recompiled from the game's executable. Whether releases may be published
  (and where) is the player's decision; the project has no licence file of its own yet. Until then: personal use.
- **Release repository and signing key:** the player's choice (`update.conf`, `sign_package.sh --new-key`).
- **Native SteamOS build:** an older-glibc build environment (section 3, option B) needs approval.
- **Steam Deck hardware test:** not done (no Deck available to this session).
