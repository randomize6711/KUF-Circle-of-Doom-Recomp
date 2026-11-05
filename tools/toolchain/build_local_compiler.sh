#!/usr/bin/env bash
# Builds the compiler the setup carries (docs/LOCAL_BUILD.md): Clang and LLD of the pinned LLVM release from its source,
# against the Steam Runtime 3 sysroot with the C++ library linked in, so they run wherever the game does (glibc 2.31).
# In build/toolchains only; nothing is installed on this machine. Idempotent: an unfinished build continues.
#   tools/toolchain/build_local_compiler.sh      (a few hours at the default JOBS=4; pauses while a Windows game runs)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TC="$ROOT/build/toolchains"
LLVM_VERSION=23.1.3
LLVM_SHA256=c44186a7762ed28954be72e5ff6df9808e0779d4f1bf014ecc4e7e211d31ee34
JOBS="${JOBS:-4}"
SRC="$TC/llvm-$LLVM_VERSION-src"; B="$TC/llvm-$LLVM_VERSION-build"; OUT="$TC/llvm-$LLVM_VERSION"
TAR="$TC/llvm-project-$LLVM_VERSION.src.tar.xz"
if ! { [[ -f "$TAR" ]] && echo "$LLVM_SHA256  $TAR" | sha256sum -c --quiet - 2>/dev/null; }; then
  curl -fL --progress-bar -o "$TAR.part" \
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/llvm-project-$LLVM_VERSION.src.tar.xz"
  echo "$LLVM_SHA256  $TAR.part" | sha256sum -c --quiet - || { rm -f "$TAR.part"; echo "checksum mismatch" >&2; exit 1; }
  mv "$TAR.part" "$TAR"
fi
if [[ ! -f "$SRC/.kuf-extracted" ]]; then
  rm -rf "$SRC" && mkdir -p "$SRC"
  tar -xJf "$TAR" -C "$SRC" --strip-components=1 llvm-project-$LLVM_VERSION.src/{llvm,clang,lld,cmake,third-party,libunwind,libc}
  touch "$SRC/.kuf-extracted"
fi
if [[ ! -f "$B/build.ninja" ]]; then
  cmake -S "$SRC/llvm" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/steamrt3-sniper.cmake" -DKUFCOD_STATIC_LIBSTDCXX=ON -DCMAKE_C_FLAGS=-march=x86-64-v2 \
    -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-linux-gnu \
    -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_THREADS=ON \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_BUILD_TOOLS=ON -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_PLUGIN_SUPPORT=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_APPEND_VC_REV=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF \
    -DLLVM_USE_LINKER=lld >/dev/null
fi
# A Windows game on this shared machine has priority: stop the build, continue when it is gone (ninja resumes).
while :; do
  while pgrep -f "[.]exe" >/dev/null; do sleep 60; done
  nice -n 19 ninja -C "$B" -j"$JOBS" clang lld & NINJA=$!
  while kill -0 "$NINJA" 2>/dev/null; do
    if pgrep -f "[.]exe" >/dev/null; then kill "$NINJA"; break; fi
    sleep 20
  done
  if wait "$NINJA"; then break; fi
  pgrep -f "[.]exe" >/dev/null || { echo "the compiler build failed" >&2; exit 1; }
done
ls -la "$B/bin" | head -20
echo "built in $B"
