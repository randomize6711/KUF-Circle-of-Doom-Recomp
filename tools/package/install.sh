#!/usr/bin/env bash
# Installs this package (run from the unpacked package directory): docs/STEAM_DECK.md.
#   ./install.sh [--root DIR] [--repo owner/name] [--signers allowed_signers]
# Updates come from the project's GitHub releases by default (DEFAULT_REPO) and are verified against the release key
# shipped in the package (allowed_signers); --repo / --signers override them.
# Copies the package into <root>/versions/<version> (default root: $XDG_DATA_HOME/kuf-cod-recomp/app), checks its
# MANIFEST.sha256, points <root>/current at it and writes <root>/kuf-cod (the stable launcher for Steam shortcuts).
# --repo and --signers configure updates (tools/updater/kuf_update.py). The player's
# saves and settings ($XDG_DATA_HOME/kuf-cod-recomp/data) are not touched.
set -euo pipefail
PKG="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/kuf-cod-recomp/app"
DEFAULT_REPO="randomize6711/KUF-Circle-of-Doom-Recomp"
REPO=""
SIGNERS=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) ROOT="$2"; shift 2 ;;
    --repo) REPO="$2"; shift 2 ;;
    --signers) SIGNERS="$(realpath "$2")"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
VERSION="$(cat "$PKG/VERSION")"
[[ -z "$SIGNERS" && -f "$PKG/allowed_signers" ]] && SIGNERS="$PKG/allowed_signers"
(cd "$PKG" && sha256sum --quiet -c MANIFEST.sha256) || { echo "package files damaged (MANIFEST.sha256)" >&2; exit 1; }
mkdir -p "$ROOT/versions"
if [[ "$(readlink "$ROOT/current" 2>/dev/null || true)" == "versions/$VERSION" ]]; then
  echo "$VERSION is already installed and current"
else
  STAGE="$(mktemp -d "$ROOT/versions/.staging-XXXXXX")"
  trap 'rm -rf "$STAGE"' EXIT
  cp -a "$PKG/." "$STAGE/pkg"
  rm -rf "$ROOT/versions/$VERSION"
  mv "$STAGE/pkg" "$ROOT/versions/$VERSION"
  OLD="$(basename "$(readlink "$ROOT/current" 2>/dev/null || echo)")"
  ln -sfn "versions/$VERSION" "$ROOT/.current.$$" && mv -T "$ROOT/.current.$$" "$ROOT/current"
  [[ -n "$OLD" && "$OLD" != "$VERSION" ]] && echo "$OLD" > "$ROOT/previous"
fi
cat > "$ROOT/kuf-cod" <<LAUNCH
#!/usr/bin/env bash
exec "$ROOT/current/kuf-cod.sh" "\$@"
LAUNCH
cat > "$ROOT/kuf-update" <<UPDATE
#!/usr/bin/env bash
exec python3 -I "$ROOT/current/kuf_update.py" --install-root "$ROOT" "\$@"
UPDATE
chmod 0755 "$ROOT/kuf-cod" "$ROOT/kuf-update"
if [[ ! -f "$ROOT/update.conf" ]]; then
  printf '# Updates (kuf-update check / update): the release repository and the release key identity.\n' > "$ROOT/update.conf"
  printf 'repo = %s\nkey_identity = release@kuf-cod-recomp\n' "${REPO:-$DEFAULT_REPO}" >> "$ROOT/update.conf"
elif [[ -n "$REPO" ]]; then
  sed -i "s|^repo = .*|repo = $REPO|" "$ROOT/update.conf"
fi
[[ -n "$SIGNERS" ]] && install -m 0644 "$SIGNERS" "$ROOT/allowed_signers"
echo "installed $VERSION in $ROOT"
echo "start: $ROOT/kuf-cod   (first start: $ROOT/kuf-cod --game-dir /path/to/your/game)"
