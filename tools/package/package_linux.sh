#!/usr/bin/env bash
# Builds a relocatable Linux package of the current build. It contains no code of the game: the game's program is made
# on the player's machine from their own files (builder/, docs/LOCAL_BUILD.md).
#
#   tools/package/package_linux.sh [--build DIR] [--out DIR] [--version V] [--strip] [--signers FILE] [--host-build]
#                                  [--compiler DIR] [--test-compiler-script FILE] [--test-builder DIR]
#
# Output: <out>/kuf-cod-recomp-<version>-linux-x86_64.tar.gz with one top directory:
#   bin/ (the runtime libraries and the code generator, RUNPATH $ORIGIN), bin/compat/libstdc++.so.6 (the C++ library the
#   portable build links against; the launcher uses it only when the system's is older), builder/ (kuf-localbuild, the
#   compiler, headers, the project's own prebuilt code: tools/package/make_builder.py), kuf-cod.sh (launcher),
#   install.sh, kuf_update.py, VERSION, REQUIRES, MANIFEST.sha256 (every other file), README-PACKAGE.txt, licenses/.
# The build is the portable one (build/linux-amd64-portable: glibc 2.31, docs/STEAM_DECK.md "Portable build").
# Its binaries are checked: nothing may need a newer glibc than the baseline, nor a newer libstdc++ than the one
# bundled. REQUIRES records what they need ("glibc 2.31", "glibcxx 3.4.33", "glibcxx-bundled 3.4.33"); the setup,
# the updater and the release audit read it. --host-build packages a build made against this machine's own
# libraries instead (local tests only: it runs on this machine's distribution and newer, nowhere else).
# --compiler is the LLVM build (tools/toolchain/build_local_compiler.sh); --test-compiler-script a stand-in for it (tests
# of the packaging only: such a package cannot be released); --test-builder DIR copies DIR as builder/ (the same). No game data: the game folder is the player's, chosen at the first start. --strip removes debug symbols (smaller;
# crash reports then name fewer host frames). --signers embeds that release public key (allowed_signers format)
# instead of tools/package/allowed_signers (the throwaway key of a disposable update test). Sign it with
# tools/package/sign_package.sh; a whole release: tools/package/make_release.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build/linux-amd64-portable"
HOST_BUILD=0
OUT="$ROOT/build/packages"
VERSION=""
STRIP=0
SIGNERS="$ROOT/tools/package/allowed_signers"
COMPILER="$ROOT/build/toolchains/llvm-23.1.3-build"
TEST_COMPILER=""
TEST_BUILDER=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) BUILD="$(realpath "$2")"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --strip) STRIP=1; shift ;;
    --signers) SIGNERS="$(realpath "$2")"; shift 2 ;;
    --host-build) HOST_BUILD=1; shift ;;
    --compiler) COMPILER="$(realpath "$2")"; shift 2 ;;
    --test-compiler-script) TEST_COMPILER="$(realpath "$2")"; shift 2 ;;
    --test-builder) TEST_BUILDER="$(realpath "$2")"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
HEADER="$BUILD/generated_include/kuf_version_generated.h"
[[ -f "$BUILD/bin/rexgluerd" && -f "$HEADER" ]] || { echo "no build in $BUILD (build kuf_cod first)" >&2; exit 1; }
BUILT="$(sed -n 's/^#define KUFCOD_VERSION "\(.*\)"$/\1/p' "$HEADER")"
VERSION="${VERSION:-$BUILT}"
if [[ "$VERSION" != "$BUILT" ]]; then
  echo "version $VERSION differs from the build's $BUILT (rebuild with KUFCOD_RELEASE_VERSION=$VERSION)" >&2
  exit 1
fi
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$ ]] || { echo "bad version $VERSION" >&2; exit 1; }
NAME="kuf-cod-recomp-$VERSION-linux-x86_64"
mkdir -p "$OUT"
STAGE="$(mktemp -d "$OUT/.stage-XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
P="$STAGE/$NAME"
mkdir -p "$P/bin" "$P/licenses"
for f in rexgluerd librexruntimerd.so librexgpu-xenosrd.so libTracyClientrd.so; do
  [[ -f "$BUILD/bin/$f" ]] || { echo "missing $BUILD/bin/$f" >&2; exit 1; }
  install -m 0755 "$BUILD/bin/$f" "$P/bin/$f"
  [[ $STRIP -eq 1 ]] && strip --strip-debug "$P/bin/$f"
done
# The C++ library of the portable build (cmake/steamrt3-sniper.cmake), with its licence.
LIBSTDCXX="$(sed -n 's/^KUFCOD_LIBSTDCXX:PATH=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
if [[ -n "$LIBSTDCXX" ]]; then
  mkdir -p "$P/bin/compat"
  install -m 0755 "$(realpath "$LIBSTDCXX/lib/libstdc++.so.6")" "$P/bin/compat/libstdc++.so.6"
  [[ $STRIP -eq 1 ]] && strip --strip-debug "$P/bin/compat/libstdc++.so.6"
  GCC_SRC="$ROOT/build/toolchains/gcc-$(basename "$LIBSTDCXX" | sed 's/^libstdcxx-//')"
  install -m 0644 "$GCC_SRC/COPYING3" "$P/licenses/libstdc++-COPYING3"
  install -m 0644 "$GCC_SRC/COPYING.RUNTIME" "$P/licenses/libstdc++-COPYING.RUNTIME"
elif [[ $HOST_BUILD -eq 0 ]]; then
  echo "$BUILD is not the portable build (cmake --preset linux-amd64-portable; --host-build for a local test)" >&2
  exit 1
fi
# The local build: what it takes to make the game's program on the player's machine.
if [[ -n "$TEST_BUILDER" ]]; then
  cp -r "$TEST_BUILDER" "$P/builder"
else
  python3 -I "$ROOT/tools/package/make_builder.py" --build "$BUILD" --out "$P" --compiler "$COMPILER" \
    ${TEST_COMPILER:+--plain-compiler-script "$TEST_COMPILER"} >&2
  [[ -x "$ROOT/build/installer/kuf-localbuild" ]] || { echo "no build/installer/kuf-localbuild (ninja -C build/installer)" >&2; exit 1; }
  install -m 0755 "$ROOT/build/installer/kuf-localbuild" "$P/builder/kuf-localbuild"
  [[ $STRIP -eq 1 ]] && strip --strip-debug "$P/builder/kuf-localbuild"
fi
# What the binaries need from the system (their newest symbol versions), checked against the baseline.
max_version() {
  [[ $# -gt 0 ]] || return 0
  # Only what they import (*UND*): a library's own symbol versions are not needs.
  { objdump -T "$@" 2>/dev/null || true; } | { grep -F '*UND*' || true; } | { grep -oE "$PATTERN" || true; } |
    sed "s/^$PREFIX//" | sort -uV | tail -1
}
PATTERN='GLIBC_2\.[0-9]+(\.[0-9]+)?'; PREFIX=GLIBC_
mapfile -t ELF < <(find "$P/bin" "$P/builder/kuf-localbuild" -type f -name '*' -exec sh -c 'head -c4 "$1" | grep -q ELF && echo "$1"' _ {} \;)
NEED_GLIBC="$(max_version "${ELF[@]}")"
PATTERN='GLIBCXX_3\.4\.[0-9]+'; PREFIX=GLIBCXX_
mapfile -t GAME_ELF < <(printf '%s\n' "${ELF[@]}" | grep -v '/bin/compat/' || true)
NEED_GLIBCXX="$(max_version "${GAME_ELF[@]}")"
BASELINE="$(sed -n 's/^set(KUFCOD_GLIBC_BASELINE \(.*\))$/\1/p' "$ROOT/cmake/steamrt3-sniper-version.cmake")"
{
  [[ -n "$NEED_GLIBC" ]] && echo "glibc $NEED_GLIBC"
  [[ -n "$NEED_GLIBCXX" ]] && echo "glibcxx $NEED_GLIBCXX"
  if [[ -f "$P/bin/compat/libstdc++.so.6" ]]; then
    echo "glibcxx-bundled $(objdump -p "$P/bin/compat/libstdc++.so.6" | grep -oE 'GLIBCXX_3\.4\.[0-9]+' | sed 's/GLIBCXX_//' | sort -uV | tail -1)"
  fi
} > "$P/REQUIRES"
if [[ $HOST_BUILD -eq 0 ]]; then
  newer() { [[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" == "$1" && "$1" != "$2" ]]; }
  newer "$NEED_GLIBC" "$BASELINE" && { echo "the binaries need glibc $NEED_GLIBC (baseline $BASELINE): not portable" >&2; exit 1; }
  BUNDLED="$(sed -n 's/^glibcxx-bundled //p' "$P/REQUIRES")"
  newer "$NEED_GLIBCXX" "$BUNDLED" && { echo "the binaries need GLIBCXX $NEED_GLIBCXX, the bundled libstdc++ has $BUNDLED" >&2; exit 1; }
fi
install -m 0755 "$ROOT/tools/package/kuf-cod.sh" "$P/kuf-cod.sh"
install -m 0755 "$ROOT/tools/package/install.sh" "$P/install.sh"
install -m 0755 "$ROOT/tools/updater/kuf_update.py" "$P/kuf_update.py"
install -m 0644 "$ROOT/tools/package/README-PACKAGE.txt" "$P/README-PACKAGE.txt"
# The release public key (updates are verified against it; tools/package/sign_package.sh --new-key makes the pair).
if [[ -f "$SIGNERS" ]]; then
  install -m 0644 "$SIGNERS" "$P/allowed_signers"
fi
install -m 0644 "$ROOT/LICENSE" "$P/licenses/PROJECT-LICENSE.txt"
install -m 0644 "$ROOT/NOTICE" "$P/licenses/PROJECT-NOTICE.txt"
install -m 0644 "$ROOT/thirdparty/rexglue-sdk/LICENSE" "$P/licenses/rexglue-sdk-LICENSE"
# The licences of the SDK's third-party code linked into the binaries (tools/package/THIRD-PARTY-NOTICES.txt lists
# them); a notice that lives only in a source file is copied from that file's header (path:first-last line).
install -m 0644 "$ROOT/tools/package/THIRD-PARTY-NOTICES.txt" "$P/licenses/THIRD-PARTY-NOTICES.txt"
install -m 0644 "$ROOT"/tools/package/licenses/* "$P/licenses/"  # kept here: not in the SDK's tree, or written for it
TP="$ROOT/thirdparty/rexglue-sdk/thirdparty"
for spec in FFmpeg-LICENSE.md=FFmpeg/LICENSE.md FFmpeg-COPYING.LGPLv2.1=FFmpeg/COPYING.LGPLv2.1 \
    libmspack-COPYING.LIB=libmspack/libmspack/COPYING.LIB SDL3-LICENSE.txt=sdl3/LICENSE.txt SDL3-hidapi-LICENSE-bsd.txt=sdl3/src/hidapi/LICENSE-bsd.txt fmt-LICENSE=fmt/LICENSE \
    spdlog-LICENSE=spdlog/LICENSE imgui-LICENSE.txt=imgui/LICENSE.txt tracy-LICENSE=tracy/LICENSE \
    glslang-LICENSE.txt=glslang/LICENSE.txt spirv-tools-LICENSE=spirv-tools/LICENSE \
    spirv-headers-LICENSE=spirv-headers/LICENSE vulkan-headers-LICENSE.md=vulkan-headers/LICENSE.md \
    vulkan-headers-Apache-2.0.txt=vulkan-headers/LICENSES/Apache-2.0.txt vulkan-headers-MIT.txt=vulkan-headers/LICENSES/MIT.txt \
    vulkan-memory-allocator-LICENSE.txt=vulkan-memory-allocator/LICENSE.txt xxHash-LICENSE=xxHash/LICENSE \
    o1heap-LICENSE=o1heap/LICENSE simde-COPYING=simde/COPYING tomlplusplus-LICENSE=tomlplusplus/LICENSE \
    cli11-LICENSE=cli11/LICENSE utfcpp-LICENSE=utfcpp/LICENSE aes_128-LICENSE=aes_128/LICENSE \
    crypto-des-LICENSE=crypto/des/LICENSE crypto-TinySHA1-NOTICE.txt=crypto/TinySHA1.hpp:1-24 \
    crypto-sha256-NOTICE.txt=crypto/sha256.h:1-6 crypto-rijndael-NOTICE.txt=crypto/rijndael-alg-fst.h:1-25 \
    stb_image-NOTICE.txt=stb/stb_image.h:7945-7985 renderdoc_app-NOTICE.txt=renderdoc/renderdoc_app.h:1-23; do
  name="${spec%%=*}" src="${spec#*=}"
  if [[ "$src" == *:* ]]; then
    [[ -f "$TP/${src%%:*}" ]] || { echo "missing licence source $TP/${src%%:*}" >&2; exit 1; }
    range="${src##*:}"; sed -n "${range%-*},${range#*-}p" "$TP/${src%%:*}" > "$P/licenses/$name"
    chmod 0644 "$P/licenses/$name"
  else
    [[ -f "$TP/$src" ]] || { echo "missing licence $TP/$src" >&2; exit 1; }
    install -m 0644 "$TP/$src" "$P/licenses/$name"
  fi
done
# The build tools' licences: LLVM, and the C library and GCC parts the local build links (their sources: the release's
# source archive, tools/package/make_source_archive.sh).
SYSROOT_BUILD="$(sed -n 's/^KUFCOD_SNIPER_SYSROOT:PATH=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
if [[ -n "$SYSROOT_BUILD" ]]; then
  install -m 0644 "$SYSROOT_BUILD/usr/share/doc/libc6/copyright" "$P/licenses/glibc-copyright"
  install -m 0644 "$SYSROOT_BUILD/usr/share/doc/libgcc-s1/copyright" "$P/licenses/gcc-runtime-copyright" 2>/dev/null ||
    install -m 0644 "$SYSROOT_BUILD/usr/share/doc/gcc-10-base/copyright" "$P/licenses/gcc-runtime-copyright"
  install -m 0644 "$SYSROOT_BUILD/usr/share/doc/libtbb-dev/copyright" "$P/licenses/tbb-copyright"
  if [[ -d "$SYSROOT_BUILD/usr/share/doc/linux-libc-dev" ]]; then
    install -m 0644 "$SYSROOT_BUILD/usr/share/doc/linux-libc-dev/copyright" "$P/licenses/linux-headers-copyright"
  fi
fi
for f in LICENSE.TXT; do
  L="$ROOT/build/toolchains/llvm-23.1.3-src/llvm/$f"
  [[ -f "$L" ]] && install -m 0644 "$L" "$P/licenses/llvm-LICENSE.TXT"
done
echo "$VERSION" > "$P/VERSION"
(cd "$P" && find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | LC_ALL=C sort | xargs -d '\n' sha256sum > MANIFEST.sha256)
TARBALL="$OUT/$NAME.tar.gz"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$(git -C "$ROOT" log -1 --format=%ct)" \
    -C "$STAGE" -czf "$TARBALL.partial" "$NAME"
mv "$TARBALL.partial" "$TARBALL"
echo "$TARBALL"
