#!/usr/bin/env bash
# Signs a release made by tools/package/make_release.sh (docs/RELEASING.md): the package and its update manifest,
# with the production key (ssh-keygen asks for its passphrase, twice); then SHA256SUMS and the audit again.
#
#   tools/package/sign_release.sh build/releases/vX.Y.Z --key ~/.local/share/kuf-cod-release-key/kuf-cod-release
#
# The key is only read by ssh-keygen; nothing about it is printed or written except the two .sig files.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST="$(realpath "${1:?release folder}")"
[[ "${2:-}" == "--key" && -f "${3:-}" ]] || { echo "usage: $0 RELEASE_DIR --key PRIVATE_KEY" >&2; exit 2; }
KEY="$(realpath "$3")"
case "$KEY" in "$ROOT"/*) echo "the private key must not be inside the repository" >&2; exit 2 ;; esac
PKG="$(ls "$DEST"/kuf-cod-recomp-*-linux-x86_64.tar.gz)"
MANIFEST="${PKG%.tar.gz}.update.json"
[[ -f "$MANIFEST" ]] || { echo "no update manifest in $DEST" >&2; exit 1; }
echo "signing $(basename "$PKG") and $(basename "$MANIFEST") (the key's passphrase is asked for each)"
"$ROOT/tools/package/sign_package.sh" --key "$KEY" "$PKG" >/dev/null
"$ROOT/tools/package/sign_package.sh" --key "$KEY" --manifest "$MANIFEST" >/dev/null
(cd "$DEST" && sha256sum -- $(ls | grep -v -e '^SHA256SUMS$' -e '^audit.txt$' -e '^publish.txt$') > SHA256SUMS)
python3 -I "$ROOT/tools/package/audit_release.py" "$DEST" --report "$DEST/audit.txt" | tail -3
