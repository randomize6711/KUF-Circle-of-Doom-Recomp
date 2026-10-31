#!/usr/bin/env bash
# Verify that the original game folder is unchanged: every file in the manifest has the recorded SHA-256,
# and there are no files the manifest does not list. Only reads the folder.
#
#   tools/verify_game_dir.sh [version]      (default v1; KUF_GAME_DIR overrides the game folder)
#   tools/verify_game_dir.sh --record [ver] writes the manifest (only if none exists yet)
#
# Manifest: build/input/<ver>/game_dir.sha256 (sha256sum format, paths relative to the game folder).
# Exit 0 = unchanged; anything else = changed or unreadable.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
record=0
if [ "${1:-}" = "--record" ]; then record=1; shift; fi
VER="${1:-v1}"
GAME_DIR="${KUF_GAME_DIR:-$(cat "$ROOT/.game_dir" 2>/dev/null || true)}"  # your game folder (.game_dir: not committed)
[ -n "$GAME_DIR" ] || { echo "set KUF_GAME_DIR, or write your game folder's path into $ROOT/.game_dir" >&2; exit 1; }
MANIFEST="$ROOT/build/input/$VER/game_dir.sha256"

listing() { (cd "$GAME_DIR" && find . -type f -print | LC_ALL=C sort); }

if [ "$record" = 1 ]; then
  [ ! -e "$MANIFEST" ] || { echo "manifest exists, not overwriting: $MANIFEST" >&2; exit 1; }
  mkdir -p "$(dirname "$MANIFEST")"
  (cd "$GAME_DIR" && listing | while IFS= read -r f; do sha256sum "$f"; done) > "$MANIFEST.tmp"
  mv "$MANIFEST.tmp" "$MANIFEST"
  echo "recorded $(wc -l < "$MANIFEST") files in $MANIFEST"
  exit 0
fi

[ -f "$MANIFEST" ] || { echo "no manifest: $MANIFEST (record one with --record)" >&2; exit 1; }
want_files="$(cut -c67- "$MANIFEST" | LC_ALL=C sort)"
got_files="$(listing)"
if [ "$want_files" != "$got_files" ]; then
  echo "game folder file list differs from the manifest:" >&2
  diff <(echo "$want_files") <(echo "$got_files") >&2 || true
  exit 1
fi
(cd "$GAME_DIR" && sha256sum --quiet -c "$MANIFEST") || { echo "game folder content changed" >&2; exit 1; }
echo "game folder unchanged: $(echo "$got_files" | wc -l) files match $MANIFEST"
