#!/usr/bin/env bash
# Cross-platform lobby test (M17F): the same lobby code built for Linux and for Windows, a host on one and a guest on
# the other, in every combination: LAN discovery (UDP), joining with the password proof both ways, session data
# byte for byte, the start, leaving, and a wrong password refused.
#
#   tests/net/cross_platform_lobby.sh LINUX_PEER WINDOWS_PEER_EXE
#
# The Windows program runs under Wine (it is a test of the Windows build's code and of the protocol between the two
# builds, not of Windows itself). Everything runs in one bubblewrap sandbox with its own network namespace (loopback
# only): nothing reaches the machine's network. Exit status 0 = every combination passed.
set -euo pipefail
LP="$(realpath -e "${1:?the Linux kuf_lobby_peer}")"
WP="$(realpath -e "${2:?the Windows kuf_lobby_peer.exe}")"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LW="$ROOT/build/toolchains/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64/x86_64-w64-mingw32/bin"
command -v wine > /dev/null || { echo "wine not found" >&2; exit 2; }
RUN="$(mktemp -d "${TMPDIR:-/tmp}/kuf-xplat-lobby.XXXXXX")"
trap 'rm -rf "$RUN"' EXIT
mkdir -p "$RUN/home" "$RUN/app"
cp "$LP" "$RUN/app/kuf_lobby_peer"
cp "$WP" "$LW/libc++.dll" "$LW/libunwind.dll" "$RUN/app/"

cat > "$RUN/run.sh" <<'INNER'
#!/usr/bin/env bash
set -u
cd /run-data/app
wineboot -i > /dev/null 2>&1
L=./kuf_lobby_peer
W="wine kuf_lobby_peer.exe"
pass=0 fail=0
combo() {  # name, host command, guest command, port
  local name="$1" host="$2" guest="$3" port="$4"
  $host host --port "$port" --discovery $((port + 1)) --password "s3cret pass" --name "Host" > "host-$port.txt" 2>&1 &
  local hp=$!
  for _ in $(seq 100); do grep -q READY "host-$port.txt" 2>/dev/null && break; sleep 0.1; done
  $guest join --port "$port" --discovery $((port + 1)) --password "s3cret pass" --name "Guest" > "guest-$port.txt" 2>&1
  local gs=$?
  wait $hp; local hs=$?
  # A wrong password against a second lobby of the same host system.
  $host host --port $((port + 2)) --discovery 0 --password "right" --name "Host" > "host2-$port.txt" 2>&1 &
  local hp2=$!
  for _ in $(seq 100); do grep -q READY "host2-$port.txt" 2>/dev/null && break; sleep 0.1; done
  $guest join --port $((port + 2)) --password "wrong" --name "Guest" --expect-reject > "guest2-$port.txt" 2>&1
  local rs=$?
  kill $hp2 2>/dev/null; wait $hp2 2>/dev/null
  if [ $gs -eq 0 ] && [ $hs -eq 0 ] && [ $rs -eq 0 ]; then
    echo "[ok] $name"; pass=$((pass + 1))
  else
    echo "[FAIL] $name (host $hs, guest $gs, wrong password $rs)"; fail=$((fail + 1))
  fi
  grep -hE "PASS|FAIL|discovered" "host-$port.txt" "guest-$port.txt" "guest2-$port.txt" | sed 's/^/     /'
}
combo "Linux host, Linux guest" "$L" "$L" 41000
combo "Windows host, Windows guest" "$W" "$W" 41010
combo "Linux host, Windows guest" "$L" "$W" 41020
combo "Windows host, Linux guest" "$W" "$L" 41030
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
INNER
chmod +x "$RUN/run.sh"

timeout --kill-after=5 600 bwrap \
  --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
  --ro-bind /etc /etc --proc /proc --dev /dev --tmpfs /tmp --bind "$RUN" /run-data \
  --unshare-all --die-with-parent --new-session --clearenv \
  --setenv HOME /run-data/home --setenv XDG_RUNTIME_DIR /tmp --setenv WINEPREFIX /run-data/prefix \
  --setenv WINEDEBUG -all --setenv PATH /usr/bin \
  /run-data/run.sh
