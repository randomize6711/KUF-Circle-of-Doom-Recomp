# Windows port: continuing development on Windows (handoff of 2026-11-08)

Until 2026-11-08 the Windows port (M16) was built on Linux (cross-compiled with llvm-mingw) and run by the player on
their Windows PC; every Windows bug needed a round trip. From here it is developed on that Windows PC directly. This
file is the starting point for an agent working there. Read `AGENTS.md` (project rules) and `docs/SESSION_STATE.md`
(the M16 section) first.

## The player's Windows PC

Windows 11 (build 26300), AMD Radeon RX 9070 XT (AMD driver 26.10.1, Vulkan 1.4), a 3360x1440 display (LC49G95T),
audio through SteelSeries Sonar. The player's portable test folder: `C:\Users\Seven Minutes\Desktop\circle of
doom\KUF-Circle-of-Doom-Portable\` (its `GameFiles\` holds the player's retail dump; `Data\game\default.xex` there is
the decrypted, supported executable the launcher made).

## Rules that matter most here (AGENTS.md has all of them)

- The player's game files are **read only**: never write, rename or delete anything in them. Never commit game data,
  generated code (`generated/`) or anything derived from the game.
- **Never push this repository, and never add a remote to it.** Its history (and the SDK's) still holds material that
  must not be published; publishing goes only through `tools/publish_public.sh`, only when the player asks.
- Do not merge, tag or publish without the player's approval. Commit completed steps separately.
- Test with disposable data folders; never touch the player's real saves or settings.
- The retail key stays out of the repository: the build reads it from the SDK's git history
  (`installer/CMakeLists.txt`); it may be only in `kuf-setup` / `Play KUF.exe` (docs/RETAIL_FILES.md).
- Do not claim Windows verification that was not actually run; do not claim Steam Deck tests.

## Where the port stands

Verified on this PC by the player (logs): the launcher, decrypting the retail `default.xex`, the local build
(generate, compile, link), Vulkan initialisation, audio, the game files opening. Four Windows-only bugs were found and
fixed on Linux (commits 61b469e CRLF object list, 93afea8 GPU plugin name, f75150c non-SEH setjmp, 978642e jmp_buf
alignment). **The last one (978642e) has never run on Windows**: it fixes the crash at the game's first setjmp
(guest `sub_8290CA18`; "access violation reading 0xFFFFFFFFFFFFFFFF" inside a Windows DLL). First task: build with it
and see how far the game gets.

Then: the checklist `docs/WINDOWS_TEST.md` (title screen, sound, keyboard/mouse, controller, new game, save/load,
settings, fullscreen, quit), the Windows setup (`Play KUF.exe --setup` flow), and the known gaps in `docs/WINDOWS.md`
(co-op, the updater, crash call stacks on Windows).

## Setting up the build on Windows

**Since M17A: `tools/windows/build.sh` does all of this (docs/WINDOWS_BUILD.md).** The manual steps below are the
history of the first native build.

Tools (none of them is needed by players; only for development):

- Git for Windows (gives Git Bash: the project's scripts in `tools/` are bash), CMake 3.28+, Ninja, Python 3.
- llvm-mingw 20261006 UCRT x86_64, the same release the packages carry: `llvm-mingw-20261006-ucrt-x86_64.zip` (beside
  this repository in the handoff folder, or from `tools/toolchain/setup_llvm_mingw.sh`'s pinned URL and SHA-256).
  Unzip it to `build\toolchains\llvm-mingw-20261006-ucrt-x86_64\` and set `KUFCOD_LLVM_MINGW` to that folder
  (`cmake/llvm-mingw-x86_64.cmake` reads it; on a Windows host it uses the `.exe` tools).

Steps (Git Bash, in the repository):

1. `git submodule update --init` if `thirdparty/rexglue-sdk` is empty (the handoff copy has it with its full history,
   which the build needs for the retail key). Then `tools/apply_sdk_patches.sh` (applies `patches/rexglue-sdk/*`; the
   SDK's working tree must carry them).
2. The game's executable for code generation: copy the **decrypted** `default.xex` (for example the portable folder's
   `Data\game\default.xex`) to `build\input\v1\default.xex` (path from `kuf_cod_manifest.toml`; untracked). Create
   `.game_dir` (untracked) with the path of a game folder for running tests.
3. `cmake --preset mingw-amd64-relwithdebinfo -DKUFCOD_BUILD_TESTS=OFF`, then
   `ninja -C build/mingw-amd64-relwithdebinfo kuf_cod`. On a Windows host the build runs its own `rexgluerd.exe` for
   code generation (the `REXGLUE_HOST_REXGLUE` cache entry is only for Linux cross builds and is ignored; see the
   condition in `thirdparty/rexglue-sdk/resources/templates/init/rexglue_cmake.inja`). Expect a long first build
   (about 170 large translation units).
4. The launcher and setup: `cmake -S installer -B build/installer-mingw -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
   -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x86_64.cmake` then `ninja -C build/installer-mingw kuf-launcher` (`Play
   KUF.exe`; decryption is on by default).

Done on the player's PC on 2026-11-08 (CMake 4.4.4, Ninja 1.13.2, Python 3.12 from winget). What it took beyond the
steps above (the first two by hand, still to automate):

- The SDK's nested submodules (FFmpeg, SDL3, Vulkan, ... 22 public repositories) were missing from the handoff copy:
  `git -C thirdparty/rexglue-sdk submodule update --init --depth 1`.
- Git for Windows checks symlinks out as text files holding the link's path: libmspack's `cabextract/mspack/*` (15)
  and o1heap's `CLAUDE.md`. Replace each with a copy of its target (`git ls-files -s` mode 120000 in each submodule;
  MoltenVK's are macOS-only). Symptom: `lzxd.c:1:1: error: expected identifier`.
- `rexgluerd.exe` needs llvm-mingw's `libc++.dll` and `libunwind.dll` (from `x86_64-w64-mingw32/bin/`) beside it in
  `build/mingw-amd64-relwithdebinfo/bin/`, or code generation fails with 0xC0000135.
- The patch check at configure time runs through bash on a Windows host (root CMakeLists.txt, commit 1877aae).
- The installer's `CMAKE_TOOLCHAIN_FILE` must be an absolute path (it is relative to `installer/` otherwise).
- No `python3` on Windows: the Microsoft Store alias is a stub; scripts use `python`.
- The portable package: relink once with `ninja -C build/mingw-amd64-relwithdebinfo -d keeprsp kuf_cod` (delete
  `bin/kuf_cod.exe` first) so the link's response file stays for `make_builder_windows.py`, then
  `tools/package/make_windows_portable.sh VERSION` with Python and Ninja on the PATH (commit e51bc20).

The tests are not built on Windows (`KUFCOD_BUILD_TESTS=OFF`; several need the Linux sandbox), so `safe_merge.sh` runs
on the Linux machine.

## Running the game for tests

`build\mingw-amd64-relwithdebinfo\bin\kuf_cod.exe` with the DLLs beside it (`librexruntimerd.dll`,
`rexgpu-xenosrd.dll`, `libTracyClientrd.dll`, llvm-mingw's `libc++.dll` and `libunwind.dll`) and the data in a
disposable folder, as the portable launcher does (`Play KUF.exe --print-launch` shows its exact command line):
`--game_data_root=<game folder> --user_data_root=<test>\user --content_root=<test>\saves` and the cache and log
paths. Logs: `<test>\logs\kuf_cod.log`; a crash writes a report (`KUF-COD CRASH REPORT v1`) with the guest
backtrace; `tools/symbolize_crash.py` maps host addresses.

## Packaging

`tools/package/make_windows_portable.sh` and `make_windows_setup.sh` were written for the Linux machine (they use the
Linux-hosted llvm-mingw for `llvm-objcopy`/`llvm-nm` and both llvm-mingw copies to build the player's builder:
`tools/package/make_builder_windows.py`). On Windows they need the Windows copy's tools instead; adapt them rather than
packaging by hand, and keep the audit (`tools/package/audit_portable.py`) in the flow.
