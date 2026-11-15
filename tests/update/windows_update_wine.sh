#!/usr/bin/env bash
# The Windows updater end to end (M17G): the real kuf-update.exe under Wine, downloading over WinHTTP from the
# loopback release server (tools/updater/release_server.py), in a bubblewrap sandbox with its own network (loopback
# only). A release of a test package signed with a throwaway key; the update manifest made by make_manifest.py.
#
#   tests/update/windows_update_wine.sh KUF_UPDATE_EXE
#
# Checked: check (update available, summary), update (download progress, verifying, staged; the running version
# untouched), apply (the new version in place, the prepared game carried over, saves and game files untouched),
# status, rollback, and a package changed after signing refused. Wine: this tests the Windows program's own code
# (WinHTTP, the verification, the file operations), not Windows itself.
set -euo pipefail
EXE="$(realpath -e "${1:?the Windows kuf-update.exe}")"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LW="$ROOT/build/toolchains/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64/x86_64-w64-mingw32/bin"
for t in wine ssh-keygen python3 bwrap; do command -v $t > /dev/null || { echo "$t not found" >&2; exit 2; }; done
W="$(mktemp -d "${TMPDIR:-/tmp}/kuf-win-update.XXXXXX")"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/home" "$W/releases" "$W/tool"
cp "$ROOT/tools/updater/release_server.py" "$ROOT/tools/package/make_manifest.py" "$W/tool/"
ssh-keygen -q -t ed25519 -N '' -C test -f "$W/key" < /dev/null > /dev/null
SIGNERS="release@kuf-cod-recomp namespaces=\"kuf-cod-recomp-release,kuf-cod-recomp-manifest\" $(cut -d' ' -f1,2 "$W/key.pub")"

program() {  # folder version generation
  mkdir -p "$1/Runtime/bin" "$1/Runtime/builder"
  printf 'launcher %s\n' "$2" > "$1/Play KUF.exe"
  printf '%s\n' "$2" > "$1/Runtime/VERSION"
  printf 'generation_id %s\n' "$3" > "$1/Runtime/builder/recipe.txt"
  printf 'generator %s\n' "$2" > "$1/Runtime/bin/rexgluerd.exe"
  printf '%s\n' "$SIGNERS" > "$1/Runtime/allowed_signers"
}
release() {  # version generation [tamper]
  local v="$1" name="KUF-Circle-of-Doom-Portable-$1-windows-x86_64"
  local tree="$W/tree-$v/KUF-Circle-of-Doom-Portable"
  program "$tree" "$v" "$2"
  printf 'repo = owner/kuf\r\nasset_prefix = KUF-Circle-of-Doom-Portable\r\nplatform = windows-x86_64\r\napi = http://127.0.0.1:47123\r\n' > "$tree/Runtime/update.conf"
  mkdir -p "$tree/GameFiles" && echo "your game here" > "$tree/GameFiles/readme.txt"
  cp "$EXE" "$LW/libc++.dll" "$LW/libunwind.dll" "$tree/Runtime/bin/"  # every package carries the updater
  (cd "$W/tree-$v" && python3 -I -c 'import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, fs in os.walk("KUF-Circle-of-Doom-Portable"):
        for f in sorted(fs): z.write(os.path.join(d, f))' "$W/releases/$name.zip")
  printf 'Faster loading.\n' > "$W/summary.txt"
  python3 -I "$W/tool/make_manifest.py" "$W/releases/$name.zip" --summary "$W/summary.txt" --published 2026-11-10 > /dev/null
  ssh-keygen -q -Y sign -f "$W/key" -n kuf-cod-recomp-release "$W/releases/$name.zip" < /dev/null > /dev/null
  ssh-keygen -q -Y sign -f "$W/key" -n kuf-cod-recomp-manifest "$W/releases/$name.update.json" < /dev/null > /dev/null
  if [ "${3:-}" = tamper ]; then printf 'x' >> "$W/releases/$name.zip"; fi
  python3 -I -c 'import json, os, sys
d = sys.argv[1]
rels = {}
for n in sorted(os.listdir(d)):
    if n.startswith("KUF-Circle-of-Doom-Portable-"):
        v = n[len("KUF-Circle-of-Doom-Portable-"):].split("-windows")[0]
        rels.setdefault(v, []).append(n)
json.dump([{"tag_name": "v" + v, "html_url": "https://github.com/owner/kuf/releases/tag/v" + v, "assets": a}
           for v, a in rels.items()], open(os.path.join(d, "releases.json"), "w"))' "$W/releases"
}

P="$W/KUF Prüfung"
program "$P" 0.9.0 aaaa
cp "$EXE" "$LW/libc++.dll" "$LW/libunwind.dll" "$P/Runtime/bin/"
printf 'repo = owner/kuf\r\nasset_prefix = KUF-Circle-of-Doom-Portable\r\nplatform = windows-x86_64\r\napi = http://127.0.0.1:47123\r\n' > "$P/Runtime/update.conf"
printf 'the prepared game\n' > "$P/Runtime/bin/kuf_cod.exe"
mkdir -p "$P/Data/saves" "$P/GameFiles"
echo "my progress" > "$P/Data/saves/save.bin"
echo "game" > "$P/GameFiles/default.xex"
release 0.9.1 aaaa

cat > "$W/run.sh" <<'INNER'
#!/usr/bin/env bash
set -u
cd /run-data
wineboot -i > /dev/null 2>&1
python3 -I tool/release_server.py --dir releases --repo owner/kuf --port 47123 --log server.log &
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/47123) 2>/dev/null && break; sleep 0.1; done
run() { wine 'Z:\run-data\KUF Prüfung\Runtime\bin\kuf-update.exe' --json "$@" 2> /dev/null | tr -d '\r'; }
echo "== check"; run check
echo "== update"; run update
echo "== status after update"; run status
cat "KUF Prüfung/Runtime/VERSION" | sed 's/^/version before apply: /'
echo "== apply"; run apply
echo "== status after apply"; run status
echo "== rollback"; run rollback
echo "== rollback again (forward)"; run rollback
INNER
chmod +x "$W/run.sh"
OUT="$W/out.txt"
timeout --kill-after=5 300 bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 \
  --symlink usr/bin /bin --ro-bind /etc /etc --proc /proc --dev /dev --tmpfs /tmp --bind "$W" /run-data \
  --unshare-all --die-with-parent --new-session --clearenv --setenv HOME /run-data/home \
  --setenv WINEPREFIX /run-data/prefix --setenv WINEDEBUG -all --setenv PATH /usr/bin --setenv LANG C.UTF-8 \
  /run-data/run.sh > "$OUT" 2>&1 || true

fail=0
want() {  # description, pattern in the output or test command
  if eval "$2"; then echo "[ok] $1"; else echo "[FAIL] $1"; fail=1; fi
}
want "check: 0.9.1 available, its summary" 'grep -A1 "== check" "$OUT" | grep -q "\"latest\": \"0.9.1\".*\"update\": true.*\"summary\": \"Faster loading.\""'
section() { awk -v s="== $1" '$0 == s {on = 1; next} /^== / {on = 0} on' "$OUT"; }
want "update: progress lines over WinHTTP" 'section update | grep -q "\"progress\": [1-9]"'
want "update: verifying, installing, staged (exit 0, restart)" 'section update | grep -q "\"stage\": \"verifying\"" && section update | grep -q "\"exit\": 0, \"current\": \"0.9.1\", \"previous\": \"0.9.0\", \"restart\": true"'
want "the running version untouched until the next start" 'grep -q "version before apply: 0.9.0" "$OUT"'
want "apply: 0.9.1 in place" 'grep -A1 "== apply" "$OUT" | grep -q "now 0.9.1"'
want "Play KUF.exe replaced, the old one kept" 'grep -q "launcher 0.9.1" "$P/Play KUF.exe" 2>/dev/null || [ "$(cat "$P/Play KUF.exe.previous")" = "launcher 0.9.0" ]'
want "rollback: back to 0.9.0, then forward to 0.9.1" 'grep -A1 "== rollback$" "$OUT" | grep -q "switched back to 0.9.0" && grep -A1 "== rollback again" "$OUT" | grep -q "switched back to 0.9.1"'
want "the prepared game carried over (same generation)" '[ "$(cat "$P/Runtime/bin/kuf_cod.exe")" = "the prepared game" ]'
want "saves and game files untouched" '[ "$(cat "$P/Data/saves/save.bin")" = "my progress" ] && [ "$(cat "$P/GameFiles/default.xex")" = "game" ] && [ ! -e "$P/GameFiles/readme.txt" ]'
want "downloads only from the configured loopback origin (server log)" 'grep -q "GET /repos/owner/kuf/releases" "$W/server.log" && grep -q "GET /download/KUF-Circle-of-Doom-Portable-0.9.1-windows-x86_64.zip " "$W/server.log"'

# A package changed after signing: refused, nothing staged.
rm -rf "$W/releases" && mkdir -p "$W/releases" && release 0.9.2 bbbb tamper
cat > "$W/run.sh" <<'INNER'
#!/usr/bin/env bash
cd /run-data
python3 -I tool/release_server.py --dir releases --repo owner/kuf --port 47123 --log server.log &
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/47123) 2>/dev/null && break; sleep 0.1; done
wine 'Z:\run-data\KUF Prüfung\Runtime\bin\kuf-update.exe' --json update 2>/dev/null | tr -d '\r' | tail -1
INNER
timeout --kill-after=5 300 bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 \
  --symlink usr/bin /bin --ro-bind /etc /etc --proc /proc --dev /dev --tmpfs /tmp --bind "$W" /run-data \
  --unshare-all --die-with-parent --new-session --clearenv --setenv HOME /run-data/home \
  --setenv WINEPREFIX /run-data/prefix --setenv WINEDEBUG -all --setenv PATH /usr/bin --setenv LANG C.UTF-8 \
  /run-data/run.sh > "$W/tamper.txt" 2>&1 || true
want "a package changed after signing: refused (exit 4), nothing staged" 'grep -q "\"exit\": 4" "$W/tamper.txt" && [ ! -e "$P/.update-pending" ] && [ ! -e "$P/Runtime.next" ]'
if [ $fail -ne 0 ]; then echo "--- output"; cat "$OUT"; cat "$W/tamper.txt"; fi
exit $fail
