#!/usr/bin/env bash
# Builds a relocatable Linux package of the current build (personal use; see docs/STEAM_DECK.md before sharing it:
# it contains code recompiled from the player's own copy of the game).
#
#   tools/package/package_linux.sh [--build DIR] [--out DIR] [--version V] [--strip] [--signers FILE]
#
# Output: <out>/kuf-cod-recomp-<version>-linux-x86_64.tar.gz with one top directory:
#   bin/ (kuf_cod and its libraries, RUNPATH $ORIGIN), kuf-cod.sh (launcher), install.sh, kuf_update.py,
#   VERSION, MANIFEST.sha256 (every other file), README-PACKAGE.txt, licenses/.
# No game data: the game folder is the player's, chosen at the first start. --strip removes debug symbols (smaller;
# crash reports then name fewer host frames). --signers embeds that release public key (allowed_signers format)
# instead of tools/package/allowed_signers (the throwaway key of a disposable update test). Sign it with
# tools/package/sign_package.sh; a whole release: tools/package/make_release.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build/linux-amd64-relwithdebinfo"
OUT="$ROOT/build/packages"
VERSION=""
STRIP=0
SIGNERS="$ROOT/tools/package/allowed_signers"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) BUILD="$(realpath "$2")"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --strip) STRIP=1; shift ;;
    --signers) SIGNERS="$(realpath "$2")"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
HEADER="$BUILD/generated_include/kuf_version_generated.h"
[[ -f "$BUILD/bin/kuf_cod" && -f "$HEADER" ]] || { echo "no build in $BUILD (build kuf_cod first)" >&2; exit 1; }
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
for f in kuf_cod librexruntimerd.so librexgpu-xenosrd.so libTracyClientrd.so; do
  [[ -f "$BUILD/bin/$f" ]] || { echo "missing $BUILD/bin/$f" >&2; exit 1; }
  install -m 0755 "$BUILD/bin/$f" "$P/bin/$f"
  [[ $STRIP -eq 1 ]] && strip --strip-debug "$P/bin/$f"
done
install -m 0755 "$ROOT/tools/package/kuf-cod.sh" "$P/kuf-cod.sh"
install -m 0755 "$ROOT/tools/package/install.sh" "$P/install.sh"
install -m 0755 "$ROOT/tools/updater/kuf_update.py" "$P/kuf_update.py"
install -m 0644 "$ROOT/tools/package/README-PACKAGE.txt" "$P/README-PACKAGE.txt"
# The release public key (updates are verified against it; tools/package/sign_package.sh --new-key makes the pair).
if [[ -f "$SIGNERS" ]]; then
  install -m 0644 "$SIGNERS" "$P/allowed_signers"
fi
install -m 0644 "$ROOT/thirdparty/rexglue-sdk/LICENSE" "$P/licenses/rexglue-sdk-LICENSE"
echo "$VERSION" > "$P/VERSION"
(cd "$P" && find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | LC_ALL=C sort | xargs -d '\n' sha256sum > MANIFEST.sha256)
TARBALL="$OUT/$NAME.tar.gz"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$(git -C "$ROOT" log -1 --format=%ct)" \
    -C "$STAGE" -czf "$TARBALL.partial" "$NAME"
mv "$TARBALL.partial" "$TARBALL"
echo "$TARBALL"
