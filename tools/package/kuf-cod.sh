#!/usr/bin/env bash
# Launcher of an installed KUF: Circle of Doom port (docs/STEAM_DECK.md). Steam's non-Steam-game shortcut points at
# the install's stable "kuf-cod" script, which runs this one from the current version.
#
#   kuf-cod [--game-dir DIR] [--collect-logs] [-- game options]
#
# The game folder (the player's own copy: the folder with default.xex) is asked for once and remembered in
# $XDG_CONFIG_HOME/kuf-cod-recomp/launcher.conf; the game only reads it. Saves, settings, logs and crash reports live
# in $XDG_DATA_HOME/kuf-cod-recomp/data. --collect-logs packs the latest logs, crash report and run summary into
# one archive for a bug report (no saves, no game files).
set -euo pipefail
HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/kuf-cod-recomp"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/kuf-cod-recomp/data"
CONF="$CONF_DIR/launcher.conf"
GAME_DIR="${KUF_GAME_DIR:-}"
COLLECT=0
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game-dir) GAME_DIR="$2"; SAVE_GAME_DIR=1; shift 2 ;;
    --collect-logs) COLLECT=1; shift ;;
    --) shift; ARGS+=("$@"); break ;;
    *) ARGS+=("$1"); shift ;;
  esac
done

# Dialogs (Game Mode has no terminal) only with a display and unless KUF_NO_DIALOG is set (tests, scripts).
DIALOGS=0
[[ -z "${KUF_NO_DIALOG:-}" && ( -n "${WAYLAND_DISPLAY:-}" || -n "${DISPLAY:-}" ) ]] && DIALOGS=1
message() {  # a dialog when there is no terminal, and always stderr
  if [[ $DIALOGS -eq 1 && ! -t 2 ]] && command -v zenity >/dev/null; then zenity --error --text="$1" 2>/dev/null || true
  elif [[ $DIALOGS -eq 1 && ! -t 2 ]] && command -v kdialog >/dev/null; then kdialog --error "$1" 2>/dev/null || true
  fi
  echo "kuf-cod: $1" >&2
}

if [[ $COLLECT -eq 1 ]]; then
  OUT="$HOME/kuf-cod-logs-$(date +%Y%m%d-%H%M%S).tar.gz"
  (cd "$DATA" && tar -czf "$OUT" --ignore-failed-read logs user/crash-report.txt user/run-summary.json \
      user/pc_settings.toml 2>/dev/null) || true
  echo "$OUT"
  exit 0
fi

if [[ -z "$GAME_DIR" && -f "$CONF" ]]; then
  GAME_DIR="$(sed -n 's/^GAME_DIR=//p' "$CONF" | tail -1)"
fi
if [[ -z "$GAME_DIR" && $DIALOGS -eq 1 ]]; then
  if command -v zenity >/dev/null; then
    GAME_DIR="$(zenity --file-selection --directory --title="Kingdom Under Fire: Circle of Doom - your game folder (with default.xex)" 2>/dev/null || true)"
  elif command -v kdialog >/dev/null; then
    GAME_DIR="$(kdialog --getexistingdirectory "$HOME" --title "Your game folder (with default.xex)" 2>/dev/null || true)"
  fi
  SAVE_GAME_DIR=1
fi
if [[ -z "$GAME_DIR" || ! -f "$GAME_DIR/default.xex" ]]; then
  message "No game folder: start once with --game-dir /path/to/your/game (the folder that contains default.xex)."
  exit 1
fi
if [[ "${SAVE_GAME_DIR:-0}" -eq 1 ]]; then
  mkdir -p "$CONF_DIR"
  printf 'GAME_DIR=%s\n' "$GAME_DIR" > "$CONF.tmp" && mv "$CONF.tmp" "$CONF"
fi
mkdir -p "$DATA/user" "$DATA/saves" "$DATA/cache" "$DATA/logs"
# Check for Updates in the title list (M7): the install's updater, when this runs from an install (versions/<v>).
if [[ "$(basename "$(dirname "$HERE")")" == "versions" && -x "$HERE/../../kuf-update" ]]; then
  export KUF_INSTALL_ROOT="$(cd "$HERE/../.." && pwd)" KUF_UPDATER="$(cd "$HERE/../.." && pwd)/kuf-update"
fi
# The GPU (the game's renderer): always on for play; it is off by default in the binary (the automated tests run
# without it). A --gpu_plugin given by the caller wins.
GPU_ARGS=(--gpu_plugin=xenos)
[[ " ${ARGS[*]} " == *" --gpu_plugin="* ]] && GPU_ARGS=()
GAME=("$HERE/bin/kuf_cod" --game_data_root="$GAME_DIR" --user_data_root="$DATA/user" --content_root="$DATA/saves"
  --cache_root="$DATA/cache" --log_file="$DATA/logs/kuf_cod.log" --log_max_file_size_mb=64 --log_max_files=3
  "${GPU_ARGS[@]}" "${ARGS[@]}")
if [[ -z "${KUF_INSTALL_ROOT:-}" || ! -x "$KUF_INSTALL_ROOT/kuf-cod" ]]; then
  exec "${GAME[@]}"  # not an install: no restart after an update
fi
# Restart after an update (M7 updater): the game writes this file and quits when the player chooses Restart; the
# install's stable launcher then starts the version now current (the update switched it), with the same options.
export KUF_RESTART_FILE="$DATA/user/.restart-requested"
rm -f "$KUF_RESTART_FILE"
status=0
"${GAME[@]}" || status=$?
if [[ -f "$KUF_RESTART_FILE" ]]; then
  rm -f "$KUF_RESTART_FILE"
  exec "$KUF_INSTALL_ROOT/kuf-cod" "${ARGS[@]}"
fi
exit "$status"
