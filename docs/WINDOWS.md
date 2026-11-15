# Windows (M16)

**Status (2026-11-10, M17): provisionally supported, not fully verified.** Played on the player's Windows 11 PC since
M16 (title, settings, gameplay, saving). The player has no Windows PC for now (decision of 2026-11-10): native
verification of the M17 work is postponed, development continues on Linux, and the Windows build, packages, setup,
updater and co-op are kept building (cross-build checks after shared-code changes). The checks below marked open
are **unverified, not passed**; the acceptance kit (docs/WINDOWS_TEST.md, tools/windows/make_acceptance_kit.sh)
runs them when a Windows PC is available again.
M17 work (each item says how it was verified; "Wine" means the Windows program ran under Wine on the Linux machine,
which tests the program's own code, not Windows):

| Area | State |
|---|---|
| Native build | One command, `tools/windows/build.sh` (docs/WINDOWS_BUILD.md); fresh-clone cross build with Git for Windows' settings in a Unicode path: passes. **Not yet run on Windows.** |
| Unicode paths | Fixed: every Windows program runs with the UTF-8 code page (manifest); a folder `Папка Ü` was garbled before (shown with an llvm-mingw program under Wine). **Native: open (WINDOWS_TEST A).** |
| No audio device | Fixed on both systems (SDK patch 0021): the game runs silently; Linux smoke test `smoke.no_audio_device_60s`. |
| Game priority | Fixed: the launcher started the game below normal priority on Windows. **Native: open** (Wine ignores the flag). |
| Co-op | Built for Windows (docs/MULTIPLAYER.md): network tests under Wine 440/440; Linux and Windows lobby builds in every combination (Wine). **Two real PCs and in-game: open.** |
| Updates | `kuf-update.exe` (docs/UPDATES_WINDOWS.md): unit tests and the real program under Wine over WinHTTP. **No Windows release published: not live.** |
| Saves | The same files on both systems; a manifest with Windows line endings now verifies. **A save moved between the systems: open.** |
| Display, Alt+Tab, controllers, setup shortcuts | Through SDL3 and the M16 setup; **only the player's Windows test can confirm** (docs/WINDOWS_TEST.md B-D). |
| Setup: administrator prompt, Settings > Apps | Fixed in code (asInvoker manifest; a per-user uninstall entry; unattended uninstall under Wine). **Native: open.** |
| Acceptance kit | Written (tools/windows/acceptance/); **never run on Windows**, so its first run may need harness fixes. |

## Decisions

- **Compiler: llvm-mingw** (the player's choice, 2026-11-08): release 20261006, LLVM 23.1.3 (the same version as the
  Linux local compiler), MinGW-w64 with the Universal C Runtime (part of Windows 10 and 11). Pinned and checked by
  `tools/toolchain/setup_llvm_mingw.sh`; `cmake/llvm-mingw-x86_64.cmake`; preset `mingw-amd64-relwithdebinfo`
  (cross-built here). Not clang-cl + MSVC: Microsoft's C runtime and Windows SDK cannot be given to players, and the
  game is compiled on the player's PC (docs/LOCAL_BUILD.md), so the Windows packages must carry a compiler that can
  be redistributed. llvm-mingw can (Apache-2.0 with LLVM exception; MinGW-w64 headers and runtime under their
  permissive licences; listed in the notices when bundled).
- **Renderer: Vulkan** (the SDK's Vulkan backend, as on Linux; D3D12 off). The Vulkan loader is the GPU driver's
  `vulkan-1.dll`.
- **One engine:** the same SDK, patches and project code; platform differences in small `#if defined(_WIN32)`
  branches or one platform file per concern. SDK changes are patches (`patches/rexglue-sdk/0020-windows-llvm-mingw.patch`,
  written to be acceptable upstream: they keep clang-cl working).
- **Co-op on Windows** (M17F): the same code as on Linux; the sockets through `src/net/sock` (Winsock), the runtime's
  original imports found by walking the loaded modules (Linux: `dlsym(RTLD_NEXT)`).

## Stage 1 audit (2026-11-08)

The SDK (Xenia's lineage) already supports Windows: threads, memory mapping, fibres, sockets, file system, SDL3
input and audio, Vulkan. What stood in the way of llvm-mingw, from a full cross-build (`-k 0`):

| Where | Problem | Fix |
|---|---|---|
| FFmpeg (SDK) | Windows configuration assumes MSVC's C runtime (`crtversion.h`) | patch 0020: overlay `config.h` with `HAVE_LIBC_MSVCRT 0` |
| `rex/chrono` (SDK) | `clock_time_conversion` missing in libc++ (only Apple's was handled) | patch 0020 |
| SDK Windows code | headers named `Windows.h`, `WinSock2.h`, `ObjBase.h` ... (MinGW installs lower case) | patch 0020: lower case |
| content transactions (patch 0005, ours) | `renameat2`, `fsync`, `open` | `MoveFileExW` write-through, `FlushFileBuffers`; Windows uses the existing "previous aside" path, which `Recover()` already handles |
| host path device (patch 0005) | `path::native()` is wide on Windows | `string()` |

Project code (`src/`, read-only audit):

| Area | State | Windows work |
|---|---|---|
| Vulkan, audio, keyboard, mouse, controllers | SDK and SDL3 | none; `setenv` of `REX_*` options in `gpu_defaults.cpp`, `display_window.cpp`, `frame_pacing.cpp` |
| Memory, threads | `std::thread`; profiler `perf_event` is Linux-only and stays off | `frame_pacing.cpp`: `/proc/self/maps` and `process_vm_readv` (diagnostics) → `VirtualQuery`, `ReadProcessMemory` |
| Files | `pc_settings.cpp` atomic write with `open`/`fsync`/`O_DIRECTORY`; `title_menu.cpp` narrow `fopen` | `CreateFileW`, `FlushFileBuffers`, `MoveFileExW`; wide paths |
| Networking | `kuf_net` (`session`, `crypto`, `coop_sock`, `lobby_service`) is POSIX only | Winsock2, `WSAPoll`, `BCryptGenRandom`, `GetAdaptersAddresses` |
| Steam | `dlopen("libsteam_api.so")` | `LoadLibraryW("steam_api64.dll")` |
| Crash reports | `_WIN32` branch exists; no host backtrace | `RtlCaptureStackBackTrace` later |
| Title menu updater | `posix_spawn`, `kill`, `xdg-open` | `CreateProcessW`, `TerminateProcess`, `ShellExecuteW` |
| Paths | user data defaults to `Documents\kuf_cod` on Windows (SDK); logs `<exe>/logs`; config `<exe>/kuf_cod.toml` | the launcher passes `--user_data_root`, `--content_root`, `--cache_root`, `--log_file` (as on Linux) |

Outside the game: the launcher (`kuf-cod.sh`), the updater (`kuf_update.py`: `fcntl`, a `current` symlink,
`ssh-keygen`), the local build (`installer/src/core/localbuild.cpp`: `fork`/`execve`, process groups, symlinked
tools; `tools/package/make_builder.py`: an ELF/glibc recipe) and the setup core (`installer/src/core`: glib for
SHA-256, `statvfs`, `mkstemp`, XDG paths, `.desktop` files, `/proc` scans) are Linux-only. The GTK window is Linux-only.

## Stage 2 progress (2026-11-08)

- **Builds:** `cmake --preset mingw-amd64-relwithdebinfo -DKUFCOD_BUILD_TESTS=OFF` then ninja (the Linux
  `build/linux-amd64-relwithdebinfo/bin/rexgluerd` generates the code: `REXGLUE_HOST_REXGLUE`). Output:
  `kuf_cod.exe`, `librexruntimerd.dll`, `rexgpu-xenosrd.dll` (no "lib": the name the runtime loads), `libTracyClientrd.dll`. They import only Windows
  system DLLs (the Universal C Runtime `api-ms-win-crt-*`, kernel32, user32, ...) and llvm-mingw's `libc++.dll` and
  `libunwind.dll`, which ship beside them. Vulkan is loaded at run time (the driver's `vulkan-1.dll`).
- **Fixed for MinGW** (patch 0020 unless noted): the items in the table above; guest imports (`__imp__Name`) exported
  explicitly (the MinGW linker's automatic export skips `__imp_*` names; the project links the runtime with
  `--export-all-symbols`); `wWinMain` (`-municode`); content-transaction test kills (`TerminateProcess`, patch 0005);
  the code generator of the build machine in cross builds (`REXGLUE_HOST_REXGLUE`; this machine has Wine registered
  for `.exe` files, which once ran the Windows code generator under Wine before this was fixed: no files changed).
- **Project code:** settings written with `CreateFileW`/`FlushFileBuffers`; `SetEnv` (`_putenv_s`); frame-pacing
  diagnostics with `VirtualQuery`/`ReadProcessMemory`; the title menu runs the updater with `CreateProcessW` and opens
  pages with `ShellExecuteW`; the lobby network layer is left out on Windows (co-op is off there).
- **Linux unchanged:** the Linux build passes, unit tests 1,788 of 1,788; the generated code is byte-identical.
- **Not yet run on Windows.** `tools/windows/make_test_bundle.sh` makes a private test bundle (it contains the
  translated program: the player's own test only, never published) with `docs/WINDOWS_TEST.md` as its checklist.

## The Windows local build (Stage 2b, 2026-11-08)

As on Linux, a Windows package carries no game code: the player's PC generates and compiles it
(docs/LOCAL_BUILD.md). `tools/package/make_builder_windows.py` writes the package's `builder/` from the llvm-mingw
build tree: `recipe.txt` (the verified build's flags; `compiler` is llvm-mingw's `clang-23.exe`, called with the
options its wrapper adds and no configuration files), and only what the build read: 1,211 headers, 32 link inputs
(MinGW-w64 CRT objects and import libraries, compiler-rt builtins, libc++, libunwind), the compiler and linker with
their DLLs (`clang-23.exe`, `ld.lld.exe`, `libLLVM-23.dll`, `libclang-cpp.dll`, `libc++.dll`, `libunwind.dll`), the
project's own code (`libkuf_own.a`, checked for translated functions) and the SDK's libraries. 168 MB unpacked. Every
llvm-mingw file is taken from the Windows build of the release after a byte comparison with the Linux build the
cross build used (1,317 target files: all identical).

`installer/src/core/localbuild.cpp` runs it on both systems (core/platform: CreateProcessW in a kill-on-close Job
Object on Windows).

**Checked on Linux, not yet on Windows:** `tools/windows/replay_recipe.sh` runs the real `kuf-localbuild` with this
builder, the Linux-hosted clang of the same release standing in for `clang-23.exe` (and `<root>/include` linked where
a Windows-hosted clang finds it). It generated, compiled (162 files, 4 jobs, 2 minutes) and linked `kuf_cod.exe`;
`tools/package/function_hashes.py` compared every function of its objects with the verified cross build's (layout
removed: addresses, displacements, labels of relocated targets): **28,923 of 28,923 identical**, the 28,671 translated
functions included. `clang-23.exe` itself runs for the first time on the player's Windows PC.

## Plan

1. **Stage 2, the native game:** the SDK and the project build with llvm-mingw (`kuf_cod.exe`, the runtime DLLs);
   the project's POSIX code gets Windows branches; tests that fork or use bubblewrap stay Linux-only. Then a Windows
   local-build recipe: the runtime and the project's own code prebuilt, the generated code compiled on the player's
   PC with a trimmed llvm-mingw (clang, lld, libc++ and MinGW-w64 headers and import libraries only), linked into
   `kuf_cod.exe`. A self-test the player runs on Windows (start, Vulkan device, audio, a controller, the title menu,
   a new game) with logs to send back.
2. **Stage 3, drag and drop:** the setup core made platform-neutral (no glib; a small platform layer for processes,
   free space, file sync, known folders, shortcuts), shared by every front end. Drag and drop in the Linux setup and
   in the Windows setup, with the same steps (detect, validate, show what was found and where it goes, confirm,
   install). Nothing is changed by dropping alone.
3. **Stage 4, portable:** one launcher program for both systems (`Play KUF.exe`, `Play KUF`), paths relative to its
   own location; first launch: find the game files in `GameFiles/`, check them with the M15.2 code, build the native
   game into `Runtime/`, start it with saves, settings and logs inside the folder; later launches skip the build
   unless the game files' identity, the package's `generation_id` or the cache changed.
4. **Stage 5, packages:** Linux AppImage and `.tar.gz`; Windows setup `.exe` and `.zip`; the same version and
   generated-code identity; the audit extended to PE files (no game code, no keys, no generated code).
5. **Stage 6, regression:** Linux unit tests, the clean-system test, the menu scenario; Windows checks the player runs.

Encrypted retail executables: every package decrypts the player's own retail `default.xex` (docs/RETAIL_FILES.md,
decision of 2026-11-08 (2)); the key is only in `Play KUF.exe` (and so the setup .exe), never in `Runtime/`.

## Packages (2026-11-08)

| Platform | Installer | Portable |
|---|---|---|
| Linux | AppImage (GTK setup, drag and drop) | `.tar.gz` (`tools/package/make_linux_portable.sh`) |
| Windows | `KUF-Circle-of-Doom-Setup-<v>-windows-x86_64.exe` (`make_windows_setup.sh`) | `.zip` (`make_windows_portable.sh`) |

All four carry no game code: the game is generated and compiled on the player's PC. The portable editions and the
Windows setup share one launcher program and core (docs/PORTABLE.md); the Linux AppImage keeps its GTK setup on the
same core. Audits: `audit_release.py` (Linux release), `audit_portable.py` (portable and Windows packages).

## Saves across systems

The saves are the game's own content packages (its bytes, big-endian as on the console) and the port's checksum
manifest, written the same way by the SDK on both systems (ASCII names valid on Windows, distinct without case; an LF
manifest written byte for byte; since M17 a manifest with Windows line endings verifies too); settings are a TOML text
file. Copying `Data/saves` and `Data/user` between a Linux and a Windows copy is expected to work; **a save made on
one system and loaded on the other has not been tried yet** (WINDOWS_TEST, and a Wine run once Xvfb is available).

## History: M2 probe (2026-10-20)

A probe with the system's MinGW-w64 GCC headers stopped on the same FFmpeg, header-case and conversion errors; at the
time the conclusion was clang-cl + MSVC. M16 chose llvm-mingw instead (above).
