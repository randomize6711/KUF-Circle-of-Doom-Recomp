#!/usr/bin/env bash
# Prepares the Windows build's compiler (docs/WINDOWS.md; M16, the player's choice of llvm-mingw), in build/toolchains
# only (nothing is installed on this machine). Pinned release and SHA-256 (GitHub's asset digests):
#   1. llvm-mingw-<REL>-ucrt-ubuntu-22.04-x86_64: runs here, targets Windows x64 (UCRT, Windows 10 and 11), used to
#      build the Windows runtime, setup and launcher (cmake/llvm-mingw-x86_64.cmake);
#   2. llvm-mingw-<REL>-ucrt-x86_64.zip: the same compiler built for Windows, the source of what the Windows packages
#      carry for the player's local build (only the parts that build needs: tools/package/make_windows_toolchain.py);
#   3. the release's source (the build scripts; LLVM and mingw-w64 sources are in the toolchain-sources asset).
# LLVM 23.1.3: the same version as the Linux local compiler (tools/toolchain/build_local_compiler.sh). Idempotent.
# On a Windows host (Git Bash) only 2 is needed: it is both the build's compiler and the packages' source; it is
# unpacked with Python's zipfile (Git for Windows has no unzip).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TC="$ROOT/build/toolchains"
REL=20261006
LINUX_SHA=5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4
WIN_SHA=317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a
BASE="https://github.com/mstorsjo/llvm-mingw/releases/download/$REL"
mkdir -p "$TC/downloads"

fetch() {  # url dest sha256
  if [[ -f "$2" ]] && echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null; then return 0; fi
  curl -fL --progress-bar -o "$2.part" "$1"
  echo "$3  $2.part" | sha256sum -c --quiet - || { rm -f "$2.part"; echo "checksum mismatch: $1" >&2; exit 1; }
  mv "$2.part" "$2"
}

WINDOWS_HOST=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS_HOST=1 ;; esac
PY=python3
if [[ $WINDOWS_HOST == 1 ]] || ! command -v python3 > /dev/null; then PY=python; fi
unzip_to() {  # zip dir
  if command -v unzip > /dev/null; then (cd "$2" && unzip -q "$1"); return; fi
  "$PY" -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$1" "$2"
}
L="llvm-mingw-$REL-ucrt-ubuntu-22.04-x86_64"
if [[ $WINDOWS_HOST == 0 && ! -f "$TC/$L/.kuf-extracted" ]]; then
  fetch "$BASE/$L.tar.xz" "$TC/downloads/$L.tar.xz" "$LINUX_SHA"
  rm -rf "${TC:?}/$L"; tar -xJf "$TC/downloads/$L.tar.xz" -C "$TC"
  touch "$TC/$L/.kuf-extracted"
fi
W="llvm-mingw-$REL-ucrt-x86_64"
if [[ ! -f "$TC/$W/.kuf-extracted" ]]; then
  fetch "$BASE/$W.zip" "$TC/downloads/$W.zip" "$WIN_SHA"
  rm -rf "${TC:?}/$W"; unzip_to "$TC/downloads/$W.zip" "$TC"
  touch "$TC/$W/.kuf-extracted"
fi
if [[ $WINDOWS_HOST == 1 ]]; then
  "$TC/$W/bin/x86_64-w64-mingw32-clang.exe" --version | head -1
  echo "llvm-mingw $REL: $TC/$W (the build's compiler and the packages' source)"
else
  "$TC/$L/bin/x86_64-w64-mingw32-clang" --version | head -1
  echo "llvm-mingw $REL: $TC/$L (host), $TC/$W (for the Windows packages)"
fi
