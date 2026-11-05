#!/usr/bin/env bash
# Launcher of an installed KUF: Circle of Doom port (docs/STEAM_DECK.md). Steam's non-Steam-game shortcut points at
# the install's stable "kuf-cod" script, which runs this one from the current version.
#
#   kuf-cod [--game-dir DIR] [--collect-logs] [--licenses] [-- game options]
#
# The game folder (the player's own copy: the folder with default.xex) is asked for once and remembered in
# $XDG_CONFIG_HOME/kuf-cod-recomp/launcher.conf; the game only reads it. Saves, settings, logs and crash reports live
# in $XDG_DATA_HOME/kuf-cod-recomp/data. --collect-logs packs the latest logs, crash report and run summary into
# one archive for a bug report (no saves, no game files). --licenses prints the notices of the open-source software
# this version uses, and where their licence texts are.
set -euo pipefail
HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/kuf-cod-recomp"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/kuf-cod-recomp/data"
CONF="$CONF_DIR/launcher.conf"
GAME_DIR="${KUF_GAME_DIR:-}"
COLLECT=0
LICENSES=0
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game-dir) GAME_DIR="$2"; SAVE_GAME_DIR=1; shift 2 ;;
    --collect-logs) COLLECT=1; shift ;;
    --licenses) LICENSES=1; shift ;;
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

if [[ $LICENSES -eq 1 ]]; then
  cat "$HERE/licenses/THIRD-PARTY-NOTICES.txt"
  echo
  echo "Licence texts: $HERE/licenses"
  exit 0
fi

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
# The C++ library (docs/STEAM_DECK.md "Portable build"): the game is built against libstdc++ GLIBCXX_3.4.N (REQUIRES)
# and carries that library in bin/compat. The system's own is used when it is as new or newer (it then also serves
# the graphics driver, which may need the newer one); the bundled one only when the system's is older or missing
# (older distributions such as Debian 12 or Ubuntu 22.04). Not in $ORIGIN: there it would always win.
system_glibcxx() {
  local lib="" f
  lib="$( (/sbin/ldconfig -p || ldconfig -p) 2>/dev/null | awk '/libstdc\+\+\.so\.6 .*x86-64/ {print $NF; exit}')"
  if [[ -z "$lib" ]]; then
    for f in /usr/lib/x86_64-linux-gnu/libstdc++.so.6 /usr/lib64/libstdc++.so.6 /usr/lib/libstdc++.so.6; do
      [[ -f "$f" ]] && { lib="$f"; break; }
    done
  fi
  [[ -n "$lib" ]] && grep -aoE 'GLIBCXX_3\.4\.[0-9]+' "$lib" 2>/dev/null | sed 's/GLIBCXX_//' | sort -uV | tail -1
  return 0
}
CXX_CHOICE="the system's"
if [[ -f "$HERE/bin/compat/libstdc++.so.6" ]]; then
  NEED_CXX="$(sed -n 's/^glibcxx //p' "$HERE/REQUIRES" 2>/dev/null || true)"
  HAVE_CXX="$(system_glibcxx)"
  if [[ -z "$HAVE_CXX" || ( -n "$NEED_CXX" && "$(printf '%s\n%s\n' "$HAVE_CXX" "$NEED_CXX" | sort -V | head -1)" != "$NEED_CXX" ) ]]; then
    export LD_LIBRARY_PATH="$HERE/bin/compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    echo "kuf-cod: the system's libstdc++ (GLIBCXX ${HAVE_CXX:-none}) is older than the game's ($NEED_CXX): using the bundled one" >&2
    CXX_CHOICE="the bundled one (the system's is GLIBCXX ${HAVE_CXX:-none})"
  fi
fi
# A few lines per start in logs/launcher.log (kept short; --collect-logs packs it): enough to tell why a start failed
# on a machine nobody here can see (a Steam Deck in Game Mode has no terminal).
LAUNCH_LOG="$DATA/logs/launcher.log"
launch_log() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LAUNCH_LOG" 2>/dev/null || true; }
if [[ -f "$LAUNCH_LOG" && $(wc -l < "$LAUNCH_LOG") -gt 400 ]]; then
  tail -n 200 "$LAUNCH_LOG" > "$LAUNCH_LOG.tmp" && mv "$LAUNCH_LOG.tmp" "$LAUNCH_LOG"
fi
OS_NAME="$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '"' || true)"
BOARD="$(cat /sys/class/dmi/id/board_vendor /sys/class/dmi/id/board_name 2>/dev/null | tr '\n' ' ' || true)"
launch_log "start $(cat "$HERE/VERSION" 2>/dev/null || echo '?'): ${OS_NAME:-unknown OS}; board ${BOARD:-unknown};" \
  "glibc $(getconf GNU_LIBC_VERSION 2>/dev/null | sed 's/glibc //' || echo '?'); libstdc++: $CXX_CHOICE;" \
  "display ${WAYLAND_DISPLAY:+wayland }${DISPLAY:+x11 }${GAMESCOPE_WAYLAND_DISPLAY:+gamescope}; LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}"
# The GPU (the game's renderer): always on for play; it is off by default in the binary (the automated tests run
# without it). A --gpu_plugin given by the caller wins.
GPU_ARGS=(--gpu_plugin=xenos)
[[ " ${ARGS[*]} " == *" --gpu_plugin="* || " ${ARGS[*]} " == *" --gpu_plugin "* ]] && GPU_ARGS=()
GAME=("$HERE/bin/kuf_cod" --game_data_root="$GAME_DIR" --user_data_root="$DATA/user" --content_root="$DATA/saves"
  --cache_root="$DATA/cache" --log_file="$DATA/logs/kuf_cod.log" --log_max_file_size_mb=64 --log_max_files=3
  "${GPU_ARGS[@]}" "${ARGS[@]}")
if [[ -z "${KUF_INSTALL_ROOT:-}" || ! -x "$KUF_INSTALL_ROOT/kuf-cod" ]]; then
  launch_log "running (not an install)"
  exec "${GAME[@]}"  # not an install: no restart after an update
fi
# Restart after an update (M7 updater): the game writes this file and quits when the player chooses Restart; the
# install's stable launcher then starts the version now current (the update switched it), with the same options.
export KUF_RESTART_FILE="$DATA/user/.restart-requested"
rm -f "$KUF_RESTART_FILE"
# The first start after an update (the updater wrote .first-start naming this version): if the game cannot run or
# crashes within FIRST_START_SECONDS, switch back to the version before and start that one instead.
FIRST="$KUF_INSTALL_ROOT/.first-start"
THIS_VERSION="$(cat "$HERE/VERSION" 2>/dev/null || true)"
FIRST_START=0
[[ -f "$FIRST" && "$(head -n1 "$FIRST")" == "$THIS_VERSION" ]] && FIRST_START=1
started=$SECONDS
status=0
"${GAME[@]}" || status=$?
launch_log "exit $status after $((SECONDS - started)) s"
if [[ $FIRST_START -eq 1 ]]; then
  # 70: the game's crash report; 126 / 127: it could not be run; above 128: a signal (not INT / TERM: a deliberate
  # stop by the player is not a failed start).
  if [[ ! -f "$KUF_RESTART_FILE" && ( $status -eq 70 || ( $status -ge 126 && $status -ne 130 && $status -ne 143 ) ) \
        && $((SECONDS - started)) -lt ${FIRST_START_SECONDS:-60} ]]; then
    message "Version $THIS_VERSION did not start (exit $status); switching back to the version before."
    rm -f "$FIRST"
    if "$KUF_INSTALL_ROOT/kuf-update" rollback >&2; then
      exec "$KUF_INSTALL_ROOT/kuf-cod" "${ARGS[@]}"
    fi
    exit "$status"
  fi
  rm -f "$FIRST"
fi
if [[ -f "$KUF_RESTART_FILE" ]]; then
  rm -f "$KUF_RESTART_FILE"
  exec "$KUF_INSTALL_ROOT/kuf-cod" "${ARGS[@]}"
fi
exit "$status"
