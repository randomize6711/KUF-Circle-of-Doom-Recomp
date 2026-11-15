#!/usr/bin/env bash
# The native Windows build in one command (M17A, docs/WINDOWS_BUILD.md): from a fresh clone to kuf_cod.exe and
# Play KUF.exe, without manual steps. Run it in Git Bash from anywhere; safe to rerun (every step checks first).
#
#   tools/windows/build.sh [--xex FILE] [--jobs N] [--no-launcher]
#
#   --xex FILE   the game's executable for code generation, unencrypted (for example a portable folder's
#                Data\game\default.xex, which Play KUF.exe decrypts there). Copied once to build/input/v1/ (only
#                read); later runs reuse that copy. Its memory image must be the supported version's.
#
# Steps: 1. tools (git, cmake >= 3.28, ninja, Python 3; clear errors, nothing installed); 2. the SDK and its
# submodules at their pinned commits, with LF line endings (git checks every object's hash); 3. llvm-mingw, the
# pinned release (tools/toolchain/setup_llvm_mingw.sh: SHA-256 checked); 4. the SDK patches; 5. the executable;
# 6. the game (preset mingw-amd64-relwithdebinfo; CMake puts llvm-mingw's libc++.dll and libunwind.dll beside the
# code generator and the game); 7. the launcher and setup (build/installer-mingw).
#
# Also runs on Linux as the cross build (then the code generator is a native build: REXGLUE_HOST_REXGLUE), which is
# how this script is tested on the development machine.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
XEX="" JOBS="" LAUNCHER=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --xex) XEX="${2:?--xex needs a file}"; shift 2 ;;
    --jobs) JOBS="${2:?--jobs needs a number}"; shift 2 ;;
    --no-launcher) LAUNCHER=0; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
  esac
done

WINDOWS_HOST=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS_HOST=1 ;; esac
step() { echo; echo "== $*"; }
fail() { echo; echo "BUILD STOPPED: $*" >&2; exit 1; }

step "1. tools"
need() {  # name, how to get it
  command -v "$1" > /dev/null || fail "$1 not found on the PATH. $2"
}
need git "Install Git for Windows (https://git-scm.com/download/win) and run this script in Git Bash."
need cmake "Install CMake 3.28 or newer (https://cmake.org/download/, or: winget install Kitware.CMake)."
need ninja "Install Ninja (https://github.com/ninja-build/ninja/releases, or: winget install Ninja-build.Ninja)."
need curl "curl is part of Git for Windows and of Windows 10/11."
need sha256sum "sha256sum is part of Git for Windows."
cmake_version="$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?')"
printf '3.28\n%s\n' "$cmake_version" | sort -V -C || fail "CMake $cmake_version is too old: 3.28 or newer is needed."
# Python: python3 where it is real; on Windows "python3" (and often "python") can be the Microsoft Store's stub,
# which prints an advertisement instead of running anything.
PY=""
for candidate in python3 python py; do
  if command -v "$candidate" > /dev/null && "$candidate" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' \
       > /dev/null 2>&1; then PY="$candidate"; break; fi
done
[[ -n "$PY" ]] || fail "Python 3.9 or newer not found (the Microsoft Store's python stub does not count). Install it from
  https://www.python.org/downloads/ (or: winget install Python.Python.3.12) and tick \"Add python.exe to PATH\"."
echo "git $(git --version | cut -d' ' -f3), cmake $cmake_version, ninja $(ninja --version), $PY $("$PY" -c 'import platform; print(platform.python_version())')"

step "2. the SDK and its submodules (pinned commits, LF line endings)"
[[ -d .git || -f .git ]] || fail "$ROOT is not a git checkout: clone the repository (docs/WINDOWS_BUILD.md)."
# core.autocrlf=false for these clones only: the SDK patches and the shell scripts need the files as committed.
git -c core.autocrlf=false submodule update --init thirdparty/rexglue-sdk
git -C thirdparty/rexglue-sdk config core.autocrlf false
git -C thirdparty/rexglue-sdk -c core.autocrlf=false submodule update --init --depth 1
if grep -q $'\r' thirdparty/rexglue-sdk/CMakeLists.txt; then
  fail "the SDK was checked out with Windows (CRLF) line endings, which the patches cannot apply to. Check it out
  again with LF endings (this discards changes inside thirdparty/rexglue-sdk only; the patches are reapplied):
    git -C thirdparty/rexglue-sdk config core.autocrlf false
    git -C thirdparty/rexglue-sdk rm -rq --cached . && git -C thirdparty/rexglue-sdk reset -q --hard
  then run this script again."
fi
echo "SDK $(git -C thirdparty/rexglue-sdk rev-parse --short HEAD), $(git -C thirdparty/rexglue-sdk submodule status | wc -l) submodules"

step "3. llvm-mingw (pinned release, SHA-256 checked)"
tools/toolchain/setup_llvm_mingw.sh

step "4. SDK patches"
tools/apply_sdk_patches.sh > /dev/null
tools/apply_sdk_patches.sh --check > /dev/null || fail "the SDK patches do not apply (tools/apply_sdk_patches.sh --check)."
echo "$(ls patches/rexglue-sdk/*.patch | wc -l) patches applied"

step "5. the game's executable"
IN="build/input/v1/default.xex"
WANT_IMAGE=c502f6617bf1ef1112474d6eb1de5bb6752625f0b7688691c1f8ce0c50c90627
check_image() {  # xex: the memory image is the supported version's
  local tmp; tmp="$(mktemp -d)"
  if ! "$PY" -I tools/xeximage.py "$1" "$tmp" > "$tmp/out.txt" 2>&1; then
    local why; why="$(tail -1 "$tmp/out.txt")"; rm -rf "$tmp"
    fail "$1 cannot be read: $why
  Use the unencrypted default.xex that Play KUF.exe writes into a portable folder's Data\\game\\ (it decrypts a
  retail one there), or one from an extracted unencrypted game folder."
  fi
  local got; got="$(sha256sum "$tmp/image.bin" | cut -d' ' -f1)"; rm -rf "$tmp"
  [[ "$got" == "$WANT_IMAGE" ]] || fail "$1 is not the supported version of the game (memory image $got)."
}
if [[ -f "$IN" ]]; then
  check_image "$IN"; echo "using $IN (already staged)"
else
  [[ -n "$XEX" ]] || fail "no executable staged yet: run again with --xex <path to an unencrypted default.xex>."
  [[ -f "$XEX" ]] || fail "--xex: no such file: $XEX"
  check_image "$XEX"
  mkdir -p "$(dirname "$IN")"
  cp "$XEX" "$IN.part" && chmod a-w "$IN.part" && mv "$IN.part" "$IN"
  echo "staged $IN"
fi

step "6. the game (kuf_cod.exe)"
PRESET=mingw-amd64-relwithdebinfo
EXTRA=(-DKUFCOD_BUILD_TESTS=OFF)
if [[ $WINDOWS_HOST == 0 ]]; then  # the cross build: a native code generator
  [[ -d build/linux-amd64-relwithdebinfo ]] || cmake --preset linux-amd64-relwithdebinfo > /dev/null
  ninja -C build/linux-amd64-relwithdebinfo rexglue > /dev/null
  EXTRA+=(-DREXGLUE_HOST_REXGLUE="$ROOT/build/linux-amd64-relwithdebinfo/bin/rexgluerd")
fi
configure() {
  cmake --preset "$PRESET" "${EXTRA[@]}" > build/windows-build-configure.log 2>&1 \
    || { tail -30 build/windows-build-configure.log >&2; fail "CMake could not configure the game (build/windows-build-configure.log)."; }
}
# The generated code exists only after the code generator ran, and CMake lists it at configure time: configure,
# generate (skipped when up to date), configure again with the generated sources, build.
configure
ninja -C "build/$PRESET" ${JOBS:+-j "$JOBS"} kuf_cod_codegen || fail "code generation failed (the output above)."
configure
# rexglue: the Windows code generator itself (the packages carry it for the player's local build; in a cross build
# nothing else depends on it).
ninja -C "build/$PRESET" ${JOBS:+-j "$JOBS"} kuf_cod rexglue || fail "the game did not build (the output above)."
for f in kuf_cod.exe librexruntimerd.dll rexgpu-xenosrd.dll libTracyClientrd.dll libc++.dll libunwind.dll rexgluerd.exe; do
  [[ -f "build/$PRESET/bin/$f" ]] || fail "build/$PRESET/bin/$f is missing after the build."
done
echo "build/$PRESET/bin: kuf_cod.exe and its DLLs"

if [[ $LAUNCHER == 1 ]]; then
  step "7. the launcher and setup (Play KUF.exe)"
  cmake -S installer -B build/installer-mingw -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw-x86_64.cmake" > build/windows-installer-configure.log 2>&1 \
    || { tail -30 build/windows-installer-configure.log >&2; fail "CMake could not configure installer/ (build/windows-installer-configure.log)."; }
  ninja -C build/installer-mingw ${JOBS:+-j "$JOBS"} || fail "the launcher did not build (the output above)."
  echo "build/installer-mingw: Play KUF.exe, kuf-localbuild.exe"
fi

echo
echo "BUILD DONE: build/$PRESET/bin/kuf_cod.exe$([[ $LAUNCHER == 1 ]] && echo ", build/installer-mingw/Play KUF.exe")"
