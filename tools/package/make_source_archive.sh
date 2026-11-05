#!/usr/bin/env bash
# The source archives a release carries (docs/LOCAL_BUILD.md "Licences"), so its GPL and LGPL obligations are met by the
# release itself and do not depend on third-party repositories staying online:
#   kuf-cod-recomp-<version>-source.tar.xz
#     this project at HEAD and the SDK with its submodules as they were built: the project's patches applied and the
#     console keys removed (tools/apply_sdk_patches.sh). Includes FFmpeg, libmspack and the GNU binutils disassembler.
#   kuf-cod-recomp-<version>-toolchain-sources.tar
#     GCC 14.2.0 (libstdc++) and the Steam Runtime 3 source packages of the GCC 10 runtime parts, glibc and the Linux
#     headers that the local build links or includes, and the AppImage runtime with libfuse and squashfuse (pinned
#     SHA-256; cached in build/toolchains/sources).
# No game data and no generated code: the audit checks the first archive.
#   tools/package/make_source_archive.sh --version V --out DIR
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="" OUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --out) OUT="$(realpath -m "$2")"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[[ -n "$VERSION" && -n "$OUT" ]] || { echo "--version V --out DIR" >&2; exit 2; }
"$ROOT/tools/apply_sdk_patches.sh" --check >/dev/null || { echo "the SDK's patches are not applied" >&2; exit 1; }
mkdir -p "$OUT"
NAME="kuf-cod-recomp-$VERSION-source"
STAGE="$(mktemp -d "$OUT/.source-XXXXXX")"
trap 'rm -rf "${STAGE:?}"' EXIT
mkdir -p "$STAGE/$NAME"
git -C "$ROOT" archive HEAD | tar -x -C "$STAGE/$NAME"
SDK="$ROOT/thirdparty/rexglue-sdk"
copy_tracked() {  # repository, destination: its files as they are on disk, also those the patches add. Prebuilt Windows tools left out.
  (cd "$1" && git ls-files -z --cached --others --exclude-standard | grep -zv '^tools/binutils/' |
     while IFS= read -r -d '' f; do if [[ -f "$f" ]]; then printf '%s\0' "$f"; fi; done |
     tar --no-recursion --null -T - -cf -) | tar -x -C "$2"
}
mkdir -p "$STAGE/$NAME/thirdparty/rexglue-sdk"
copy_tracked "$SDK" "$STAGE/$NAME/thirdparty/rexglue-sdk"
git -C "$SDK" submodule foreach --quiet --recursive 'echo "$displaypath"' | while read -r sm; do
  mkdir -p "$STAGE/$NAME/thirdparty/rexglue-sdk/$sm"
  copy_tracked "$SDK/$sm" "$STAGE/$NAME/thirdparty/rexglue-sdk/$sm"
  mkdir -p "$STAGE/$NAME/thirdparty/rexglue-sdk/$sm/.git"  # the SDK's CMake only asks whether this exists
done
git -C "$ROOT" rev-parse HEAD > "$STAGE/$NAME/SOURCE-COMMIT.txt"
git -C "$SDK" submodule status --recursive | cut -c2- | sed 's/ (.*//' > "$STAGE/$NAME/SOURCE-SDK-SUBMODULES.txt"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$(git -C "$ROOT" log -1 --format=%ct)" -C "$STAGE" -cf - "$NAME" |
  xz -T4 -6 > "$OUT/$NAME.tar.xz"
echo "$OUT/$NAME.tar.xz"

# The toolchain's sources, from their publishers.
TC="$ROOT/build/toolchains"
SR="https://repo.steampowered.com/steamrt3/images/$(sed -n 's/^set(KUFCOD_SNIPER_VERSION \(.*\))$/\1/p' "$ROOT/cmake/steamrt3-sniper-version.cmake")/sources"
D="$TC/sources"
mkdir -p "$D"
while read -r sha file; do
  if ! { [[ -f "$D/$file" ]] && echo "$sha  $D/$file" | sha256sum -c --quiet - 2>/dev/null; }; then
    curl -fsSL -o "$D/$file.part" "$SR/$(printf %s "$file" | sed 's/+/%2B/g')"
    echo "$sha  $D/$file.part" | sha256sum -c --quiet - || { rm -f "${D:?}/${file:?}.part"; echo "checksum mismatch: $file" >&2; exit 1; }
    mv "$D/$file.part" "$D/$file"
  fi
done <<'LIST'
9ace579357cc2e976e4d2576fc1d519b6856495d98ccf11d1a67c5a9b4f79b8c gcc-10_10.3.0.orig.tar.gz
a50fba674000a2d688a619a2a8909b542afaeea0dbe08c3926cf4026109cc3b7 gcc-10_10.3.0-3+steamrt3.1.debian.tar.xz
1bc54e15345e4b2500ae43017c2f646c5122b812cbb061c1e4699b1339ed5d33 gcc-10_10.3.0-3+steamrt3.1.dsc
3dc7704b6166839c37d7047626fd199f3d4c09aca0d90e48c51c31c967dce34e glibc_2.31.orig.tar.xz
b173a5551de5c569738ee545beda2988769ed3f83c2b67def5cf862408bebe95 glibc_2.31-13+deb11u14.debian.tar.xz
f3cb6255cbf60297fae7dfdb6f8c6f72b3692b89429fe2536f15fad238e10ec3 glibc_2.31-13+deb11u14.dsc
e03766e4b2c246a20c9e2770aa599ee25c336d33b31c0980950230634e195339 linux_5.10.262.orig.tar.xz
117a1e100275833b98b14b0af5babf9f5996190c87fe489f00cc5ec9f953ce3a linux_5.10.262-1.debian.tar.xz
573d73cce15a3ac06ae030c2678cbb66217a42cf9afc5a036c611f1a205dd316 linux_5.10.262-1.dsc
LIST
# The AppImage runtime (installer/appimage/build_appimage.sh) and the libraries its release links statically.
while read -r sha file url; do
  if ! { [[ -f "$D/$file" ]] && echo "$sha  $D/$file" | sha256sum -c --quiet - 2>/dev/null; }; then
    curl -fsSL -o "$D/$file.part" "$url"
    echo "$sha  $D/$file.part" | sha256sum -c --quiet - || { rm -f "${D:?}/${file:?}.part"; echo "checksum mismatch: $file" >&2; exit 1; }
    mv "$D/$file.part" "$D/$file"
  fi
done <<LIST
4c4f6df4647c9f01f871d7edd3716d8aeeffda9d22ffbebe3fccd95a6ab52c95 type2-runtime-20251108.tar.gz https://github.com/AppImage/type2-runtime/archive/refs/tags/20251108.tar.gz
70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0 fuse-3.15.0.tar.xz https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz
db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3 squashfuse-0.5.2.tar.gz https://github.com/vasi/squashfuse/archive/0.5.2.tar.gz
LIST
GCC14="$TC/gcc-$(sed -n 's/^set(KUFCOD_LIBSTDCXX_VERSION \(.*\))$/\1/p' "$ROOT/cmake/steamrt3-sniper-version.cmake").tar.xz"
[[ -f "$GCC14" ]] || { echo "missing $GCC14 (tools/toolchain/setup_sniper.sh)" >&2; exit 1; }
TAR="$OUT/kuf-cod-recomp-$VERSION-toolchain-sources.tar"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$(git -C "$ROOT" log -1 --format=%ct)" \
  -cf "$TAR" -C "$TC" "$(basename "$GCC14")" -C "$D" gcc-10_10.3.0.orig.tar.gz gcc-10_10.3.0-3+steamrt3.1.debian.tar.xz \
  gcc-10_10.3.0-3+steamrt3.1.dsc glibc_2.31.orig.tar.xz glibc_2.31-13+deb11u14.debian.tar.xz \
  glibc_2.31-13+deb11u14.dsc linux_5.10.262.orig.tar.xz linux_5.10.262-1.debian.tar.xz linux_5.10.262-1.dsc \
  type2-runtime-20251108.tar.gz fuse-3.15.0.tar.xz squashfuse-0.5.2.tar.gz
echo "$TAR"
