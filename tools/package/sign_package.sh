#!/usr/bin/env bash
# Signs a package for the updater: <package>.sig (ssh-keygen -Y sign, namespace kuf-cod-recomp-release).
#   tools/package/sign_package.sh --key PRIVATE_KEY PACKAGE.tar.gz
#   tools/package/sign_package.sh --new-key DIR        # makes an ed25519 release key pair in DIR (asks for a passphrase)
# Keep the private key off the build machine's repository and backed up: whoever holds it can publish updates that
# installed copies accept. The public key goes into each install's allowed_signers (install.sh --signers).
set -euo pipefail
NAMESPACE=kuf-cod-recomp-release
IDENTITY=release@kuf-cod-recomp
case "${1:-}" in
  --new-key)
    DIR="$2"; mkdir -p "$DIR"; chmod 700 "$DIR"
    ssh-keygen -t ed25519 -C "$IDENTITY" -f "$DIR/kuf-cod-release"
    echo "$IDENTITY namespaces=\"$NAMESPACE\" $(cut -d' ' -f1,2 "$DIR/kuf-cod-release.pub")" > "$DIR/allowed_signers"
    echo "public key for installs: $DIR/allowed_signers" ;;
  --key)
    KEY="$2"; PKG="$3"
    ssh-keygen -Y sign -f "$KEY" -n "$NAMESPACE" "$PKG"
    echo "$PKG.sig" ;;
  *) sed -n '2,6p' "$0"; exit 2 ;;
esac
