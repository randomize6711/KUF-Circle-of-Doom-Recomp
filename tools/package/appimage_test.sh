#!/usr/bin/env bash
# Disposable tests of the setup AppImage and of the installed game's GitHub updates (docs/RELEASING.md "AppImage
# tests"). Everything runs in bubblewrap with HOME = a test folder below build/, the game folder read-only and nothing
# else of the player's writable: the real installation, saves and settings are never seen.
#
#   tools/package/appimage_test.sh selftest DIR SETUP.AppImage [SIGNERS]   install, check, repair, uninstall
#   tools/package/appimage_test.sh install DIR SETUP.AppImage    install into DIR/home/Games/KUF (kept)
#   tools/package/appimage_test.sh launch DIR                    the installed game in a window, with the network (the
#                                                                player's manual GitHub update test; no input sent)
#   tools/package/appimage_test.sh report DIR                    installed versions, the game's updater lines
#
# The setup runs unattended (kuf-setup --install-to / --remove: the same checks and the same installation code as
# its window). selftest and install have no network; launch shares the network (GitHub).
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CMD="${1:-}"; DIR="${2:-}"
[[ -n "$CMD" && -n "$DIR" ]] || { sed -n '2,12p' "$0"; exit 2; }
mkdir -p "$DIR"; DIR="$(realpath "$DIR")"
case "$DIR" in "$REPO"/build/*) ;; *) echo "DIR must be below $REPO/build" >&2; exit 2 ;; esac
GAME_DIR="${KUF_GAME_DIR:-$(cat "$REPO/.game_dir" 2>/dev/null || true)}"
[[ -f "$GAME_DIR/default.xex" ]] || { echo "no game folder (KUF_GAME_DIR or .game_dir)" >&2; exit 1; }
H="$DIR/home"
ROOTDIR="$H/Games/KUF"
mkdir -p "$H/Desktop"  # as a player's home has (the setup only uses an existing desktop folder)
REAL_ROOT="$HOME/Games/Kingdom Under Fire - Circle of Doom"
mkdir -p "$H" "$DIR/logs"

box() {  # box NET? cmd...: the sandbox (NET=net shares the network)
  local net=()
  [[ "$1" == net ]] && net=(--share-net)
  shift
  bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
    --ro-bind /etc /etc --proc /proc --dev /dev --tmpfs /tmp \
    --ro-bind "$GAME_DIR" /game --bind "$DIR" "$DIR" \
    --unshare-all "${net[@]}" --die-with-parent --clearenv \
    --setenv HOME "$H" --setenv XDG_CONFIG_HOME "$H/.config" --setenv XDG_DATA_HOME "$H/.local/share" \
    --setenv XDG_DESKTOP_DIR "$H/Desktop" --setenv XDG_RUNTIME_DIR /tmp --setenv PATH /usr/bin \
    --setenv APPIMAGE_EXTRACT_AND_RUN 1 --setenv NO_CLEANUP 1 "$@"
}

snapshot_real() {
  for d in "$REAL_ROOT" "$HOME/.local/share/kuf-cod-recomp" "$HOME/.config/kuf-cod-recomp"; do
    [[ -e "$d" ]] || { echo "absent $d"; continue; }
    find "$d" -path "$REAL_ROOT/game" -prune -o -printf '%p %y %s %T@ %l\n' | LC_ALL=C sort
  done 2>/dev/null
}

setup_run() {  # setup_run APPIMAGE args...: the AppImage itself (a copy inside DIR), unattended, no network
  local img="$1"; shift
  mkdir -p "$DIR/setup"
  cp -f "$img" "$DIR/setup/"
  chmod +x "$DIR/setup/$(basename "$img")"
  box nonet "$DIR/setup/$(basename "$img")" "$@"
}

check() { if eval "$2"; then echo "  [ok] $1"; else echo "  [FAIL] $1"; FAILS=$((FAILS + 1)); fi; }

case "$CMD" in
selftest)
  IMG="$(realpath "${3:?SETUP.AppImage}")"
  SIGNERS="$(realpath "${4:-$REPO/tools/package/allowed_signers}")"  # the key the setup must install
  [[ "$(ls -A "$H")" == Desktop ]] || { echo "$DIR is not fresh (use a new folder)" >&2; exit 1; }
  snapshot_real > "$DIR/real.before"
  FAILS=0
  V="$(basename "$IMG" | sed -E 's/^KingdomUnderFire-CircleOfDoom-Setup-(.*)-x86_64\.AppImage$/\1/')"
  echo "== install $V (the AppImage, unattended, no network)"
  setup_run "$IMG" --install-to "$ROOTDIR" --game-dir /game --desktop > "$DIR/logs/install.log" 2>&1 || true
  tail -4 "$DIR/logs/install.log"
  check "installed into the chosen folder" '[[ -f "$ROOTDIR/.kuf-install" && -x "$ROOTDIR/kuf-cod" ]]'
  check "current -> versions/$V with the game program" '[[ "$(readlink "$ROOTDIR/current")" == "versions/$V" && -x "$ROOTDIR/current/bin/kuf_cod" ]]'
  check "the updater and its launcher script" '[[ -x "$ROOTDIR/kuf-update" && -f "$ROOTDIR/current/kuf_update.py" ]]'
  check "the release public key installed (the expected one)" 'cmp -s "$ROOTDIR/allowed_signers" "$SIGNERS"'
  check "update.conf: the official GitHub repository, no test endpoint" 'grep -q "^repo = randomize6711/KUF-Circle-of-Doom-Recomp$" "$ROOTDIR/update.conf" && ! grep -q "^api" "$ROOTDIR/update.conf"'
  check "the game files copied and verified (137)" '[[ "$(find "$ROOTDIR/game" -type f | wc -l)" -eq 137 ]]'
  check "the game folder is the installed copy (launcher)" 'grep -q "KUF_GAME_DIR=.*/KUF/game" "$ROOTDIR/kuf-cod"'
  check "application-menu shortcut" '[[ -f "$H/.local/share/applications/kuf-cod-recomp.desktop" ]] && grep -q "^Exec=.*KUF/kuf-cod" "$H/.local/share/applications/kuf-cod-recomp.desktop"'
  check "desktop shortcut" 'grep -q "^Exec=.*KUF/kuf-cod" "$H"/Desktop/*.desktop 2>/dev/null'
  check "nothing owned by root, nothing set-uid" '[[ -z "$(find "$ROOTDIR" \( -user root -o -perm -4000 \) -print -quit)" ]]'
  STATUS="$(box nonet "$ROOTDIR/kuf-update" --json status || true)"
  check "the installed updater runs (status: current $V)" '[[ "$STATUS" == *"\"current\": \"$V\""* ]]'
  CHECK="$(box nonet "$ROOTDIR/kuf-update" --json check || true)"
  check "without a network the update check fails cleanly (exit 1, a reason)" '[[ "$CHECK" == *"could not reach"* ]]'
  echo "== launcher finds its updater (KUF_UPDATER / KUF_INSTALL_ROOT, a stand-in for the game program)"
  printf '#!/bin/sh\nenv | grep -E "^KUF_(UPDATER|INSTALL_ROOT|RESTART_FILE)=" > "%s/probe.env"\n' "$DIR" > "$ROOTDIR/current/bin/kuf_cod.probe"
  chmod +x "$ROOTDIR/current/bin/kuf_cod.probe"
  mv "$ROOTDIR/current/bin/kuf_cod" "$DIR/kuf_cod.real"
  mv "$ROOTDIR/current/bin/kuf_cod.probe" "$ROOTDIR/current/bin/kuf_cod"
  box nonet "$ROOTDIR/kuf-cod" > "$DIR/logs/probe.log" 2>&1 || true
  mv "$DIR/kuf_cod.real" "$ROOTDIR/current/bin/kuf_cod"
  check "the launcher passes the updater and the restart file" 'grep -q "^KUF_UPDATER=$ROOTDIR/kuf-update$" "$DIR/probe.env" && grep -q "^KUF_RESTART_FILE=$H/.local/share/kuf-cod-recomp/data/user/" "$DIR/probe.env"'
  check "saves and settings go to the data folder" '[[ -d "$H/.local/share/kuf-cod-recomp/data/saves" && -d "$H/.local/share/kuf-cod-recomp/data/user" ]]'
  echo "== repair (the same AppImage over the installation)"
  echo progress > "$H/.local/share/kuf-cod-recomp/data/saves/marker"
  rm -f "$ROOTDIR/game/default.xex"
  setup_run "$IMG" --install-to "$ROOTDIR" --game-dir /game --desktop > "$DIR/logs/repair.log" 2>&1 || true
  check "repair restores a deleted game file" '[[ -f "$ROOTDIR/game/default.xex" ]] && grep -q "^repairing" "$DIR/logs/repair.log"'
  check "repair keeps saves" '[[ "$(cat "$H/.local/share/kuf-cod-recomp/data/saves/marker")" == progress ]]'
  echo "== uninstall"
  setup_run "$IMG" --remove "$ROOTDIR" > "$DIR/logs/remove.log" 2>&1 || true
  check "uninstall removes the installation and its shortcuts" '[[ ! -e "$ROOTDIR" && ! -f "$H/.local/share/applications/kuf-cod-recomp.desktop" ]]'
  check "uninstall keeps saves" '[[ "$(cat "$H/.local/share/kuf-cod-recomp/data/saves/marker")" == progress ]]'
  snapshot_real > "$DIR/real.after"
  check "the real installation, data and settings unchanged" 'cmp -s "$DIR/real.before" "$DIR/real.after"'
  echo "APPIMAGE SELFTEST: $([[ $FAILS -eq 0 ]] && echo PASS || echo "FAIL ($FAILS)")"
  [[ $FAILS -eq 0 ]]
  ;;
install)
  IMG="$(realpath "${3:?SETUP.AppImage}")"
  [[ ! -e "$ROOTDIR" ]] || { echo "$ROOTDIR exists" >&2; exit 1; }
  snapshot_real > "$DIR/real.before"
  setup_run "$IMG" --install-to "$ROOTDIR" --game-dir /game --desktop 2>&1 | tee "$DIR/logs/install.log" | tail -3
  SETTINGS="$(ls -t "$REPO"/build/runs/play-*/user/pc_settings.toml 2>/dev/null | head -1 || true)"
  mkdir -p "$H/.local/share/kuf-cod-recomp/data/user"
  [[ -n "$SETTINGS" ]] && cp "$SETTINGS" "$H/.local/share/kuf-cod-recomp/data/user/pc_settings.toml" &&
    echo "settings: a copy of $SETTINGS"
  box nonet "$ROOTDIR/kuf-update" status
  ;;
launch)
  [[ -x "$ROOTDIR/kuf-cod" ]] || { echo "install first" >&2; exit 1; }
  SOCK="${XDG_RUNTIME_DIR:-}/${WAYLAND_DISPLAY:-}"
  [[ -n "${WAYLAND_DISPLAY:-}" && -S "$SOCK" ]] || { echo "needs the Wayland session socket" >&2; exit 1; }
  PULSE=()
  [[ -S "${XDG_RUNTIME_DIR:-}/pulse/native" ]] && PULSE=(--ro-bind "$XDG_RUNTIME_DIR/pulse/native" /tmp/pulse/native
    --setenv PULSE_SERVER unix:/tmp/pulse/native --setenv SDL_AUDIO_DRIVER pulseaudio)
  echo "$(date '+%F %T') launch: $(box nonet "$ROOTDIR/kuf-update" status | head -1)" >> "$DIR/logs/launch.log"
  set +e
  timeout --kill-after=10 3h bwrap \
    --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
    --ro-bind /etc /etc --proc /proc --dev /dev --tmpfs /tmp \
    --dev-bind /dev/dri /dev/dri --ro-bind /sys /sys \
    --bind "$DIR" "$DIR" \
    --unshare-all --share-net --die-with-parent --new-session --clearenv \
    --setenv HOME "$H" --setenv XDG_CONFIG_HOME "$H/.config" --setenv XDG_DATA_HOME "$H/.local/share" \
    --setenv XDG_RUNTIME_DIR /tmp --setenv PATH /usr/bin \
    --ro-bind "$SOCK" "/tmp/$WAYLAND_DISPLAY" --setenv WAYLAND_DISPLAY "$WAYLAND_DISPLAY" \
    --setenv SDL_VIDEO_DRIVER wayland "${PULSE[@]}" \
    "$ROOTDIR/kuf-cod" -- --fullscreen=false > "$DIR/logs/launcher.out" 2>&1
  status=$?
  set -e
  echo "$(date '+%F %T') exit $status" >> "$DIR/logs/launch.log"
  echo "exit $status; logs: $DIR/logs, $H/.local/share/kuf-cod-recomp/data/logs"
  ;;
report)
  box nonet "$ROOTDIR/kuf-update" status || true
  cat "$DIR/logs/launch.log" 2>/dev/null || true
  grep -h -E '\[kuf\] version|\[kuf-title\] (update|installing|restarting)' \
    "$H/.local/share/kuf-cod-recomp/data/logs"/kuf_cod*.log 2>/dev/null | cut -c1-200 || true
  snapshot_real > "$DIR/real.after"
  [[ -f "$DIR/real.before" ]] && { cmp -s "$DIR/real.before" "$DIR/real.after" && echo "real installation: unchanged" ||
    echo "real installation: CHANGED"; }
  ;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
