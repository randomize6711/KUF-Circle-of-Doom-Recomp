#!/usr/bin/env bash
# Verifies a release as players get it (docs/RELEASING.md "After publishing"): its assets downloaded from GitHub into
# a new folder, SHA256SUMS checked, then the full release audit (tools/package/audit_release.py: production key,
# signatures, one version everywhere, the AppImage's package identical to the update package file by file, the
# updater and the official repository, no private keys, game data or personal data). Reads GitHub only.
#
#   tools/package/verify_github_release.sh vX.Y.Z DIR [--repo owner/name]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TAG="${1:?tag}"; DIR="${2:?a new folder}"; REPO="randomize6711/KUF-Circle-of-Doom-Recomp"
[[ "${3:-}" == "--repo" ]] && REPO="$4"
[[ ! -e "$DIR" ]] || { echo "$DIR exists (use a new folder)" >&2; exit 1; }
mkdir -p "$DIR"
gh release view "$TAG" -R "$REPO" --json tagName,isPrerelease,isDraft,targetCommitish,url \
  --jq '"release \(.tagName): prerelease \(.isPrerelease), draft \(.isDraft), target \(.targetCommitish), \(.url)"'
gh release download "$TAG" -R "$REPO" -D "$DIR" >/dev/null
echo "downloaded: $(ls "$DIR" | tr '\n' ' ')"
(cd "$DIR" && sha256sum --quiet --ignore-missing -c SHA256SUMS) && echo "SHA256SUMS: every published asset listed matches"
gh release view "$TAG" -R "$REPO" --json body --jq .body > "$DIR/notes.txt"  # the release page's text (not an asset)
python3 -I "$ROOT/tools/package/audit_release.py" "$DIR" --report "$DIR/audit.txt" | grep -v '^  ' || true
grep -q '^AUDIT: clean$' "$DIR/audit.txt"
