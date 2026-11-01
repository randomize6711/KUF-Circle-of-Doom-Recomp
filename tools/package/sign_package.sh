#!/usr/bin/env bash
# Signs release assets for the updater (ssh-keygen -Y sign; docs/RELEASING.md):
#   tools/package/sign_package.sh --key PRIVATE_KEY PACKAGE.tar.gz            # -> PACKAGE.tar.gz.sig
#   tools/package/sign_package.sh --key PRIVATE_KEY --manifest X.update.json  # -> X.update.json.sig
#   tools/package/sign_package.sh --new-key DIR [--no-passphrase]   # an ed25519 release key pair in DIR
# A package is signed in the namespace kuf-cod-recomp-release, an update manifest in kuf-cod-recomp-manifest (a
# signature of one kind is never valid as the other). --no-passphrase is for throwaway test keys only.
# Keep a real private key out of every repository and backed up: whoever holds it can publish updates that installed
# copies accept. The public key (DIR/allowed_signers) goes into tools/package/allowed_signers when the player approves
# it; packages then carry it and installs verify against it.
set -euo pipefail
NAMESPACE=kuf-cod-recomp-release
MANIFEST_NAMESPACE=kuf-cod-recomp-manifest
IDENTITY=release@kuf-cod-recomp
case "${1:-}" in
  --new-key)
    DIR="$2"; PASS=()
    [[ "${3:-}" == "--no-passphrase" ]] && PASS=(-N "")
    [[ -e "$DIR/kuf-cod-release" ]] && { echo "$DIR/kuf-cod-release exists: not overwritten" >&2; exit 1; }
    REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    case "$(realpath -m "$DIR")/" in "$REPO"/*) echo "a key never goes inside the repository: $DIR" >&2; exit 1 ;; esac
    mkdir -p "$DIR"; chmod 700 "$DIR"
    ssh-keygen -q -t ed25519 -C "$IDENTITY" -f "$DIR/kuf-cod-release" "${PASS[@]}"
    chmod 600 "$DIR/kuf-cod-release"
    echo "$IDENTITY namespaces=\"$NAMESPACE,$MANIFEST_NAMESPACE\" $(cut -d' ' -f1,2 "$DIR/kuf-cod-release.pub")" \
      > "$DIR/allowed_signers"
    echo "public key for installs: $DIR/allowed_signers"
    echo "fingerprint: $(ssh-keygen -lf "$DIR/kuf-cod-release.pub")" ;;
  --key)
    KEY="$2"; NS="$NAMESPACE"; FILE="${3:-}"
    if [[ "$FILE" == "--manifest" ]]; then NS="$MANIFEST_NAMESPACE"; FILE="$4"; fi
    [[ -f "$FILE" ]] || { echo "no file $FILE" >&2; exit 2; }
    rm -f "$FILE.sig"
    ssh-keygen -q -Y sign -f "$KEY" -n "$NS" "$FILE"
    echo "$FILE.sig" ;;
  *) sed -n '2,6p' "$0"; exit 2 ;;
esac
