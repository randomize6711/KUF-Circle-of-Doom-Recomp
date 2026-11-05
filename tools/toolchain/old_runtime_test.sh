#!/usr/bin/env bash
# Runs a game package on an older Linux than this machine's (docs/STEAM_DECK.md "Portable build"): the Steam
# Runtime 3 sysroot (Debian 11: glibc 2.31, libstdc++ 10 = GLIBCXX_3.4.28) as the whole system, in bwrap. The
# package's own launcher (kuf-cod.sh) starts the game, which must load every library, choose the bundled libstdc++
# (the system's is older than the game's), run its main loop and exit cleanly on its timer. No graphics (the
# sysroot's 2021 Mesa does not know this machine's GPU; on a real system its own driver serves the game), offscreen
# video, ALSA's null sink, no network; the game folder read-only; nothing outside a new run folder is written.
#
#   tools/toolchain/old_runtime_test.sh PACKAGE.tar.gz [--seconds N] [--run-id ID]
# Exit 0 = PASS. Report: build/runs/<id>/old-runtime-report.txt
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PKG="$(realpath -e "${1:?package tarball}")"; shift
SECONDS_RUN=40
RUN_ID="old-runtime-$(date +%Y%m%d-%H%M%S)"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    --run-id) RUN_ID="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
V="$(sed -n 's/^set(KUFCOD_SNIPER_VERSION \(.*\))$/\1/p' "$ROOT/cmake/steamrt3-sniper-version.cmake")"
SR="$ROOT/build/toolchains/sniper-$V/sysroot"
[[ -f "$SR/.kuf-extracted" ]] || { echo "no sysroot: tools/toolchain/setup_sniper.sh" >&2; exit 1; }
GAME_DIR="${KUF_GAME_DIR:-$(cat "$ROOT/.game_dir" 2>/dev/null || true)}"
[[ -f "$GAME_DIR/default.xex" ]] || { echo "set KUF_GAME_DIR (the folder with default.xex)" >&2; exit 1; }
RUN="$ROOT/build/runs/$RUN_ID"
[[ ! -e "$RUN" ]] || { echo "$RUN exists" >&2; exit 1; }
mkdir -p "$RUN/home/.config" "$RUN/pkg"
tar -xzf "$PKG" -C "$RUN/pkg"
APP="$(find "$RUN/pkg" -mindepth 1 -maxdepth 1 -type d | head -1)"
printf 'pcm.!default { type null }\nctl.!default { type hw card 0 }\n' > "$RUN/home/.asoundrc"
REPORT="$RUN/old-runtime-report.txt"
{
  echo "package: $(basename "$PKG")"
  echo "system: $(sed -n 's/^PRETTY_NAME=//p' "$SR/usr/lib/os-release"), glibc $(basename "$(ls "$SR"/lib/x86_64-linux-gnu/libc-2.*.so)" .so | sed 's/libc-//'), libstdc++ GLIBCXX_$(grep -aoE 'GLIBCXX_3\.4\.[0-9]+' "$SR/usr/lib/x86_64-linux-gnu/libstdc++.so.6" | sed 's/GLIBCXX_//' | sort -uV | tail -1)"
  echo "package REQUIRES: $(tr '\n' ' ' < "$APP/REQUIRES" 2>/dev/null || echo "(none: an older package)")"
} > "$REPORT"
set +e
timeout --kill-after=5 $((SECONDS_RUN + 60)) bwrap \
  --ro-bind "$SR/usr" /usr --ro-bind "$SR/lib" /lib --ro-bind "$SR/lib64" /lib64 --ro-bind "$SR/bin" /bin \
  --ro-bind "$SR/sbin" /sbin --ro-bind "$SR/etc" /etc --proc /proc --dev /dev --tmpfs /tmp \
  --ro-bind "$GAME_DIR" /game --ro-bind "$APP" /app --bind "$RUN/home" /home/player \
  --unshare-all --die-with-parent --new-session --clearenv \
  --setenv HOME /home/player --setenv XDG_RUNTIME_DIR /tmp --setenv PATH /usr/bin:/bin \
  --setenv SDL_VIDEO_DRIVER offscreen --setenv SDL_AUDIO_DRIVER alsa --setenv KUF_NO_DIALOG 1 \
  /app/kuf-cod.sh --game-dir /game -- --headless=true --kuf_audio_pace=true \
    --kuf_exit_after_seconds="$SECONDS_RUN" --gpu_plugin "" > "$RUN/stdout.txt" 2>&1
status=$?
set -e
LOG="$RUN/home/.local/share/kuf-cod-recomp/data/logs/kuf_cod.log"
fail=0
check() { if eval "$2"; then echo "  [ok]   $1" >> "$REPORT"; else echo "  [FAIL] $1" >> "$REPORT"; fail=1; fi; }
check "exit status 0 (the timed exit), got $status" '[[ $status -eq 0 ]]'
check "the launcher chose the bundled libstdc++ (the system's is older)" 'grep -q "using the bundled one" "$RUN/stdout.txt"'
check "no missing library or symbol version" '! grep -qE "error while loading shared libraries|version .GLIBC|not found \(required by" "$RUN/stdout.txt"'
check "the game wrote its log" '[[ -s "$LOG" ]]'
check "the game's code ran (guest timebase ticking) until its timer asked it to exit" \
  'grep -qE "guest_ticks/s=[1-9]" "$LOG" && grep -q "requesting exit after" "$LOG"'
check "no crash report" '[[ ! -f "$RUN/home/.local/share/kuf-cod-recomp/data/user/crash-report.txt" ]]'
check "no [critical] lines" '! grep -q "\[critical\]" "$LOG"'
echo "OLD RUNTIME: $([[ $fail -eq 0 ]] && echo PASS || echo FAIL)" >> "$REPORT"
cat "$REPORT"
exit $fail
