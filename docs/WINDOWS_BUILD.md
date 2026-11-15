# Building on Windows (M17A)

For developers. Players never build anything: the packages prepare the game on their PC by themselves
(docs/PORTABLE.md, docs/LOCAL_BUILD.md).

## Prerequisites (nothing else, no administrator rights, no Developer Mode)

| Tool | Get it | Notes |
|---|---|---|
| Git for Windows | https://git-scm.com/download/win, or `winget install Git.Git` | gives Git Bash, where the scripts run |
| CMake 3.28+ | https://cmake.org/download/, or `winget install Kitware.CMake` | |
| Ninja | `winget install Ninja-build.Ninja` | |
| Python 3.9+ | https://www.python.org/downloads/, or `winget install Python.Python.3.12` | tick "Add python.exe to PATH"; the Microsoft Store's `python3` stub does not count |

A Vulkan driver (current AMD, NVIDIA or Intel) to run the game. The compiler, llvm-mingw, is fetched by the build
itself (pinned release, SHA-256 checked) into `build\toolchains\`.

## Build

In Git Bash:

```sh
git -c core.autocrlf=false clone https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp.git "KUF COD"
cd "KUF COD"
tools/windows/build.sh --xex "/c/Users/<you>/KUF Portable/Data/game/default.xex"
```

`--xex` is the game's unencrypted executable: a portable folder's `Data\game\default.xex` (Play KUF.exe decrypts a
retail one there) or one from an extracted unencrypted game folder. It is copied once to `build\input\v1\` (only
read) and checked: an encrypted or other-version executable is refused with the reason. Later runs need no `--xex`.

The script (safe to rerun; each step checks first):

1. checks the tools and their versions;
2. checks out the SDK and its 22 submodules at their pinned commits, with LF line endings (git verifies every object);
3. sets up llvm-mingw 20261006 (tools/toolchain/setup_llvm_mingw.sh: download, SHA-256, unpack);
4. applies the SDK patches;
5. stages the executable;
6. configures, generates the game's code, configures again, builds `kuf_cod.exe` and the code generator;
7. builds `Play KUF.exe` and `kuf-localbuild.exe`.

Output: `build\mingw-amd64-relwithdebinfo\bin\` (`kuf_cod.exe` with its DLLs, llvm-mingw's `libc++.dll` and
`libunwind.dll` included) and `build\installer-mingw\`.

What used to be manual (K80) and how it is handled now:

| Before | Now |
|---|---|
| SDK submodules missing in a non-recursive clone | step 2; CMake also stops with the command to run |
| Symlinks checked out as text (libmspack, o1heap) | not needed: the SDK builds libmspack from its own sources (patch 0020) |
| `libc++.dll`/`libunwind.dll` copied beside `rexgluerd.exe` | CMake copies them into `bin\` at configure time |
| `KUFCOD_LLVM_MINGW` set by hand | the toolchain file finds the Windows llvm-mingw in `build\toolchains\` itself |
| The installer's toolchain path had to be absolute | build.sh passes it absolute |
| `python3` vs `python` | build.sh and setup_llvm_mingw.sh find a real Python |
| CRLF checkouts break the scripts and patches | `.gitattributes` (LF) and LF submodule checkouts; a CRLF SDK is detected and explained |
| WSL's `bash.exe` found instead of Git's | CMake takes bash from Git for Windows' own folder |

## Verified

- 2026-11-10, Linux cross build of a fresh clone with Git for Windows' defaults (`core.symlinks=false`,
  `core.autocrlf=true`), in a path with spaces and `ü`: `tools/windows/build.sh` built everything from nothing, and a
  second run (with a new SDK patch) applied only the new patch and finished.
- **Not yet run on a Windows PC** with the script (the first native build, 2026-11-08, was done by hand:
  docs/WINDOWS_DEV_HANDOFF.md).

## Packaging

`tools/package/make_windows_portable.sh VERSION` after the build (on Windows: relink once with
`ninja -C build/mingw-amd64-relwithdebinfo -d keeprsp kuf_cod`, deleting `bin\kuf_cod.exe` first, so the link's
response file stays for `make_builder_windows.py`).
