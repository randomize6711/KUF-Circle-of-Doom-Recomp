#!/usr/bin/env bash
# Builds one release for the updater (docs/RELEASING.md): the package (no game code: docs/LOCAL_BUILD.md), its update
# manifest, the setup AppImage, the source archives, SHA256SUMS and the audit. Publishes nothing.
#
#   tools/package/make_release.sh --version V --summary FILE [--notes FILE] [--key PRIVATE_KEY] [--signers FILE]
#       [--min-installed V] [--channel stable|prerelease] [--appimage] [--out DIR] [--build DIR] [--strip]
#       [--allow-dirty]
#
# Steps: a clean work tree (unless --allow-dirty, test builds only) and no tracked game data; the portable game build
# (build/linux-amd64-portable: Steam Runtime 3 sysroot, glibc 2.31; docs/STEAM_DECK.md "Portable build") with
# KUFCOD_RELEASE_VERSION=V; tools/package/package_linux.sh (carrying the public key: --signers, else
# tools/package/allowed_signers); the update manifest (tools/package/make_manifest.py); with --appimage the setup
# AppImage around this very package (installer/appimage/build_appimage.sh --package). --summary: the in-game update
# summary (signed, in the manifest: what changed); --notes: the GitHub release page's text (installation
# instructions belong there; never shown in the game). With --key (a key without a
# passphrase: the disposable tests) the package and the manifest are signed here; without it the release is left for
# tools/package/sign_release.sh (which asks for the key's passphrase). Then SHA256SUMS, publish.txt and the audit
# (tools/package/audit_release.py: contents, secrets, personal data, signatures). Output in <out>/v<V>/ (default
# build/releases). The build tree is left at version V: build again without KUFCOD_RELEASE_VERSION afterwards.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build/linux-amd64-portable"
OUT="$ROOT/build/releases"
VERSION="" KEY="" SIGNERS="" NOTES="" SUMMARY="" MIN="" CHANNEL="" STRIP=() DIRTY=0 APPIMAGE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --key) KEY="$(realpath "$2")"; shift 2 ;;
    --signers) SIGNERS="$(realpath "$2")"; shift 2 ;;
    --notes) NOTES="$(realpath "$2")"; shift 2 ;;
    --summary) SUMMARY="$(realpath "$2")"; shift 2 ;;
    --min-installed) MIN="$2"; shift 2 ;;
    --channel) CHANNEL="$2"; shift 2 ;;
    --appimage) APPIMAGE=1; shift ;;
    --out) OUT="$2"; shift 2 ;;
    --build) BUILD="$(realpath "$2")"; shift 2 ;;
    --strip) STRIP=(--strip); shift ;;
    --allow-dirty) DIRTY=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || { echo "--version: a release version (1.2.3 or 1.2.3-rc.1)" >&2; exit 2; }
[[ -z "$KEY" || -f "$KEY" ]] || { echo "--key: no such file" >&2; exit 2; }
[[ -f "$SUMMARY" ]] || { echo "--summary FILE: the in-game update summary (what changed)" >&2; exit 2; }
# One authoritative version: the channel follows it (a pre-release version is never published as stable).
[[ -z "$CHANNEL" ]] && { [[ "$VERSION" == *-* ]] && CHANNEL=prerelease || CHANNEL=stable; }
[[ "$CHANNEL" == stable || "$CHANNEL" == prerelease ]] || { echo "--channel stable|prerelease" >&2; exit 2; }
[[ -z "$SIGNERS" ]] && SIGNERS="$ROOT/tools/package/allowed_signers"
[[ -f "$SIGNERS" ]] || { echo "no public key: --signers FILE or tools/package/allowed_signers" >&2; exit 2; }
if [[ -n "$KEY" ]]; then
  case "$KEY" in "$ROOT"/*) echo "the private key must not be inside the repository" >&2; exit 2 ;; esac
fi
if [[ $DIRTY -eq 0 && -n "$(git -C "$ROOT" status --porcelain --ignore-submodules=dirty)" ]]; then
  echo "the work tree has uncommitted changes (a release is built from a commit)" >&2; exit 1
fi
python3 -I "$ROOT/tools/check_tracked.py" >/dev/null || { echo "tools/check_tracked.py failed" >&2; exit 1; }

COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
echo "== build $VERSION from $COMMIT"
"$ROOT/tools/toolchain/setup_sniper.sh" >/dev/null  # the portable build's sysroot and libstdc++ (idempotent)
[[ -f "$BUILD/CMakeCache.txt" ]] || (cd "$ROOT" && cmake --preset linux-amd64-portable >/dev/null)
KUFCOD_RELEASE_VERSION="$VERSION" systemd-run --user --scope -q -p MemoryMax=14G \
  cmake --build "$BUILD" -j"${JOBS:-4}" >/dev/null
# The local builder the package carries (the setup's build tree: built as the setup, against the sysroot).
SETUP_BUILD="$ROOT/build/installer"
cmake -S "$ROOT/installer" -B "$SETUP_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/steamrt3-sniper.cmake" -DKUFCOD_STATIC_LIBSTDCXX=ON >/dev/null
ninja -C "$SETUP_BUILD" -j"${JOBS:-4}" kuf-localbuild >/dev/null
DEST="$OUT/v$VERSION"
rm -rf "$DEST"; mkdir -p "$DEST"
echo "== package"
PKG="$("$ROOT/tools/package/package_linux.sh" --build "$BUILD" --out "$DEST" --version "$VERSION" \
        --signers "$SIGNERS" "${STRIP[@]}" | tail -1)"
printf 'commit=%s\nversion=%s\nchannel=%s\n' "$COMMIT" "$VERSION" "$CHANNEL" > "$DEST/SOURCE"  # not an asset
MARGS=(--channel "$CHANNEL")
if [[ -n "$NOTES" ]]; then cp "$NOTES" "$DEST/notes.txt"; else echo "Kingdom Under Fire: Circle of Doom Recomp $VERSION" > "$DEST/notes.txt"; fi
cp "$SUMMARY" "$DEST/summary.txt"
MARGS+=(--summary "$DEST/summary.txt")
[[ -n "$MIN" ]] && MARGS+=(--min-installed "$MIN")
MANIFEST="$(python3 -I "$ROOT/tools/package/make_manifest.py" "$PKG" "${MARGS[@]}")"
if [[ $APPIMAGE -eq 1 ]]; then
  echo "== setup AppImage"
  "$ROOT/installer/appimage/build_appimage.sh" --package "$PKG" --out "$DEST" > "$DEST/.appimage.log" 2>&1 ||
    { tail -20 "$DEST/.appimage.log" >&2; exit 1; }
  rm -f "$DEST/.appimage.log"
fi
echo "== source archives"
"$ROOT/tools/package/make_source_archive.sh" --version "$VERSION" --out "$DEST" >/dev/null
cat > "$DEST/publish.txt" <<TXT
Release $VERSION ($CHANNEL). Publish only with the player's approval (docs/RELEASING.md "Publishing"):
  1. tools/package/sign_release.sh $DEST --key <private key>     (if not signed yet)
  2. tools/publish_public.sh --public-dir <public clone> --rev <commit> --push     (the source of this release)
  3. tools/package/publish_release.sh $DEST --target <the public commit> --confirm v$VERSION
TXT
if [[ -n "$KEY" ]]; then
  echo "== sign"
  "$ROOT/tools/package/sign_package.sh" --key "$KEY" "$PKG" >/dev/null
  "$ROOT/tools/package/sign_package.sh" --key "$KEY" --manifest "$MANIFEST" >/dev/null
fi
# SHA256SUMS lists exactly the assets that are published (not the notes or the working files).
(cd "$DEST" && sha256sum -- $(ls | grep -E '^(KingdomUnderFire-CircleOfDoom-Setup-.*\.AppImage|kuf-cod-recomp-.*\.(tar\.gz|update\.json)(\.sig)?|kuf-cod-recomp-.*-source\.tar\.xz|kuf-cod-recomp-.*-toolchain-sources\.tar)$') > SHA256SUMS)
echo "== audit"
AUDIT=(--signers "$SIGNERS" --report "$DEST/audit.txt")
[[ "$SIGNERS" != "$ROOT/tools/package/allowed_signers" ]] && AUDIT+=(--test-key)  # a disposable test release
[[ -z "$KEY" ]] && AUDIT+=(--unsigned)
python3 -I "$ROOT/tools/package/audit_release.py" "$DEST" "${AUDIT[@]}" | tail -3 || {
  echo "the audit found problems: $DEST/audit.txt" >&2; exit 1; }
echo "release assets in $DEST"
[[ -z "$KEY" ]] && echo "next: tools/package/sign_release.sh $DEST --key <private key>  (asks for its passphrase)"
exit 0
