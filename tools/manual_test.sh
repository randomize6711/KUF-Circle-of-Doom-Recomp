#!/usr/bin/env bash
# Start this build by hand, in a window, with its own data: docs/MANUAL_TEST_PC.md. Nothing outside build/manual-test is
# written: not the installed game, not the real saves or settings (those saves are only copied once, if there are any).
#
#   tools/manual_test.sh [game arguments]    (KUF_MANUAL_PRESET: another build tree under build/)
#   tools/manual_test.sh --make-launcher     writes "build/manual-test/KUF test build.desktop", to double-click
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRESET="${KUF_MANUAL_PRESET:-linux-amd64-relwithdebinfo}"
GAME="$ROOT/build/$PRESET/bin/kuf_cod"
DATA="$ROOT/build/manual-test"
GAME_DIR="${KUF_GAME_DIR:-$(cat "$ROOT/.game_dir" 2>/dev/null || true)}"
REAL_SAVES="${XDG_DATA_HOME:-$HOME/.local/share}/kuf-cod-recomp/data/saves"

[ -x "$GAME" ] || { echo "no build at $GAME" >&2; exit 1; }
[ -d "$GAME_DIR" ] || { echo "no game folder: set KUF_GAME_DIR, or write its path into $ROOT/.game_dir" >&2; exit 1; }
mkdir -p "$DATA/user" "$DATA/cache" "$DATA/logs"

if [ "${1:-}" = "--make-launcher" ]; then
  LAUNCHER="$DATA/KUF test build.desktop"
  printf '[Desktop Entry]\nType=Application\nName=KUF test build\nExec="%s"\nTerminal=false\n' "$ROOT/tools/manual_test.sh" > "$LAUNCHER"
  chmod +x "$LAUNCHER"
  echo "$LAUNCHER"
  exit 0
fi

if [ ! -d "$DATA/saves" ]; then
  if [ -d "$REAL_SAVES" ]; then cp -a "$REAL_SAVES" "$DATA/saves"; else mkdir -p "$DATA/saves"; fi
fi
exec "$GAME" --game_data_root="$GAME_DIR" --user_data_root="$DATA/user" --content_root="$DATA/saves" \
  --cache_root="$DATA/cache" --log_file="$DATA/logs/kuf_cod.log" --log_max_file_size_mb=64 --log_max_files=3 "$@"
