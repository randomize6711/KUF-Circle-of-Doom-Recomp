#!/usr/bin/env bash
# Builds the setup and its AppImage (installer/README.md):
#   installer/appimage/build_appimage.sh [--skip-package | --package TARBALL] [--out DIR]
# 1. the setup (cmake -S installer -B build/installer), its unit tests;
# 2. its assets in build/installer/assets: theme, game-file list, the artwork rendered to PNG (rsvg-convert), the
#    Cinzel font (SIL Open Font License, with its licence; fetched once, SHA-256 checked, cached in build/tools);
# 3. the game package of the current build (tools/package/package_linux.sh --strip), or --package (a release's own
#    package: tools/package/make_release.sh --appimage), unpacked into the AppDir;
# 4. build/installer/AppDir and, with appimagetool (pinned, cached in build/tools; SHA-256 checked),
#    build/installer/KingdomUnderFire-CircleOfDoom-Setup-<version>-x86_64.AppImage.
# The AppImage contains the package (runtime, compiler and tools: no code of the game, docs/LOCAL_BUILD.md) and never any
# game data. Nothing is published.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/installer"
TOOLS="$ROOT/build/tools"
SKIP_PACKAGE=0
PACKAGE=""
APPIMAGE_OUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-package) SKIP_PACKAGE=1; shift ;;
    --package) PACKAGE="$(realpath "$2")"; shift 2 ;;
    --out) APPIMAGE_OUT="$(realpath -m "$2")"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
# The AppImage runtime: one release, not appimagetool's moving default (its source is in the toolchain-sources asset).
RUNTIME_VERSION=20251108
RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
# Cinzel, from the Google Fonts repository at a pinned commit: the font and its licence, both checked.
FONT_BASE=https://raw.githubusercontent.com/google/fonts/3dd78844021e948ceb633d1dcee3f7885561b5d9/ofl/cinzel
FONT_SHA256=f4d83d34d1f6c741193e4acf4b3dff9531e5a67b6aa65228d00a7db72a4e0f34
FONT_LICENSE_SHA256=f2b3029aba64c378bf0963b62945eee15e564fe4330b934c8f2eb058282b5e83

fetch() {  # url dest sha256: downloaded only when missing or not matching
  local url="$1" dest="$2" sha="$3"
  if [[ -f "$dest" ]] && echo "$sha  $dest" | sha256sum -c --quiet - 2>/dev/null; then return 0; fi
  mkdir -p "$(dirname "$dest")"
  curl -fsSL -o "$dest.part" "$url"
  echo "$sha  $dest.part" | sha256sum -c --quiet - || { rm -f "$dest.part"; echo "checksum mismatch: $url" >&2; exit 1; }
  mv "$dest.part" "$dest"
}

echo "== setup"
# Built as the game: against the Steam Runtime 3 sysroot (glibc 2.31; cmake/steamrt3-sniper.cmake), its C++ library
# linked in, so the AppImage starts on SteamOS and any Linux with glibc 2.31 or newer.
"$ROOT/tools/toolchain/setup_sniper.sh" >/dev/null
if [[ -f "$OUT/CMakeCache.txt" ]] && ! grep -q '^KUFCOD_SNIPER_SYSROOT:' "$OUT/CMakeCache.txt"; then
  rm -rf "$OUT/CMakeCache.txt" "$OUT/CMakeFiles"  # configured for this machine before (a toolchain applies only fresh)
fi
cmake -S "$ROOT/installer" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/steamrt3-sniper.cmake" -DKUFCOD_STATIC_LIBSTDCXX=ON >/dev/null
ninja -C "$OUT" kuf-setup kuf_setup_tests >/dev/null
"$OUT/kuf_setup_tests" | grep -E "passed|failed"

echo "== assets"
A="$OUT/assets"
rm -rf "$A" && mkdir -p "$A/fonts"
cp "$ROOT/installer/assets/theme.css" "$ROOT/installer/assets/game_v1.tsv" "$A/"
rsvg-convert -w 256 -h 256 "$ROOT/installer/assets/emblem.svg" -o "$A/icon-256.png"
rsvg-convert -w 512 -h 512 "$ROOT/installer/assets/emblem.svg" -o "$A/icon-512.png"
rsvg-convert -w 840 -h 42 "$ROOT/installer/assets/divider.svg" -o "$A/divider.png"
FONT_DIR="$TOOLS/cinzel"
fetch "$FONT_BASE/Cinzel%5Bwght%5D.ttf" "$FONT_DIR/Cinzel.ttf" "$FONT_SHA256"
fetch "$FONT_BASE/OFL.txt" "$FONT_DIR/OFL.txt" "$FONT_LICENSE_SHA256"
cp "$FONT_DIR/Cinzel.ttf" "$A/fonts/Cinzel.ttf"
cp "$FONT_DIR/OFL.txt" "$A/fonts/Cinzel-OFL.txt"

echo "== game package"
PKG_OUT="$OUT/package"
if [[ -n "$PACKAGE" ]]; then
  rm -rf "$PKG_OUT" && mkdir -p "$PKG_OUT"
  tar -xzf "$PACKAGE" -C "$PKG_OUT"
elif [[ $SKIP_PACKAGE -eq 0 || ! -d "$PKG_OUT" ]]; then
  rm -rf "$PKG_OUT" && mkdir -p "$PKG_OUT"
  ninja -C "$OUT" kuf-localbuild >/dev/null
  TARBALL="$("$ROOT/tools/package/package_linux.sh" --out "$PKG_OUT" --strip | tail -1)"
  tar -xzf "$TARBALL" -C "$PKG_OUT"
  rm -f "$TARBALL"
fi
PKG_DIR="$(find "$PKG_OUT" -mindepth 1 -maxdepth 1 -type d -name 'kuf-cod-recomp-*' | head -1)"
VERSION="$(cat "$PKG_DIR/VERSION")"
(cd "$PKG_DIR" && sha256sum --quiet -c MANIFEST.sha256)

echo "== AppDir"
APPDIR="$OUT/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/kuf-setup/payload"
install -m 0755 "$OUT/kuf-setup" "$APPDIR/usr/bin/kuf-setup"
strip --strip-debug "$APPDIR/usr/bin/kuf-setup"
SETUP_GLIBC="$(objdump -T "$APPDIR/usr/bin/kuf-setup" | grep -oE 'GLIBC_2\.[0-9]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
BASELINE="$(sed -n 's/^set(KUFCOD_GLIBC_BASELINE \(.*\))$/\1/p' "$ROOT/cmake/steamrt3-sniper-version.cmake")"
[[ "$(printf '%s\n%s\n' "$SETUP_GLIBC" "$BASELINE" | sort -V | tail -1)" == "$BASELINE" ]] ||
  { echo "the setup needs glibc $SETUP_GLIBC (baseline $BASELINE)" >&2; exit 1; }
if objdump -p "$APPDIR/usr/bin/kuf-setup" | grep -q 'NEEDED.*libstdc++'; then
  echo "the setup links libstdc++ dynamically (it must carry it: KUFCOD_STATIC_LIBSTDCXX)" >&2; exit 1
fi
cp -a "$A" "$APPDIR/usr/share/kuf-setup/assets"
cp -a "$PKG_DIR" "$APPDIR/usr/share/kuf-setup/payload/package"
# The release public key: the one the package carries (installs verify updates against it).
[[ -f "$PKG_DIR/allowed_signers" ]] && cp "$PKG_DIR/allowed_signers" "$APPDIR/usr/share/kuf-setup/payload/"
# The game's system requirements (the package's REQUIRES, from its binaries: tools/package/package_linux.sh): the
# setup checks them before it offers an installation.
[[ -f "$PKG_DIR/REQUIRES" ]] || { echo "the package has no REQUIRES (made by an older package_linux.sh)" >&2; exit 1; }
cp "$PKG_DIR/REQUIRES" "$APPDIR/usr/share/kuf-setup/payload/requires.txt"
echo "game needs: $(tr '\n' ' ' < "$PKG_DIR/REQUIRES")"
install -m 0755 "$ROOT/installer/appimage/AppRun" "$APPDIR/AppRun"
cp "$ROOT/installer/appimage/kuf-setup.desktop" "$APPDIR/kuf-setup.desktop"
cp "$A/icon-256.png" "$APPDIR/kuf-setup.png"
cp "$A/icon-256.png" "$APPDIR/.DirIcon"
desktop-file-validate "$APPDIR/kuf-setup.desktop"

echo "== AppImage"
TOOL="$TOOLS/appimagetool-$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage"
RUNTIME="$TOOLS/type2-runtime-$RUNTIME_VERSION/runtime-x86_64"
fetch "https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-x86_64" "$RUNTIME" "$RUNTIME_SHA256"
fetch "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage" \
  "$TOOL" "$APPIMAGETOOL_SHA256"
chmod +x "$TOOL"
NAME="KingdomUnderFire-CircleOfDoom-Setup-$VERSION-x86_64.AppImage"
DEST="${APPIMAGE_OUT:-$OUT}"
mkdir -p "$DEST"
rm -f "$DEST/$NAME"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" --no-appstream --runtime-file "$RUNTIME" "$APPDIR" "$DEST/$NAME" >"$OUT/appimagetool.log" 2>&1 ||
  { tail -20 "$OUT/appimagetool.log" >&2; exit 1; }
echo "$DEST/$NAME"
