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
#   tools/package/appimage_test.sh upgrade DIR SETUP.AppImage OLD_ROOT
#       an existing installation upgraded by a newer setup (Repair): OLD_ROOT (without its game/ copy) and the
#       player's data and launcher settings copied (only read) into DIR, the setup run over the copy, then: the new
#       version current, the old one kept, the release key added, update.conf, saves, settings kept, the updater
#       online (GitHub)
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
# Name resolution with the network: /etc/resolv.conf is often a link into /run (systemd-resolved), which the
# sandbox does not otherwise contain.
RESOLVER=()
[[ -d /run/systemd/resolve ]] && RESOLVER=(--ro-bind /run/systemd/resolve /run/systemd/resolve)

box() {  # box NET? cmd...: the sandbox (NET=net shares the network); BOX_GPU=1: the graphics card (the setup's check)
  local net=() gpu=()
  [[ "$1" == net ]] && net=(--share-net "${RESOLVER[@]}")
  [[ -n "${BOX_GPU:-}" && -d /dev/dri ]] && gpu=(--dev-bind /dev/dri /dev/dri --ro-bind /sys /sys)
  shift
  bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
    --ro-bind /etc /etc --proc /proc --dev /dev "${gpu[@]}" --tmpfs /tmp \
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
  BOX_GPU=1 box nonet "$DIR/setup/$(basename "$img")" "$@"  # the setup checks the graphics driver (M11)
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
  # The sandbox the game gets must reach GitHub: the installed updater's own check, inside the same sandbox.
  PRE="$(box net "$ROOTDIR/kuf-update" --json check || true)"
  echo "pre-launch check in the sandbox: $PRE" | cut -c1-200 | tee -a "$DIR/logs/launch.log"
  [[ "$PRE" == *'"exit": 0'* || "$PRE" == *'"exit": 3'* ]] || { echo "GitHub is not reachable from the sandbox: not launching" >&2; exit 1; }
  echo "$(date '+%F %T') launch: $(box nonet "$ROOTDIR/kuf-update" status | head -1)" >> "$DIR/logs/launch.log"
  # Open GitHub Page from inside the sandbox: an xdg-open stand-in (first in the game's PATH) records the URL; outside,
  # only the official releases page is handed to the desktop's real xdg-open (the game's code is as installed).
  mkdir -p "$DIR/shim"
  printf '#!/bin/sh\nprintf "%%s\\n" "$1" >> "%s/logs/open-url.requests"\n' "$DIR" > "$DIR/shim/xdg-open"
  chmod +x "$DIR/shim/xdg-open"
  : > "$DIR/logs/open-url.requests"
  ( tail -n0 -F "$DIR/logs/open-url.requests" 2>/dev/null | while IFS= read -r url; do
      if [[ "$url" =~ ^https://github\.com/randomize6711/KUF-Circle-of-Doom-Recomp/releases(/tag/v[0-9][0-9A-Za-z.-]*)?$ ]]; then
        echo "$(date '+%T') opening $url" >> "$DIR/logs/open-url.log"; xdg-open "$url" >/dev/null 2>&1 &
      else
        echo "$(date '+%T') refused $url" >> "$DIR/logs/open-url.log"
      fi
    done ) &
  RELAY=$!
  set +e
  timeout --kill-after=10 3h bwrap \
    --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
    --ro-bind /etc /etc --proc /proc --dev /dev --tmpfs /tmp \
    --dev-bind /dev/dri /dev/dri --ro-bind /sys /sys \
    --bind "$DIR" "$DIR" \
    --unshare-all --share-net "${RESOLVER[@]}" --die-with-parent --new-session --clearenv \
    --setenv HOME "$H" --setenv XDG_CONFIG_HOME "$H/.config" --setenv XDG_DATA_HOME "$H/.local/share" \
    --setenv XDG_RUNTIME_DIR /tmp --setenv PATH "$DIR/shim:/usr/bin" \
    --ro-bind "$SOCK" "/tmp/$WAYLAND_DISPLAY" --setenv WAYLAND_DISPLAY "$WAYLAND_DISPLAY" \
    --setenv SDL_VIDEO_DRIVER wayland "${PULSE[@]}" \
    "$ROOTDIR/kuf-cod" -- --fullscreen=false > "$DIR/logs/launcher.out" 2>&1
  status=$?
  set -e
  pkill -P "$RELAY" 2>/dev/null; kill "$RELAY" 2>/dev/null || true
  echo "$(date '+%F %T') exit $status" >> "$DIR/logs/launch.log"
  echo "exit $status; logs: $DIR/logs, $H/.local/share/kuf-cod-recomp/data/logs"
  ;;
upgrade)
  IMG="$(realpath "${3:?SETUP.AppImage}")"; OLD="$(realpath "${4:?OLD_ROOT}")"
  [[ -f "$OLD/.kuf-install" ]] || { echo "$OLD is not an installation of the setup" >&2; exit 1; }
  [[ "$(ls -A "$H")" == Desktop ]] || { echo "$DIR is not fresh (use a new folder)" >&2; exit 1; }
  snapshot_real > "$DIR/real.before"
  FAILS=0
  COPY="$H/Games/$(basename "$OLD")"  # the installation's own folder name (a player's custom place)
  mkdir -p "$COPY" "$H/.local/share/kuf-cod-recomp" "$H/.config"
  (cd "$OLD" && tar --exclude=./game -cf - .) | (cd "$COPY" && tar -xf -)
  [[ -d "$HOME/.local/share/kuf-cod-recomp/data" ]] && cp -a "$HOME/.local/share/kuf-cod-recomp/data" "$H/.local/share/kuf-cod-recomp/"
  [[ -d "$HOME/.config/kuf-cod-recomp" ]] && cp -a "$HOME/.config/kuf-cod-recomp" "$H/.config/"
  data_hash() { (cd "$H" && { find .local/share/kuf-cod-recomp .config/kuf-cod-recomp -type f -print0 2>/dev/null || true; } |
                 LC_ALL=C sort -z | xargs -0 -r sha256sum); }
  data_hash > "$DIR/data.before"
  OLDV="$(readlink "$COPY/current")"; OLDV="${OLDV#versions/}"
  cp "$COPY/update.conf" "$DIR/update.conf.before"
  echo "== the copy: $OLDV, release key: $([[ -s "$COPY/allowed_signers" ]] && echo yes || echo none)"
  V="$(basename "$IMG" | sed -E 's/^KingdomUnderFire-CircleOfDoom-Setup-(.*)-x86_64\.AppImage$/\1/')"
  echo "== upgrade with the $V setup (Repair, unattended, no network)"
  setup_run "$IMG" --install-to "$COPY" --game-dir /game > "$DIR/logs/upgrade.log" 2>&1 || true
  tail -3 "$DIR/logs/upgrade.log"
  check "repaired in place (the same folder)" 'grep -q "^repairing" "$DIR/logs/upgrade.log" && [[ -f "$COPY/.kuf-install" ]]'
  check "the new version current: $V" '[[ "$(readlink "$COPY/current")" == "versions/$V" ]]'
  check "the old version kept for rollback: $OLDV" '[[ -d "$COPY/versions/$OLDV" ]]'
  check "the production release key added" 'cmp -s "$COPY/allowed_signers" "$REPO/tools/package/allowed_signers"'
  check "update.conf kept" 'cmp -s "$COPY/update.conf" "$DIR/update.conf.before"'
  check "the launchers point at this folder" 'grep -qF "$COPY/current/kuf-cod.sh" "$COPY/kuf-cod" && grep -qF "$COPY/current/kuf_update.py" "$COPY/kuf-update"'
  check "the game files restored and verified (137)" '[[ "$(find "$COPY/game" -type f | wc -l)" -eq 137 ]]'
  data_hash > "$DIR/data.after"
  check "saves, settings, bindings and launcher settings unchanged" 'cmp -s "$DIR/data.before" "$DIR/data.after"'
  CHECK="$(box net "$COPY/kuf-update" --json check || true)"
  echo "online check: $(echo "$CHECK" | cut -c1-120)"
  check "the upgraded updater reaches GitHub (exit 0 or 3)" '[[ "$CHECK" == *"\"exit\": 0"* || "$CHECK" == *"\"exit\": 3"* ]]'
  snapshot_real > "$DIR/real.after"
  check "the real installation, data and settings unchanged" 'cmp -s "$DIR/real.before" "$DIR/real.after"'
  echo "UPGRADE TEST: $([[ $FAILS -eq 0 ]] && echo PASS || echo "FAIL ($FAILS)")"
  [[ $FAILS -eq 0 ]]
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
