# KUF: Circle of Doom Recomp

A native PC port of **Kingdom Under Fire: Circle of Doom** (Xbox 360, 2007), built by statically recompiling the
game's PowerPC executable to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and replacing the
console's services with a native runtime. Linux x86-64 today; Windows is a goal, not yet built.

**You need your own copy of the game.** This repository contains **no game files and no code derived from them**:
the recompiled C++ is generated locally from your own `default.xex` and is never committed or published here.

## Status

**Playable on Linux.** Details and evidence: [docs/PROGRESS.md](docs/PROGRESS.md); open problems:
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

- **Menus** with keyboard and mouse (hover, click, wheel, drag) or a controller; PC button prompts for the device in
  use; the console's storage and Xbox LIVE prompts removed.
- **Gameplay, combat, saves and campaign progress** (the game's own saves, written transactionally to their own
  folder; docs/SAVES.md).
- **Modern camera** with free look and mouse look; the original camera remains available (docs/CAMERA.md).
- **Native resolutions, widescreen and ultrawide**, windowed / borderless / fullscreen (docs/DISPLAY.md).
- **High frame rates** (30 to 240 or unlimited) with interpolation between the game's 30 ticks per second
  (docs/HIGH_FPS.md).
- **Settings inside the game's own menus**, by category: Display, Camera, Audio, Mouse & Keyboard (rebindable keys),
  Controller (rebindable buttons), Gameplay (docs/SETTINGS.md).
- **Updater**: installed copies check this repository's GitHub releases for signed updates (below).

Not yet: Windows builds (docs/WINDOWS.md), the title update (base executable only), online co-op, translations of
the PC texts. An installer is planned.

## Building (Linux)

Requirements: Clang >= 20, CMake >= 3.25, Ninja, Python 3 with `numpy` and `cryptography`, LLVM tools
(`llvm-objcopy`, `llvm-objdump`), `bubblewrap`, GTK3 headers, a Vulkan loader and driver, git. About 1.5 GB of disk per
build tree plus about 550 MB for the SDK sources. The generated code needs a lot of memory to compile: cap the build
(the last line below) on machines with less than 32 GB of RAM.

On Arch Linux, for example:

```sh
sudo pacman -S --needed clang llvm lld cmake ninja python-numpy python-cryptography bubblewrap gtk3 vulkan-icd-loader git
```

Then:

```sh
git clone --recurse-submodules https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp.git
cd KUF-Circle-of-Doom-Recomp
echo "/path/to/your/game" > .game_dir                # the folder with default.xex (only ever read; or set KUF_GAME_DIR)
tools/stage_input.sh v1                              # copies and verifies default.xex into build/input
tools/apply_sdk_patches.sh                           # the project's fixes to the pinned SDK (patches/rexglue-sdk)
cmake --preset linux-amd64-relwithdebinfo            # warns: no recompiled sources yet
ninja -C build/linux-amd64-relwithdebinfo rexglue    # the codegen tool
tools/codegen_iterate.sh                             # recompiles your default.xex to C++ (generated/default)
cmake --preset linux-amd64-relwithdebinfo            # picks up the generated sources
systemd-run --user --scope -p MemoryMax=14G ninja -C build/linux-amd64-relwithdebinfo -j8
```

Only the base game executable is supported (`stage_input.sh` checks its SHA-256). The build's version and build time
come from git (cmake/kuf_version.cmake) and are shown in the title screen's SETTINGS.

## Installing and updating

A build can be packaged and installed for everyday play (the planned installer will wrap these steps):

```sh
tools/package/package_linux.sh --version 0.7.0       # build/packages/kuf-cod-recomp-0.7.0-linux-x86_64.tar.gz
tar -xzf build/packages/kuf-cod-recomp-0.7.0-linux-x86_64.tar.gz -C /tmp
/tmp/kuf-cod-recomp-0.7.0-linux-x86_64/install.sh    # into ~/.local/share/kuf-cod-recomp/app
~/.local/share/kuf-cod-recomp/app/kuf-cod --game-dir /path/to/your/game   # first start; later just kuf-cod
```

Saves, settings and logs live in `~/.local/share/kuf-cod-recomp/data`, never in the game folder or the install.
**Updates:** the title list's CHECK FOR UPDATES asks this repository's GitHub releases for a newer signed package,
shows its version, date and notes, downloads it with progress (cancellable), verifies its signature
(`ssh-keygen -Y verify` against the release key shipped with the install) and every file, installs it beside the
running version, and offers a restart; `kuf-update rollback` returns to the previous version. Nothing is downloaded
except over HTTPS from GitHub, and nothing from a release is executed before it is verified (docs/STEAM_DECK.md).
Release packages contain code recompiled from the game's executable; whether they are published is the project
owner's decision, so updates only work once releases exist.

## Running

The game is only ever started through the sandbox runner, which mounts the game folder read-only, keeps saves,
caches and logs in `build/runs/<run-id>/`, and has no network. A windowed session with sound and a controller, using
a dedicated save directory:

```sh
mkdir -p build/manual-saves
tools/run_sandboxed.sh -t 3600 -n play-1 --gpu --window --audio \
  --pad /dev/input/by-id/<your controller>-event-joystick \
  --save-dir build/manual-saves -- --kuf_exit_after_seconds=3590
```

`--window` binds only the Wayland socket and `--audio` only the PulseAudio/PipeWire socket; `--pad` is optional
(keyboard and mouse work without it). `--saves-from <dir>` starts from a copy of existing saves instead. All
options are described at the top of `tools/run_sandboxed.sh`; play sessions and their checks in docs/PROGRESS.md.

## Testing

```sh
ctest --test-dir build/linux-amd64-relwithdebinfo -j8                # everything that is registered
ctest --test-dir build/linux-amd64-relwithdebinfo -L display         # one group of game-run tests
python3 -I tools/smoke_test.py --seconds 300                         # needs the linux-amd64-audit tree
```

The unit tests (instructions, recompiled game functions, input, camera and display maths, settings, saves, pacing,
GPU) run without starting the game. The game-run tests (menus, gameplay, saves, camera, display, timing, render) run the game in the sandbox with scripted
input and check captures, logs and saves; they need the staged game and a GPU, and some need a local save fixture in
`build/m6-saves/` (they are not registered without it). docs/TESTING.md lists every suite.

## Documentation

| Document | Contents |
|---|---|
| [docs/PROGRESS.md](docs/PROGRESS.md) | Milestone reports, with what was tested |
| [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) | Open problems |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestones and acceptance criteria |
| [docs/TESTING.md](docs/TESTING.md) | Test suites, smoke test and diagnostics |
| [docs/FEASIBILITY.md](docs/FEASIBILITY.md) | M0 feasibility study and architecture decision |
| [docs/ENGINE_ANALYSIS.md](docs/ENGINE_ANALYSIS.md) | Executable, engine, middleware, asset formats |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Pipeline, project structure, version handling |
| [docs/RECOMPILATION.md](docs/RECOMPILATION.md) | Codegen results, coverage audit, overrides |
| [docs/IMPORT_AUDIT.md](docs/IMPORT_AUDIT.md) | Xbox system-function audit |
| [docs/INPUT.md](docs/INPUT.md) | Keyboard, mouse and controller input |
| [docs/SAVES.md](docs/SAVES.md) | Saves: format, transactions, damaged saves |
| [docs/CAMERA.md](docs/CAMERA.md) | The modern camera |
| [docs/DISPLAY.md](docs/DISPLAY.md) | Resolutions, display modes, widescreen and ultrawide |
| [docs/SETTINGS.md](docs/SETTINGS.md) | PC Settings in the game's menus |
| [docs/MENU_ARCHITECTURE.md](docs/MENU_ARCHITECTURE.md) | The game's menu system |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Profiling and performance work |
| [docs/HIGH_FPS.md](docs/HIGH_FPS.md) | M6D: timing, the fixed tick, frames between ticks |
| [docs/WINDOWS.md](docs/WINDOWS.md) | Windows compatibility investigation |
| [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) | Toolchain, SDK pin, licences |
| [docs/TECHNICAL_RISKS.md](docs/TECHNICAL_RISKS.md) | Risk register |
| [AGENTS.md](AGENTS.md) | Rules for coding agents |

## Legal

Not affiliated with Blueside, Phantagram or Microsoft. All trademarks belong to their owners. The project uses only
independent reverse engineering and permissively licensed open-source software.
