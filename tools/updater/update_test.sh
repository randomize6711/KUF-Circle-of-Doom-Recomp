#!/usr/bin/env bash
# A disposable, end-to-end test of the GitHub updater with real builds (docs/RELEASING.md "Disposable update test").
#
#   tools/updater/update_test.sh prepare DIR [--old 0.7.0] [--new 0.7.1]   # builds, installs, checks (no game run)
#   tools/updater/update_test.sh launch DIR                                 # the old version in a window (the player)
#   tools/updater/update_test.sh report DIR                                 # what happened; the real install unchanged
#
# prepare: a throwaway release key (made outside the repository, no passphrase; its private half deleted once both
#   releases are signed, the public half kept in DIR/key); the current commit built
#   and released twice with tools/package/make_release.sh (--old and --new: the same code, each a full build stamped
#   with its version, packaged, signed, with a signed update manifest); --old installed with its own install.sh into
#   DIR/root (update.conf: repo test/kuf-cod-update-test, api http://127.0.0.1:PORT; the throwaway public key from the
#   package); --new published into DIR/releases for tools/updater/release_server.py; a snapshot of the player's real
#   installation; then the installed updater's own `check` against the server on the host's loopback (signatures,
#   manifest, endpoint) and `status`. The build tree is built again as the development version at the end.
# launch: bubblewrap with no network (a private network namespace: only its own loopback), the game folder read-only,
#   only DIR writable, HOME = DIR/home (settings, saves, logs: disposable; a copy of the player's last sandboxed
#   playtest settings when present), the Wayland and PulseAudio sockets, the GPU. Inside: the release server on
#   127.0.0.1:PORT (throttled so the download progress can be seen), then the installed launcher DIR/root/kuf-cod
#   in a window (--fullscreen=false). Nothing is typed or clicked for the player. Restarts after the update stay
#   inside. Ends when the game is quit (or after 3 hours).
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT=47371
TEST_REPO=test/kuf-cod-update-test
CMD="${1:-}"; DIR="${2:-}"
[[ -n "$CMD" && -n "$DIR" ]] || { sed -n '2,6p' "$0"; exit 2; }
shift 2
mkdir -p "$DIR"
DIR="$(realpath "$DIR")"
case "$DIR" in "$REPO"/build/*) ;; *) echo "DIR must be below $REPO/build (never tracked)" >&2; exit 2 ;; esac
GAME_DIR="${KUF_GAME_DIR:-$(cat "$REPO/.game_dir" 2>/dev/null || true)}"
[[ -f "$GAME_DIR/default.xex" ]] || { echo "no game folder (KUF_GAME_DIR or .game_dir)" >&2; exit 1; }
REAL_ROOT="$HOME/Games/Kingdom Under Fire - Circle of Doom"

snapshot_real() {  # the player's installation, data and launcher settings: names, sizes, times, small file hashes
  {
    for d in "$REAL_ROOT" "$HOME/.local/share/kuf-cod-recomp" "$HOME/.config/kuf-cod-recomp"; do
      [[ -e "$d" ]] || { echo "absent $d"; continue; }
      find "$d" -path "$REAL_ROOT/game" -prune -o -printf '%p %y %s %T@ %l\n' | LC_ALL=C sort
      find "$d" -path "$REAL_ROOT/game" -prune -o -type f -size -1M -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum
    done
    [[ -d "$REAL_ROOT/game" ]] && find "$REAL_ROOT/game" -printf '%p %s %T@\n' | LC_ALL=C sort | sha256sum
  } 2>/dev/null
}

case "$CMD" in
prepare)
  OLD=0.7.0 NEW=0.7.1
  while [[ $# -gt 0 ]]; do
    case "$1" in --old) OLD="$2"; shift 2 ;; --new) NEW="$2"; shift 2 ;; *) echo "unknown $1" >&2; exit 2 ;; esac
  done
  [[ -z "$(ls -A "$DIR")" ]] || { echo "$DIR is not empty (use a new directory)" >&2; exit 1; }
  snapshot_real > "$DIR/real-install.before"
  mkdir -p "$DIR/logs" "$DIR/releases" "$DIR/home"
  echo "== throwaway release key (outside the repository; the private half deleted after signing)"
  KEYDIR="$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/kuf-update-test-key.XXXXXX")"
  trap 'rm -rf "$KEYDIR"' EXIT
  rmdir "$KEYDIR"
  "$REPO/tools/package/sign_package.sh" --new-key "$KEYDIR" --no-passphrase >/dev/null
  mkdir -p "$DIR/key"
  cp "$KEYDIR/allowed_signers" "$KEYDIR/kuf-cod-release.pub" "$DIR/key/"
  for v in "$OLD" "$NEW"; do
    printf 'DISPOSABLE UPDATER TEST BUILD %s\nSigned with a throwaway test key; served from a local test server only.\n' \
      "$v" > "$DIR/notes-$v.txt"
    echo "== release $v (build, package, sign, manifest)"
    "$REPO/tools/package/make_release.sh" --version "$v" --key "$KEYDIR/kuf-cod-release" \
      --signers "$DIR/key/allowed_signers" --notes "$DIR/notes-$v.txt" --min-installed "$OLD" --strip \
      --out "$DIR/built" > "$DIR/logs/release-$v.log" 2>&1 || { tail -20 "$DIR/logs/release-$v.log"; exit 1; }
    grep -E 'signature ok|AUDIT' "$DIR/logs/release-$v.log" || true
  done
  rm -rf "$KEYDIR"; trap - EXIT
  echo "throwaway private key deleted; public key in $DIR/key"
  echo "== the build tree back to the development version"
  systemd-run --user --scope -q -p MemoryMax=14G cmake --build "$REPO/build/linux-amd64-relwithdebinfo" -j8 >/dev/null
  echo "== install $OLD into $DIR/root"
  mkdir -p "$DIR/unpack"
  tar -xzf "$DIR/built/v$OLD/kuf-cod-recomp-$OLD-linux-x86_64.tar.gz" -C "$DIR/unpack"
  "$DIR/unpack/kuf-cod-recomp-$OLD-linux-x86_64/install.sh" --root "$DIR/root" --repo "$TEST_REPO" \
    --api "http://127.0.0.1:$PORT" > "$DIR/logs/install.log"
  rm -rf "$DIR/unpack"
  echo "== publish $NEW to the test server's directory"
  cp "$DIR/built/v$NEW/"kuf-cod-recomp-"$NEW"-linux-x86_64.{tar.gz,tar.gz.sig,update.json,update.json.sig} \
     "$DIR/releases/"
  python3 -I - "$DIR/releases" "$NEW" <<'PY'
import json, os, sys
d, v = sys.argv[1:3]
base = f"kuf-cod-recomp-{v}-linux-x86_64"
json.dump([{"tag_name": f"v{v}", "name": f"{v} (disposable updater test)", "draft": False, "prerelease": False,
            "published_at": "2026-11-01T00:00:00Z",
            "assets": [base + s for s in (".tar.gz", ".tar.gz.sig", ".update.json", ".update.json.sig")]}],
          open(os.path.join(d, "releases.json"), "w"), indent=1)
PY
  cp "$REPO/tools/updater/release_server.py" "$DIR/release_server.py"
  SETTINGS="$(ls -t "$REPO"/build/runs/play-*/user/pc_settings.toml 2>/dev/null | head -1 || true)"
  DATA="$DIR/home/.local/share/kuf-cod-recomp/data"
  mkdir -p "$DATA/user"
  if [[ -n "$SETTINGS" ]]; then cp "$SETTINGS" "$DATA/user/pc_settings.toml"; echo "settings: a copy of $SETTINGS"; fi
  echo "== checks"
  python3 -I "$DIR/release_server.py" --dir "$DIR/releases" --repo "$TEST_REPO" --port "$PORT" \
    --port-file "$DIR/logs/preflight.port" --log "$DIR/logs/preflight-server.log" &
  SERVER=$!
  trap 'kill $SERVER 2>/dev/null || true' EXIT
  for _ in $(seq 50); do [[ -f "$DIR/logs/preflight.port" ]] && break; sleep 0.1; done
  [[ -f "$DIR/logs/preflight.port" ]] || { echo "the test server did not start (port $PORT in use?)" >&2; exit 1; }
  "$DIR/root/kuf-update" status
  CHECK="$("$DIR/root/kuf-update" --json check || true)"
  echo "check: $CHECK"
  kill $SERVER; wait $SERVER 2>/dev/null || true; trap - EXIT
  python3 -I -c "import json,sys; r=json.loads(sys.argv[1]); sys.exit(0 if r.get('update') and r.get('latest')==sys.argv[2] and r.get('current')==sys.argv[3] else 1)" \
    "$CHECK" "$NEW" "$OLD" || { echo "the installed $OLD does not see $NEW: not ready" >&2; exit 1; }
  for v in "$OLD" "$NEW"; do
    b="$DIR/built/v$v/kuf-cod-recomp-$v-linux-x86_64.tar.gz"
    echo "$v: $(du -h "$b" | cut -f1) package, kuf_cod $(tar -xzOf "$b" "kuf-cod-recomp-$v-linux-x86_64/bin/kuf_cod" | sha256sum | cut -c1-16), stamped $(tar -xzOf "$b" "kuf-cod-recomp-$v-linux-x86_64/bin/kuf_cod" "kuf-cod-recomp-$v-linux-x86_64/bin/librexruntimerd.so" | grep -a -c -F "$v" || true) times"
  done
  snapshot_real > "$DIR/real-install.after-prepare"
  cmp -s "$DIR/real-install.before" "$DIR/real-install.after-prepare" && echo "real installation: unchanged"
  echo "ready: $DIR (installed $OLD, server offers $NEW)"
  ;;
launch)
  [[ -x "$DIR/root/kuf-cod" && -f "$DIR/releases/releases.json" ]] || { echo "prepare $DIR first" >&2; exit 1; }
  SOCK="${XDG_RUNTIME_DIR:-}/${WAYLAND_DISPLAY:-}"
  [[ -n "${WAYLAND_DISPLAY:-}" && -S "$SOCK" ]] || { echo "needs the Wayland session socket" >&2; exit 1; }
  PULSE=()
  [[ -S "${XDG_RUNTIME_DIR:-}/pulse/native" ]] && PULSE=(--ro-bind "$XDG_RUNTIME_DIR/pulse/native" /tmp/pulse/native
    --setenv PULSE_SERVER unix:/tmp/pulse/native --setenv SDL_AUDIO_DRIVER pulseaudio)
  printf 'autospawn = no\n' > "$DIR/home/pulse-client.conf"
  cat > "$DIR/inside.sh" <<INSIDE
#!/bin/sh
# Runs inside the sandbox: the loopback release server, then the installed launcher (restarts stay inside).
python3 -I "$DIR/release_server.py" --dir "$DIR/releases" --repo "$TEST_REPO" --port $PORT --rate 4096 \\
  --port-file "$DIR/logs/server.port" --log "$DIR/logs/server.log" &
SERVER=\$!
trap 'kill \$SERVER 2>/dev/null' EXIT
i=0; while [ ! -f "$DIR/logs/server.port" ] && [ \$i -lt 50 ]; do sleep 0.1; i=\$((i+1)); done
"$DIR/root/kuf-cod" --game-dir /game -- --fullscreen=false
status=\$?
echo "launcher exit \$status" >> "$DIR/logs/launch.log"
exit \$status
INSIDE
  chmod +x "$DIR/inside.sh"
  rm -f "$DIR/logs/server.port"
  echo "$(date '+%F %T') launch: installed $("$DIR/root/kuf-update" status | head -1)" >> "$DIR/logs/launch.log"
  set +e
  timeout --kill-after=10 3h bwrap \
    --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
    --ro-bind /etc /etc --proc /proc --dev /dev --tmpfs /tmp \
    --dev-bind /dev/dri /dev/dri --ro-bind /sys /sys \
    --ro-bind "$GAME_DIR" /game \
    --bind "$DIR" "$DIR" \
    --unshare-all --die-with-parent --new-session --clearenv \
    --setenv HOME "$DIR/home" --setenv XDG_RUNTIME_DIR /tmp --setenv PATH /usr/bin \
    --setenv XDG_CONFIG_HOME "$DIR/home/.config" --setenv XDG_DATA_HOME "$DIR/home/.local/share" \
    --setenv PULSE_CLIENTCONFIG "$DIR/home/pulse-client.conf" \
    --ro-bind "$SOCK" "/tmp/$WAYLAND_DISPLAY" --setenv WAYLAND_DISPLAY "$WAYLAND_DISPLAY" \
    --setenv SDL_VIDEO_DRIVER wayland "${PULSE[@]}" \
    "$DIR/inside.sh" > "$DIR/logs/launcher.out" 2>&1
  status=$?
  set -e
  echo "$(date '+%F %T') sandbox exit $status" >> "$DIR/logs/launch.log"
  echo "sandbox exit $status; logs in $DIR/logs and $DIR/home/.local/share/kuf-cod-recomp/data/logs"
  ;;
report)
  echo "== installed versions"; "$DIR/root/kuf-update" status || true
  echo "== update.conf"; cat "$DIR/root/update.conf"
  echo "== launches"; cat "$DIR/logs/launch.log" 2>/dev/null || true
  echo "== server requests"; grep -E 'GET|serving|ended' "$DIR/logs/server.log" 2>/dev/null || true
  LOGS="$DIR/home/.local/share/kuf-cod-recomp/data/logs"
  echo "== game log: updater lines"
  grep -h -E '\[kuf-title\] (update|installing)|restart|version' "$LOGS"/kuf_cod*.log 2>/dev/null | cut -c1-200 | tail -30 || true
  echo "== the player's real installation"
  snapshot_real > "$DIR/real-install.after"
  if cmp -s "$DIR/real-install.before" "$DIR/real-install.after"; then echo "unchanged"; else echo "CHANGED (diff the snapshots)"; fi
  ;;
*) sed -n '2,6p' "$0"; exit 2 ;;
esac
