# Steam Deck and other Linux PCs: the portable build, installation, updates

Status (2026-11-01, M11): releases are built for **glibc 2.31 and newer** (SteamOS 3.x, Debian 11+, Ubuntu 20.04+,
any current desktop distribution) with their own C++ library, and the setup checks the graphics driver for what the
game's renderer needs. Tested on this development PC (Arch, RADV) and in an old Linux runtime (Steam Runtime 3:
glibc 2.31, libstdc++ 10). **Not yet run on a physical Steam Deck** (section 7).

## 1. Why v0.7.0 did not install on a Steam Deck (M11 findings)
- **glibc.** v0.7.0 was built on this Arch PC (glibc 2.44, GCC 16 libstdc++). Its binaries imported symbol versions
  that exist only there: `librexruntimerd.so` needed GLIBC_2.44 (`cosh`, `sinh`: glibc 2.43/2.44 re-versioned the
  math functions) and GLIBCXX_3.4.36 (`std::format` internals of GCC 16), `kuf_cod` GLIBC_2.43 (`sqrtf`, `remainder`)
  and GLIBC_2.38 (`__isoc23_strtol`: C23 headers); the setup itself only needed glibc 2.38. The setup read these
  needs from the binaries (`requires.txt`) and correctly refused SteamOS, whose glibc is older. Nothing in
  the game needs those functions' new versions: it was purely the build machine.
- **Vulkan.** v0.7.0's setup had **no** Vulkan check: nothing in it (or in the game's launcher) reads the graphics
  driver, so the Vulkan message seen on the Deck did not come from the setup's checks (to be identified from a
  screenshot or log). The game's real requirements (ReXGlue SDK, `src/ui/vulkan/vulkan_device.cpp`): Vulkan 1.0; the
  features `independentBlend`, `fragmentStoresAndAtomics`, `vertexPipelineStoresAndAtomics`, `geometryShader`,
  `fillModeNonSolid`; a graphics + compute queue; `VK_KHR_swapchain`; `VK_KHR_surface` with the X11 (xcb) or Wayland
  surface. RADV (SteamOS's driver for the Deck's RDNA 2 GPU) has all of them. No GPU names or driver versions are
  checked anywhere.
- **One real gap, fixed:** the M9 direct texture loads (SDK patch 0013) created textures with storage usage without
  asking the device whether their formats support storage images. Now the 14 formats are checked at start
  (`vkGetPhysicalDeviceFormatProperties`, optimal tiling) and any that is not supported takes the staging-buffer path
  (logged). On this PC (RADV, RDNA 4) all 14 are supported, so the desktop keeps every fast path; RADV reports the
  same for RDNA 2.

## 2. Portable build
```sh
tools/toolchain/setup_sniper.sh                  # once: build/toolchains (about 1.3 GB download, 15-30 min)
cmake --preset linux-amd64-portable && cmake --build build/linux-amd64-portable
tools/package/package_linux.sh --strip           # refuses binaries that exceed the baseline
tools/toolchain/old_runtime_test.sh build/packages/kuf-cod-recomp-*.tar.gz   # the package on glibc 2.31
```
- **Sysroot:** Valve's Steam Runtime 3 "sniper" SDK (Debian 11, glibc 2.31), pinned version and SHA-256
  (`cmake/steamrt3-sniper-version.cmake`), unpacked under `build/toolchains`. This machine's Clang compiles against it
  (`cmake/steamrt3-sniper.cmake`): headers, libraries and pkg-config files come only from the sysroot; its absolute
  symbolic links are made relative so nothing resolves to this machine's files. SDL's Wayland code is generated with
  the sysroot's own wayland-scanner (libwayland 1.18). Same compiler flags as the desktop build (`-O2 -g`,
  `-march=x86-64-v2`; the Deck's Zen 2 has x86-64-v3).
- **C++ library:** the SDK's GCC 14 ships only a static libstdc++, but the game's GPU code is a plugin sharing C++
  objects with the runtime, so it needs one shared copy. `setup_sniper.sh` builds GNU GCC 14.2.0's libstdc++ (SHA-256
  pinned source, the configuration of the SDK's own GCC 14) inside the sysroot. The package carries it as
  `bin/compat/libstdc++.so.6` (GLIBCXX_3.4.33, needs glibc 2.26). The launcher uses it only when the system's
  libstdc++ is older than the game needs (`REQUIRES`); otherwise the system's (it then also serves the graphics
  driver, which may need the newer one). Not in `$ORIGIN`, where it would always win.
- **Result:** the game needs glibc 2.30 and GLIBCXX 3.4.32 (GCC 13); the setup glibc 2.25, with its C++ library
  linked in. glibc itself is never bundled.
- **Checks:** `package_linux.sh` writes `REQUIRES` (glibc, glibcxx, glibcxx-bundled) from the binaries and refuses
  anything above the baseline; the release audit refuses the same from the ELF version tables, and a REQUIRES or
  manifest `requires` that does not match (docs/RELEASING.md); `build_appimage.sh` refuses a setup above the baseline
  or linking libstdc++.

## 3. Install on a Steam Deck
**You need the game's files on the Deck, with an unencrypted `default.xex`.** Retail copies are encrypted and are
refused (docs/RETAIL_FILES.md). This has not been tried on a physical Deck yet (section 7).

1. Desktop Mode (Power > Switch to Desktop). Download the AppImage from the GitHub release page in a browser, or copy
   it over with a USB stick or a network share, into e.g. `~/Downloads`.
2. Dolphin: right-click the AppImage > Properties > Permissions > "Is executable" or "Allow executing" (or Konsole:
   `chmod +x ~/Downloads/KingdomUnderFire-CircleOfDoom-Setup-*.AppImage`). Double-click it.
3. The setup checks the system first (libraries and the graphics driver; the welcome page names the system and the
   GPU). Choose your game folder (with `default.xex`; the SD card is fine). Keep the installation folder
   `~/Games/Kingdom Under Fire - Circle of Doom` or choose another; 5.5 GB must be free.
4. Options: on SteamOS, "Add to Steam" is ticked when Steam has a profile: Game Mode starts only what is in the Steam
   library. Steam must be closed while the setup adds the shortcut (the setup asks: Steam > Exit, then Retry).
5. Install. The setup generates and compiles the game on the Deck, without a terminal or a compiler of yours. How long
   this takes on a Deck has not been measured: plug it in. Cancel is safe, and an interrupted attempt continues.
6. Start Steam again, then return to Gaming Mode. The game is in the library under non-Steam games. Controller: Steam
   Input's default gamepad layout (the port reads it as an Xbox controller).

If the shortcut is missing: in Desktop Mode, Steam > Games > Add a Non-Steam Game to My Library > Browse, and choose
`kuf-cod` in the installation folder.

Without a window: `./KingdomUnderFire-...AppImage --check-system` prints what the setup checks (exit 0: compatible).

### Settings on the Deck
On the first start on a Steam Deck (recognised by its board: Valve Jupiter / Galileo, not by its GPU) and only
when there is no `pc_settings.toml` yet: render scale 1x (the game's 1280x720 frame rendered once; "native" would
render 1280x1440 for the 800-line screen) and VSync on (even pacing on its 60 Hz panel). The frame rate stays the
original 30. Everything is changeable in PC Settings and never overwritten afterwards. The display fills 16:10
(1280 x 800) Hor+, menus and HUD in the centred 16:9 area (M6C).

## 4. Updates
In the installation folder (a terminal install: `~/.local/share/kuf-cod-recomp/app`):
```sh
./kuf-update check      # is there a newer release?
./kuf-update update     # download, verify, make the program, switch (asks first)
./kuf-update rollback   # back to the version before
./kuf-update status
```
A pre-release is only offered with `channel = prerelease` in `update.conf` there.
In the game: the title list's CHECK FOR UPDATES row (docs/SETTINGS.md "Title menu rows"): the installed and the new
version, Install Update / Open GitHub Page / Cancel; download with progress (cancellable), verification, Restart.
Updates are signed (ed25519, `ssh-keygen -Y verify`; the production key is pinned in releases) and checked in detail
(docs/RELEASING.md section 2), including whether this system can run the new version (M11: the signed manifest's
`requires`, and the unpacked package's REQUIRES before the switch). A new version that fails its first start is
switched back automatically. Saves, settings and logs (`~/.local/share/kuf-cod-recomp/data`) are never touched.

## 5. Logs and bug reports
- `~/.local/share/kuf-cod-recomp/data/logs/kuf_cod.log` (the game: `[kuf-platform]` names the system, Steam Deck
  model and gamescope; the Vulkan device and driver follow), `logs/launcher.log` (each start: version, system,
  board, glibc, which libstdc++, display, `LD_LIBRARY_PATH`, exit status); a crash writes `data/user/crash-report.txt`.
- `kuf-cod --collect-logs`, in the installation folder, packs those (no saves, no game files) into
  `~/kuf-cod-logs-<date>.tar.gz`. For example:
  `~/"Games/Kingdom Under Fire - Circle of Doom"/kuf-cod --collect-logs`
- The setup prints its system check to stderr: run the AppImage from Konsole to see it, or `--check-system`.

## 6. What changed for desktop Linux
Nothing visible: the same game, settings and update flow. Releases now also run on older distributions; on this PC
the portable build passes the same tests as the desktop build (docs/TESTING.md).

## 7. Not verified yet (needs the physical Steam Deck)
The AppImage in Desktop Mode, the installation and how long its compile takes, the game's start in Desktop and Game Mode, rendering (RADV on
RDNA 2), the controller through Steam Input, audio, frame rate at 1280 x 800, the Steam shortcut, the updater on the
Deck. Steam's environment for non-Steam games (its runtime's `LD_LIBRARY_PATH`) is logged by the launcher.
