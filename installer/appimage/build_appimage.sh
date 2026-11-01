#!/usr/bin/env bash
# Builds the setup and its AppImage (installer/README.md):
#   installer/appimage/build_appimage.sh [--skip-package | --package TARBALL] [--out DIR]
# 1. the setup (cmake -S installer -B build/installer), its unit tests;
# 2. its assets in build/installer/assets: theme, game-file list, the artwork rendered to PNG (rsvg-convert), the
#    X360 font (by Redge, "100% Free" at dafont.com; fetched once, SHA-256 checked, cached in build/tools);
# 3. the game package of the current build (tools/package/package_linux.sh --strip), or --package (a release's own
#    package: tools/package/make_release.sh --appimage), unpacked into the AppDir;
# 4. build/installer/AppDir and, with appimagetool (pinned, cached in build/tools; SHA-256 checked),
#    build/installer/KingdomUnderFire-CircleOfDoom-Setup-<version>-x86_64.AppImage.
# The AppImage contains the game package (code recompiled from the player's own executable: personal use, see
# docs/STEAM_DECK.md before sharing) and never any game data. Nothing is published.
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
# The X360 font by Redge (dafont.com/x360.font, "100% Free"): the zip and the font file inside it, both checked.
FONT_ZIP_SHA256=f3535bc298f082a47a33cc960888e14f2cda9ada4459cd08ace51ff4ce796542
FONT_SHA256=19b150a061ddd338a550c312b271072d985f5ca9b3be4e4827d128b2f8e78ece

fetch() {  # url dest sha256: downloaded only when missing or not matching
  local url="$1" dest="$2" sha="$3"
  if [[ -f "$dest" ]] && echo "$sha  $dest" | sha256sum -c --quiet - 2>/dev/null; then return 0; fi
  mkdir -p "$(dirname "$dest")"
  curl -fsSL -o "$dest.part" "$url"
  echo "$sha  $dest.part" | sha256sum -c --quiet - || { rm -f "$dest.part"; echo "checksum mismatch: $url" >&2; exit 1; }
  mv "$dest.part" "$dest"
}

echo "== setup"
cmake -S "$ROOT/installer" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ >/dev/null
ninja -C "$OUT" kuf-setup kuf_setup_tests >/dev/null
"$OUT/kuf_setup_tests" | grep -E "passed|failed"

echo "== assets"
A="$OUT/assets"
rm -rf "$A" && mkdir -p "$A/fonts"
cp "$ROOT/installer/assets/theme.css" "$ROOT/installer/assets/game_v1.tsv" "$A/"
rsvg-convert -w 256 -h 256 "$ROOT/installer/assets/emblem.svg" -o "$A/icon-256.png"
rsvg-convert -w 512 -h 512 "$ROOT/installer/assets/emblem.svg" -o "$A/icon-512.png"
rsvg-convert -w 840 -h 42 "$ROOT/installer/assets/divider.svg" -o "$A/divider.png"
FONT_DIR="$TOOLS/x360-font"
fetch "https://dl.dafont.com/dl/?f=x360" "$FONT_DIR/x360.zip" "$FONT_ZIP_SHA256"
python3 -I -c 'import sys, zipfile; open(sys.argv[2], "wb").write(zipfile.ZipFile(sys.argv[1]).read("X360.ttf"))' \
  "$FONT_DIR/x360.zip" "$A/fonts/X360.ttf"
echo "$FONT_SHA256  $A/fonts/X360.ttf" | sha256sum -c --quiet -
printf '%s\n' "X360 font by Redge, from https://www.dafont.com/x360.font (licence there: \"100% Free\")." \
  "The archive contains no licence file; SHA-256 of X360.ttf: $FONT_SHA256" > "$A/fonts/X360-SOURCE.txt"

echo "== game package"
PKG_OUT="$OUT/package"
if [[ -n "$PACKAGE" ]]; then
  rm -rf "$PKG_OUT" && mkdir -p "$PKG_OUT"
  tar -xzf "$PACKAGE" -C "$PKG_OUT"
elif [[ $SKIP_PACKAGE -eq 0 || ! -d "$PKG_OUT" ]]; then
  rm -rf "$PKG_OUT" && mkdir -p "$PKG_OUT"
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
cp -a "$A" "$APPDIR/usr/share/kuf-setup/assets"
cp -a "$PKG_DIR" "$APPDIR/usr/share/kuf-setup/payload/package"
# The release public key: the one the package carries (installs verify updates against it).
[[ -f "$PKG_DIR/allowed_signers" ]] && cp "$PKG_DIR/allowed_signers" "$APPDIR/usr/share/kuf-setup/payload/"
# The game's system requirements, from its binaries (the newest glibc and libstdc++ versions they need): the setup
# refuses to install on an older system (the game would not start).
GLIBC_NEED="$(objdump -T "$PKG_DIR"/bin/* 2>/dev/null | grep -o 'GLIBC_2\.[0-9]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
GLIBCXX_NEED="$(objdump -T "$PKG_DIR"/bin/* 2>/dev/null | grep -o 'GLIBCXX_3\.4\.[0-9]*' | sed 's/GLIBCXX_//' | sort -uV | tail -1)"
printf 'glibc %s\nglibcxx %s\n' "$GLIBC_NEED" "$GLIBCXX_NEED" > "$APPDIR/usr/share/kuf-setup/payload/requires.txt"
echo "game needs glibc $GLIBC_NEED, GLIBCXX $GLIBCXX_NEED"
install -m 0755 "$ROOT/installer/appimage/AppRun" "$APPDIR/AppRun"
cp "$ROOT/installer/appimage/kuf-setup.desktop" "$APPDIR/kuf-setup.desktop"
cp "$A/icon-256.png" "$APPDIR/kuf-setup.png"
cp "$A/icon-256.png" "$APPDIR/.DirIcon"
desktop-file-validate "$APPDIR/kuf-setup.desktop"

echo "== AppImage"
TOOL="$TOOLS/appimagetool-$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage"
fetch "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage" \
  "$TOOL" "$APPIMAGETOOL_SHA256"
chmod +x "$TOOL"
NAME="KingdomUnderFire-CircleOfDoom-Setup-$VERSION-x86_64.AppImage"
DEST="${APPIMAGE_OUT:-$OUT}"
mkdir -p "$DEST"
rm -f "$DEST/$NAME"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" --no-appstream "$APPDIR" "$DEST/$NAME" >"$OUT/appimagetool.log" 2>&1 ||
  { tail -20 "$OUT/appimagetool.log" >&2; exit 1; }
echo "$DEST/$NAME"
