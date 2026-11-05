#!/usr/bin/env bash
# The public installation experience, tested on a clean system (docs/LOCAL_BUILD.md): Valve's Steam Runtime 3 Platform
# image (no compiler, linker, make or CMake) is the whole system, in bwrap, with an empty environment and no network; only
# the release's AppImage and a game folder are visible. A fixture home with fake saves and settings; a throwaway key for
# the update tests. The player's real installation, saves and settings are compared before and after, never used.
#
#   tools/package/local_install_test.sh RELEASE_DIR GAME_DIR RUN [--rc1-package TARBALL] [--retail-xex FILE]
#
# GAME_DIR holds the game with an unencrypted default.xex; it is only read, and copies for the failure cases are reflinks.
# --retail-xex: an encrypted default.xex for the refusal test (only read). Output: build/local-install-test/<RUN>/.
# Exit 0 when every check passed.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REL="$(realpath "$1")"; GAME="$(realpath "$2")"; RUN="$3"; shift 3
RC1="" RETAIL=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --rc1-package) RC1="$(realpath "$2")"; shift 2 ;;
    --retail-xex) RETAIL="$(realpath "$2")"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
T="$ROOT/build/local-install-test/$RUN"
[[ ! -e "$T" ]] || { echo "$T exists" >&2; exit 1; }
mkdir -p "$T"
P="$ROOT/build/toolchains/sniper-platform"
[[ -f "$P/.kuf-extracted" ]] || { echo "no $P (unpack the Platform image: docs/LOCAL_BUILD.md)" >&2; exit 1; }
IMG="$(ls "$REL"/KingdomUnderFire-CircleOfDoom-Setup-*.AppImage)"
PKG="$(ls "$REL"/kuf-cod-recomp-*-linux-x86_64.tar.gz)"
V="$(basename "$PKG" | sed -E 's/^kuf-cod-recomp-(.*)-linux-x86_64\.tar\.gz$/\1/')"
cp "$IMG" "$T/setup.AppImage"; chmod +x "$T/setup.AppImage"
FAILS=0 CHECKS=0
check() { CHECKS=$((CHECKS + 1)); if eval "$2"; then echo "  [ok] $1"; else echo "  [FAIL] $1"; FAILS=$((FAILS + 1)); fi; }
say() { echo "== $* ($(date +%T))"; }

real_snapshot() {
  for d in "$HOME/Games/Kingdom Under Fire - Circle of Doom" "$HOME/.local/share/kuf-cod-recomp" "$HOME/.config/kuf-cod-recomp"; do
    [[ -e "$d" ]] || continue
    find "$d" -printf '%p %y %s %T@ %l\n' | LC_ALL=C sort
  done
}
real_snapshot > "$T/real.before"

fixture_home() {
  mkdir -p "$1/.local/share/kuf-cod-recomp/data/"{saves,user} "$1/.config/kuf-cod-recomp" "$1/Desktop"
  head -c 65536 /dev/urandom > "$1/.local/share/kuf-cod-recomp/data/saves/fixture-save.bin"
  printf '[display]\nrender_scale = 2\n# fixture\n' > "$1/.local/share/kuf-cod-recomp/data/user/pc_settings.toml"
  printf 'fixture bindings\n' > "$1/.local/share/kuf-cod-recomp/data/user/bindings.toml"
}
data_hash() { (cd "$1/.local/share/kuf-cod-recomp/data" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum); }

# box HOME GAME_FOLDER [extra bwrap options] -- command...: the clean system.
box() {
  local home="$1" game="$2"; shift 2
  local extra=()
  while [[ "$1" != "--" ]]; do extra+=("$1"); shift; done
  shift
  bwrap --ro-bind "$P/usr" /usr --ro-bind "$P/lib" /lib --ro-bind "$P/lib64" /lib64 --ro-bind "$P/bin" /bin \
    --ro-bind "$P/sbin" /sbin --ro-bind "$P/etc" /etc --proc /proc --dev /dev --tmpfs /tmp \
    --ro-bind "$T/setup.AppImage" /setup.AppImage --ro-bind "$game" /game --bind "$home" /home/player \
    "${extra[@]}" --unshare-all --die-with-parent --new-session --clearenv \
    --setenv HOME /home/player --setenv XDG_RUNTIME_DIR /tmp --setenv PATH /usr/bin:/bin \
    --setenv APPIMAGE_EXTRACT_AND_RUN 1 --setenv KUF_NO_DIALOG 1 --setenv SDL_VIDEO_DRIVER offscreen \
    --setenv SDL_AUDIO_DRIVER alsa "$@"
}
SETUP=(/setup.AppImage --install-to /home/player/Games/KUF --game-dir /game --ignore-graphics-check --no-menu)

measured() {  # log file, command...: one line of wall time, processor time and the largest process.
  python3 -I - "$@" <<'PY'
import resource, subprocess, sys, time
log, cmd = sys.argv[1], sys.argv[2:]
start = time.time()
with open(log, "w") as out:
    code = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT).returncode
use = resource.getrusage(resource.RUSAGE_CHILDREN)
print(f"exit {code}; wall {time.time() - start:.0f} s; processor {use.ru_utime + use.ru_stime:.0f} s; "
      f"largest process {use.ru_maxrss / 1024:.0f} MiB")
sys.exit(code)
PY
}

say "system: $(sed -n 's/^PRETTY_NAME=//p' "$P/usr/lib/os-release") $(ls "$P"/lib/x86_64-linux-gnu/libc-2.* | sed 's/.*libc-//; s/\.so//'); tools present:"
for tool in gcc g++ cc clang ld make cmake ninja; do [[ -e "$P/usr/bin/$tool" ]] && echo "  $tool PRESENT" || true; done

# ---- refusals: nothing may be written -------------------------------------------------------------------------------
mkdir -p "$T/refuse/home"
fixture_home "$T/refuse/home"
refused() {  # name, folder, expected text in the message
  local out rc want="$3"
  out="$(box "$T/refuse/home" "$2" -- "${SETUP[@]}" 2>&1)"; rc=$?
  echo "    $(echo "$out" | grep 'kuf-setup:' | tail -1 | cut -c1-150)"
  check "$1: refused" '[[ $rc -ne 0 && "$out" == *"$want"* ]]'
  check "$1: nothing written" '[[ ! -e "$T/refuse/home/Games" ]]'
}
reflinked() {  # destination: a copy of the game folder (reflinks where the filesystem can)
  cp -r --reflink=auto "$GAME" "$1"
  chmod -R u+w "$1"
}
say "3. missing game files"
reflinked "$T/refuse/missing"
rm -f "${T:?}/refuse/missing/Data/Material/CodShaders.ccz"
refused "a missing game file" "$T/refuse/missing" "missing"
mkdir -p "$T/refuse/empty"
refused "an empty folder" "$T/refuse/empty" "no default.xex"
say "4. unsupported game versions"
reflinked "$T/refuse/other"
python3 -I - "$T/refuse/other/default.xex" <<'PY'
import sys
d = bytearray(open(sys.argv[1], "rb").read())
d[-100] ^= 1
open(sys.argv[1], "wb").write(d)
PY
refused "another version of default.xex" "$T/refuse/other" "unsupported game version"
if [[ -n "$RETAIL" ]]; then
  reflinked "$T/refuse/encrypted"
  rm -f "${T:?}/refuse/encrypted/default.xex"
  cp --reflink=auto "$RETAIL" "$T/refuse/encrypted/default.xex"
  refused "an encrypted (retail) default.xex" "$T/refuse/encrypted" "encrypted"
  OUT="$(box "$T/refuse/home" "$T/refuse/encrypted" -- "${SETUP[@]}" 2>&1)"
  check "the refusal says no keys are carried" '[[ "$OUT" == *"no console decryption keys"* ]]'
else
  echo "  [not run] an encrypted default.xex (no --retail-xex)"
fi
say "7. insufficient disk space"
OUT="$(box "$T/refuse/home" "$GAME" --size $((3 << 30)) --tmpfs /home/player/Small -- \
  /setup.AppImage --install-to /home/player/Small/KUF --game-dir /game --ignore-graphics-check --no-menu 2>&1)"; RC=$?
echo "    $(echo "$OUT" | grep 'kuf-setup:' | tail -1 | cut -c1-150)"
check "3 GiB free: refused before writing" '[[ $RC -ne 0 && "$OUT" == *"not enough free space"* ]]'

# ---- 5. cancelling while compiling ----------------------------------------------------------------------------------
say "5. cancellation while compiling"
C="$T/cancel/home"; mkdir -p "$C"; fixture_home "$C"
data_hash "$C" > "$T/cancel.before"
cat > "$C/stop.sh" <<'EOF'
#!/bin/sh
# Starts the setup, waits for the given number of compiled objects, sends the signal.
/setup.AppImage --install-to /home/player/Games/KUF --game-dir /game --ignore-graphics-check --no-menu --jobs 4 > /home/player/setup.log 2>&1 &
SETUP=$!
while kill -0 $SETUP 2>/dev/null; do
  n=$(ls /home/player/Games/.KUF.kuf-buildcache/*/obj/*.o 2>/dev/null | wc -l)
  [ "$n" -ge "$2" ] && break
  sleep 1
done
echo "$n objects when signalled"
PID=$(pidof -s kuf-setup || echo $SETUP)
kill -"$1" "$PID"
wait $SETUP
echo "setup exit $?"
EOF
chmod +x "$C/stop.sh"
box "$C" "$GAME" -- /bin/sh /home/player/stop.sh TERM 10 | sed 's/^/    /'
tail -2 "$C/setup.log" | cut -c1-130 | sed 's/^/    /'
check "cancelled: no installation" '[[ ! -e "$C/Games/KUF" ]]'
check "cancelled: no staging folder, no build folder" '[[ -z "$(ls -A "$C/Games" 2>/dev/null)" ]]'
check "cancelled: saves and settings unchanged" 'data_hash "$C" | cmp -s - "$T/cancel.before"'

# ---- 6 and 15. a crash while compiling, then recovery ---------------------------------------------------------------
say "6, 15. killed while compiling (as a crash or power cut); the next attempt recovers"
K="$T/crash/home"; mkdir -p "$K"; fixture_home "$K"
data_hash "$K" > "$T/crash.before"
cp "$C/stop.sh" "$K/stop.sh"
box "$K" "$GAME" -- /bin/sh /home/player/stop.sh KILL 20 | sed 's/^/    /'
KEPT="$(ls "$K"/Games/.KUF.kuf-buildcache/*/obj/*.o 2>/dev/null | wc -l)"
echo "    left behind: $(ls -A "$K/Games" | tr '\n' ' '); $KEPT objects"
check "killed: no installation" '[[ ! -e "$K/Games/KUF" ]]'
check "killed: finished objects kept" '[[ "$KEPT" -ge 15 ]]'
MARK="$K/mark"; touch "$MARK"; sleep 2
box "$K" "$GAME" -- "${SETUP[@]}" --jobs 4 > "$T/crash.log" 2>&1; RC=$?
tail -1 "$T/crash.log" | cut -c1-130 | sed 's/^/    /'
check "the next attempt installs" '[[ $RC -eq 0 && -x "$K/Games/KUF/current/bin/kuf_cod" ]]'
OLD="$(find "$K/Games/KUF/build-cache" -name '*.o' ! -newer "$MARK" | wc -l)"
check "it reused the objects of the killed attempt ($OLD of 162)" '[[ "$OLD" -ge 15 ]]'
check "the leftovers are gone" '[[ "$(ls -A "$K/Games")" == KUF ]]'
check "saves and settings unchanged" 'data_hash "$K" | cmp -s - "$T/crash.before"'

# ---- 1, 2. the clean installation -----------------------------------------------------------------------------------
say "1, 2. clean installation with valid game files"
H="$T/clean/home"; mkdir -p "$H"; fixture_home "$H"
data_hash "$H" > "$T/clean.before"
( peak=0; while :; do s="$(du -sb "$H/Games" 2>/dev/null | cut -f1)"; [[ "${s:-0}" -gt "$peak" ]] && { peak="$s"; echo "$peak" > "$T/clean.peak"; }; sleep 2; done ) &
WATCH=$!
cat > "$H/measure.py" <<'PY'
# Inside the sandbox: its processes are waited for here, so their processor time and sizes add up here.
import resource, subprocess, sys, time
start = time.time()
code = subprocess.run(sys.argv[1:]).returncode
use = resource.getrusage(resource.RUSAGE_CHILDREN)
print(f"MEASURED exit {code}; wall {time.time() - start:.0f} s; processor {use.ru_utime + use.ru_stime:.0f} s; "
      f"largest process {use.ru_maxrss / 1024:.0f} MiB")
sys.exit(code)
PY
box "$H" "$GAME" -- python3 -I /home/player/measure.py "${SETUP[@]}" --jobs 4 > "$T/clean.log" 2>&1; RC=$?
MEASURE="$(grep MEASURED "$T/clean.log")"
rm -f "${H:?}/measure.py"
kill "$WATCH" 2>/dev/null; wait "$WATCH" 2>/dev/null
echo "    $MEASURE"
echo "    peak in the target's folder: $(($(cat "$T/clean.peak") / 1048576)) MiB; final: $(($(du -sb "$H/Games/KUF" | cut -f1) / 1048576)) MiB" \
  "(game $(($(du -sb "$H/Games/KUF/game" | cut -f1) / 1048576)), program and tools $(($(du -sb "$H/Games/KUF/versions" | cut -f1) / 1048576)), cache $(($(du -sb "$H/Games/KUF/build-cache" | cut -f1) / 1048576)))"
grep -E '^\[' "$T/clean.log" | awk '{print $1}' | uniq | tr '\n' ' ' | sed 's/^/    stages: /'; echo
R="$H/Games/KUF"
check "installed" '[[ $RC -eq 0 && -f "$R/.kuf-install" && -x "$R/kuf-cod" ]]'
check "current -> versions/$V" '[[ "$(readlink "$R/current")" == "versions/$V" ]]'
check "the game program was made here" '[[ -x "$R/current/bin/kuf_cod" ]] && ! tar -tzf "$PKG" | grep -q "bin/kuf_cod$"'
check "the program links the runtime by name, from its own folder" 'readelf -d "$R/current/bin/kuf_cod" | grep -q "RUNPATH.*\$ORIGIN"'
check "the program needs no newer glibc than the baseline" '[[ "$(objdump -T "$R/current/bin/kuf_cod" | grep -oE "GLIBC_2\.[0-9]+" | sort -uV | tail -1 | sed "s/GLIBC_2.//")" -le 31 ]]'
check "137 game files" '[[ "$(find "$R/game" -type f | wc -l)" -eq 137 ]]'
check "no generated source left" '[[ -z "$(find "$R" -name "kuf_cod_recomp.*.cpp" -print -quit)" ]]'
check "no staging or build folder left next to it" '[[ "$(ls -A "$H/Games")" == KUF ]]'
check "saves and settings unchanged" 'data_hash "$H" | cmp -s - "$T/clean.before"'
sha256sum "$R/current/bin/kuf_cod" | cut -c1-16 | sed 's/^/    program SHA-256: /'
OTHER="$(sha256sum "$K/Games/KUF/current/bin/kuf_cod" | cut -d' ' -f1)"
check "reproducible: the same program as the recovered installation's" '[[ "$OTHER" == "$(sha256sum "$R/current/bin/kuf_cod" | cut -d" " -f1)" ]]'

# ---- 12, 14. the game starts on this old, clean system --------------------------------------------------------------
say "12, 14. game startup on the clean system (no graphics card in the sandbox: renderer off, offscreen)"
printf 'pcm.!default { type null }\nctl.!default { type hw card 0 }\n' > "$H/.asoundrc"
timeout --kill-after=5 120 bash -c "$(declare -f box); $(declare -p P T); box '$H' '$GAME' -- /home/player/Games/KUF/current/kuf-cod.sh --game-dir /game -- --headless=true --kuf_audio_pace=true --kuf_exit_after_seconds=30 --gpu_plugin ''" > "$T/start.log" 2>&1; RC=$?
GLOG="$H/.local/share/kuf-cod-recomp/data/logs/kuf_cod.log"
check "started and exited cleanly (exit $RC)" '[[ $RC -eq 0 ]]'
check "the bundled libstdc++ chosen on this old system" 'grep -q "using the bundled" "$T/start.log"'
check "ran the game (log)" '[[ -s "$GLOG" ]] && ! grep -q "encrypted" "$GLOG"'
mkdir -p "$T/keep"; cp "$GLOG" "$T/keep/start-kuf_cod.log" 2>/dev/null

# ---- 13. Vulkan: this machine's GPU, the sandbox the project's tests use --------------------------------------------
say "13. Vulkan initialisation and rendering with the program made on the clean system (this machine's GPU)"
RUNP="$ROOT/build/lt-$RUN"; mkdir -p "$RUNP"; ln -sfn "$R/current/bin" "$RUNP/bin"
(cd "$ROOT" && KUF_GAME_DIR="$GAME" nice python3 -I tools/menu_test.py --preset "lt-$RUN" --run-prefix "lt-$RUN" tests/perf/render_equivalence.toml) > "$T/vulkan.log" 2>&1; RC=$?
tail -4 "$T/vulkan.log" | cut -c1-140 | sed 's/^/    /'
VLOG="$ROOT/build/runs/lt-$RUN-render_equivalence/kuf_cod.log"
check "menu scenario passed (exit $RC)" '[[ $RC -eq 0 ]] && grep -q "\[ok\] images" "$T/vulkan.log"'
check "Vulkan device created" 'grep -qiE "vulkan|radv" "$VLOG"'
grep -m2 -iE 'device:|Vulkan device|selected.*device|api version' "$VLOG" | cut -c1-140 | sed 's/^/    /'

# ---- 8, 10, 11. reinstalling over it --------------------------------------------------------------------------------
say "8, 10, 11. reinstallation over the existing installation"
data_hash "$H" > "$T/repair.before"
printf '\n# my edit\n' >> "$R/update.conf"; cp "$R/update.conf" "$T/update.conf.mine"
MARK="$H/mark2"; touch "$MARK"; sleep 2
MEASURE="$(measured "$T/repair.log" bash -c "$(declare -f box); $(declare -p P T); box '$H' '$GAME' -- /setup.AppImage --install-to /home/player/Games/KUF --game-dir /game --ignore-graphics-check --no-menu")"; RC=$?
echo "    $MEASURE"
check "repaired" '[[ $RC -eq 0 && -x "$R/current/bin/kuf_cod" ]]'
check "no recompilation: every object reused" '[[ -z "$(find "$R/build-cache" -name "*.o" -newer "$MARK" -print -quit)" ]]'
check "saves and settings unchanged" 'data_hash "$H" | cmp -s - "$T/repair.before"'
check "the player'\''s update.conf kept" 'cmp -s "$R/update.conf" "$T/update.conf.mine"'

# ---- 9. updating ----------------------------------------------------------------------------------------------------
say "9. updates (a loopback release server, a throwaway key: mechanism, not the production signature)"
U="$T/update"; mkdir -p "$U/rel" "$U/work"
KEYDIR="$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/kuf-local-test-key.XXXXXX")"; rmdir "$KEYDIR"  # never inside the repository
"$ROOT/tools/package/sign_package.sh" --new-key "$KEYDIR" --no-passphrase >/dev/null
cp "$KEYDIR/allowed_signers" "$R/allowed_signers"
echo "channel = prerelease" >> "$R/update.conf"
make_version() {  # version, recipe id or "", extra edit (a command run in the unpacked package), repackaged and signed
  local nv="$1" id="$2" edit="${3:-:}" w="$U/work/$1" name="kuf-cod-recomp-$1-linux-x86_64"
  mkdir -p "$w"; tar -xzf "$PKG" -C "$w"
  mv "$w/kuf-cod-recomp-$V-linux-x86_64" "$w/$name"
  echo "$nv" > "$w/$name/VERSION"
  cp "$KEYDIR/allowed_signers" "$w/$name/allowed_signers"
  [[ -n "$id" ]] && sed -i "s/^generation_id .*/generation_id $id/" "$w/$name/builder/recipe.txt"
  (cd "$w/$name" && eval "$edit")
  (cd "$w/$name" && rm -f MANIFEST.sha256 && find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | LC_ALL=C sort | xargs -d '\n' sha256sum > MANIFEST.sha256)
  tar --sort=name --owner=0 --group=0 --numeric-owner -C "$w" -czf "$U/rel/$name.tar.gz" "$name"
  rm -rf "${w:?}"
  echo "update summary" > "$U/summary.txt"
  (cd "$U/rel" && python3 -I "$ROOT/tools/package/make_manifest.py" "$name.tar.gz" --channel prerelease --summary "$U/summary.txt" >/dev/null)
  "$ROOT/tools/package/sign_package.sh" --key "$KEYDIR/kuf-cod-release" "$U/rel/$name.tar.gz" >/dev/null
  "$ROOT/tools/package/sign_package.sh" --key "$KEYDIR/kuf-cod-release" --manifest "$U/rel/$name.update.json" >/dev/null
}
index() {  # the versions the server offers, newest first
  python3 -I - "$U/rel" "$@" <<'PY'
import json, sys
d, vs = sys.argv[1], sys.argv[2:]
rel = []
for v in vs:
    b = f"kuf-cod-recomp-{v}-linux-x86_64"
    rel.append({"tag_name": "v" + v, "name": v, "draft": False, "prerelease": True, "body": "test",
                "published_at": "2026-11-04T00:00:00Z", "html_url": "https://github.com/test/kuf/releases/tag/v" + v,
                "assets": [b + s for s in (".tar.gz", ".tar.gz.sig", ".update.json", ".update.json.sig")]})
json.dump(rel, open(d + "/releases.json", "w"))
PY
}
PORT=$((20000 + RANDOM % 20000))
python3 -I "$ROOT/tools/updater/release_server.py" --dir "$U/rel" --repo test/kuf --port "$PORT" > "$U/server.log" 2>&1 &
SERVER=$!
sleep 1
sed -i '/^repo = /d; /^api = /d' "$R/update.conf"
printf 'repo = test/kuf\napi = http://127.0.0.1:%s\n' "$PORT" >> "$R/update.conf"
KU() { HOME="$H" XDG_DATA_HOME="$H/.local/share" XDG_CONFIG_HOME="$H/.config" timeout 900 python3 -I "$R/current/kuf_update.py" --install-root "$R" "$@"; }
data_hash "$H" > "$U/data.before"

make_version 0.8.0-rc.90 "" ; index 0.8.0-rc.90
MARK="$H/mark3"; touch "$MARK"; sleep 2
start=$(date +%s); OUT="$(KU --json update --yes 2>&1)"; RC=$?; took=$(( $(date +%s) - start ))
echo "    same generation: $took s; $(echo "$OUT" | tail -1 | cut -c1-120)"
check "update installed" '[[ $RC -eq 0 && "$(readlink "$R/current")" == "versions/0.8.0-rc.90" ]]'
check "only linked: no object compiled" '[[ -z "$(find "$R/build-cache" -name "*.o" -newer "$MARK" -print -quit)" ]]'
check "the new version has its program" '[[ -x "$R/current/bin/kuf_cod" ]]'
check "the previous version is kept" '[[ -x "$R/versions/$V/bin/kuf_cod" ]]'

make_version 0.8.0-rc.91 "" 'printf "#!/bin/sh\nexit 1\n" > builder/toolchain/llvm' ; index 0.8.0-rc.91
OUT="$(KU --json update --yes 2>&1)"; RC=$?
echo "    broken compiler, same generation: $(echo "$OUT" | tail -1 | cut -c1-160)"
check "a failed build refuses the update; the old version stays current" '[[ $RC -ne 0 && "$(readlink "$R/current")" == "versions/0.8.0-rc.90" && -x "$R/current/bin/kuf_cod" ]]'
check "nothing staged is left" '[[ ! -e "$R/versions/0.8.0-rc.91" && -z "$(find "$R/versions" -maxdepth 1 -name ".staging-*" -print -quit)" ]]'

make_version 0.8.0-rc.92 "$(printf 'b%.0s' {1..64})" ; index 0.8.0-rc.92
start=$(date +%s); OUT="$(KU --json update --yes 2>&1)"; RC=$?; took=$(( $(date +%s) - start ))
echo "    changed generation: $took s; $(echo "$OUT" | tail -1 | cut -c1-120)"
check "a changed generation regenerates, compiles, then switches" '[[ $RC -eq 0 && "$(readlink "$R/current")" == "versions/0.8.0-rc.92" && -x "$R/current/bin/kuf_cod" ]]'
check "the old generation's objects are gone" '[[ "$(ls "$R/build-cache" | grep -vc build.log)" -eq 1 ]]'

make_version 0.8.0-rc.93 ""; cp "$U/rel/kuf-cod-recomp-0.8.0-rc.93-linux-x86_64.tar.gz.sig" "$U/keep.sig"
printf 'x' >> "$U/rel/kuf-cod-recomp-0.8.0-rc.93-linux-x86_64.tar.gz"; index 0.8.0-rc.93
OUT="$(KU --json update --yes 2>&1)"; RC=$?
check "a tampered package is refused (signature or SHA-256)" '[[ $RC -ne 0 && "$(readlink "$R/current")" == "versions/0.8.0-rc.92" ]]'

OUT="$(KU --json rollback 2>&1)"; RC=$?
check "rollback to the previous version" '[[ $RC -eq 0 && "$(readlink "$R/current")" == "versions/0.8.0-rc.90" ]]'
check "saves and settings unchanged through all of it" 'data_hash "$H" | cmp -s - "$U/data.before"'

if [[ -n "$RC1" ]]; then
  say "9b. an installation of an older updater (rc.1) is offered this model's package"
  O="$T/old"; mkdir -p "$O/pkg" "$O/home"; fixture_home "$O/home"
  tar -xzf "$RC1" -C "$O/pkg"
  OLDP="$(ls -d "$O"/pkg/kuf-cod-recomp-*)"
  HOME="$O/home" XDG_DATA_HOME="$O/home/.local/share" "$OLDP/install.sh" --root "$O/root" --signers "$KEYDIR/allowed_signers" --repo test/kuf \
    --api "http://127.0.0.1:$PORT" > "$O/install.log" 2>&1
  echo "channel = prerelease" >> "$O/root/update.conf"
  make_version 0.8.0-rc.94 ""; index 0.8.0-rc.94
  OLDV="$(readlink "$O/root/current")"
  OUT="$(HOME="$O/home" XDG_DATA_HOME="$O/home/.local/share" python3 -I "$O/root/current/kuf_update.py" --install-root "$O/root" --json update --yes 2>&1)"; RC=$?
  echo "    $(echo "$OUT" | tail -1 | cut -c1-170)"
  check "the old updater refuses it, because it has no program, and stays on its version" '[[ $RC -ne 0 && "$OUT" == *"bin/kuf_cod"* && "$(readlink "$O/root/current")" == "$OLDV" && -x "$O/root/current/bin/kuf_cod" ]]'
fi
kill "$SERVER" 2>/dev/null; wait "$SERVER" 2>/dev/null
rm -rf "${KEYDIR:?}"  # the throwaway key

# ---- uninstall, and the real data -----------------------------------------------------------------------------------
say "uninstall (saves kept)"
data_hash "$H" > "$T/uninstall.before"
box "$H" "$GAME" -- /setup.AppImage --remove /home/player/Games/KUF > "$T/remove.log" 2>&1; RC=$?
check "removed, its cache too" '[[ $RC -eq 0 && ! -e "$R/build-cache" && ! -e "$R/versions" ]]'
check "saves and settings kept" 'data_hash "$H" | cmp -s - "$T/uninstall.before"'

real_snapshot > "$T/real.after"
check "the real installation, saves and settings unchanged" 'cmp -s "$T/real.before" "$T/real.after"'
echo
echo "RESULT: $((CHECKS - FAILS)) of $CHECKS checks passed; $([[ $FAILS -eq 0 ]] && echo PASS || echo "FAIL ($FAILS)")"
[[ $FAILS -eq 0 ]]
