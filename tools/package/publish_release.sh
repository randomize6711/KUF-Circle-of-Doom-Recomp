#!/usr/bin/env bash
# Publishes a signed, audited release to GitHub, then verifies it as players will see it (docs/RELEASING.md
# "Publishing"). Run only with the player's approval of exactly this release.
#
#   tools/package/publish_release.sh build/releases/vX.Y.Z --target <public commit> --confirm vX.Y.Z
#       [--repo owner/name]
#
# Refuses: --confirm not naming this release; an audit problem (tools/package/audit_release.py, signatures
# included); a tag or release vX.Y.Z already on GitHub (nothing is ever replaced: no --clobber, no deletes); a target
# commit GitHub does not have. Publishes the setup AppImage, the package, the update manifest, both signatures and
# SHA256SUMS with the notes (pre-release when the manifest's channel says so). Afterwards: the release's assets
# downloaded again and compared byte for byte, and the updater's own check run against GitHub with the release key.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO="randomize6711/KUF-Circle-of-Doom-Recomp"
DEST="" TARGET="" CONFIRM=""
DEST="$(realpath "${1:?release folder}")"; shift
while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) TARGET="$2"; shift 2 ;;
    --confirm) CONFIRM="$2"; shift 2 ;;
    --repo) REPO="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
PKG="$(basename "$(ls "$DEST"/kuf-cod-recomp-*-linux-x86_64.tar.gz)")"
VERSION="${PKG#kuf-cod-recomp-}"; VERSION="${VERSION%-linux-x86_64.tar.gz}"
TAG="v$VERSION"
[[ "$CONFIRM" == "$TAG" ]] || { echo "--confirm $TAG is required (the approved release)" >&2; exit 2; }
[[ "$TARGET" =~ ^[0-9a-f]{40}$ ]] || { echo "--target: the full commit hash on $REPO" >&2; exit 2; }
echo "== audit"
python3 -I "$ROOT/tools/package/audit_release.py" "$DEST" --report "$DEST/audit.txt" | tail -1
grep -q '^AUDIT: clean$' "$DEST/audit.txt" || { echo "the audit is not clean: $DEST/audit.txt" >&2; exit 1; }
[[ -f "$DEST/SOURCE" ]] || { echo "no SOURCE (make the release with tools/package/make_release.sh)" >&2; exit 1; }
grep -q '^built from commit ' "$DEST/audit.txt" || { echo "the audit did not tie the program to its commit" >&2; exit 1; }
[[ -f "$DEST/KingdomUnderFire-CircleOfDoom-Setup-$VERSION-x86_64.AppImage" ]] ||
  { echo "no setup AppImage: a release always carries the setup of the same version (--appimage)" >&2; exit 1; }
echo "== GitHub state"
gh api "repos/$REPO/commits/$TARGET" --jq .sha >/dev/null || { echo "$REPO has no commit $TARGET" >&2; exit 1; }
if gh api "repos/$REPO/git/ref/tags/$TAG" >/dev/null 2>&1; then echo "tag $TAG already exists on $REPO" >&2; exit 1; fi
if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then echo "release $TAG already exists on $REPO" >&2; exit 1; fi
CHANNEL="$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get("channel","stable"))' "$DEST/${PKG%.tar.gz}.update.json")"
FLAGS=(--latest)
[[ "$CHANNEL" == prerelease ]] && FLAGS=(--prerelease)
ASSETS=()
for f in "KingdomUnderFire-CircleOfDoom-Setup-$VERSION-x86_64.AppImage" "$PKG" "$PKG.sig" \
         "${PKG%.tar.gz}.update.json" "${PKG%.tar.gz}.update.json.sig" SHA256SUMS \
         "${PKG%-linux-x86_64.tar.gz}-source.tar.xz" "${PKG%-linux-x86_64.tar.gz}-toolchain-sources.tar"; do
  [[ -f "$DEST/$f" ]] && ASSETS+=("$DEST/$f")
done
echo "== publish $TAG to $REPO (${#ASSETS[@]} assets, $CHANNEL)"
gh release create "$TAG" -R "$REPO" --target "$TARGET" --title "Kingdom Under Fire: Circle of Doom Recomp $VERSION" \
  --notes-file "$DEST/notes.txt" "${FLAGS[@]}" "${ASSETS[@]}"
echo "== verify what was published"
CHECK="$(mktemp -d)"
trap 'rm -rf "$CHECK"' EXIT
gh release download "$TAG" -R "$REPO" -D "$CHECK/assets" >/dev/null
for f in "${ASSETS[@]}"; do
  cmp -s "$f" "$CHECK/assets/$(basename "$f")" || { echo "PUBLISHED ASSET DIFFERS: $(basename "$f")" >&2; exit 1; }
done
echo "all ${#ASSETS[@]} assets downloaded again: identical"
mkdir -p "$CHECK/root"
printf 'repo = %s\nkey_identity = release@kuf-cod-recomp\n' "$REPO" > "$CHECK/root/update.conf"
[[ "$CHANNEL" != prerelease ]] || echo "channel = prerelease" >> "$CHECK/root/update.conf"  # stable installs ignore it
cp "$ROOT/tools/package/allowed_signers" "$CHECK/root/allowed_signers"
python3 -I "$ROOT/tools/updater/kuf_update.py" --install-root "$CHECK/root" --json check | tee "$CHECK/check.json"
python3 -I -c 'import json,sys; r=json.load(open(sys.argv[1])); sys.exit(0 if r.get("latest")==sys.argv[2] else 1)' \
  "$CHECK/check.json" "$VERSION" || { echo "the updater does not see $VERSION on GitHub" >&2; exit 1; }
echo "== the release as players download it (tools/package/verify_github_release.sh)"
"$ROOT/tools/package/verify_github_release.sh" "$TAG" "$CHECK/as-published" --repo "$REPO"
echo "published and verified: https://github.com/$REPO/releases/tag/$TAG"
