#!/usr/bin/env bash
# Developer co-op test: two sandboxed instances of the game on this machine, a host and a guest (docs/MULTIPLAYER.md
# "Two-instance test"). Run only with the player's permission: it starts the game twice.
#
#   - one network namespace of their own with nothing but loopback (an outer bubblewrap with --unshare-net): the
#     host is 127.0.0.10, the guest 127.0.0.11; the machine's network, router and firewall are never reachable or
#     touched. Each instance is otherwise the usual tools/run_sandboxed.sh sandbox (--share-net, which refuses to
#     run unless its network namespace is loopback-only)
#   - separate run directories build/runs/<prefix>-host and <prefix>-guest, each with its own saves: --host-saves /
#     --guest-saves are COPIED in (the sources are only read; nothing is written back), so a test never changes
#     another character's save or the player's own saves
#   - the guest starts a few seconds after the host; both end at the time limit (or when closed)
#
# Usage: tools/run_coop_pair.sh -n <prefix> [-t seconds] [--gpu] [--window] [--audio]
#                               [--host-saves dir] [--guest-saves dir] [--pc-settings-from file|dir]
#                               [-- extra kuf_cod args for both]
#   --audio gives the host sound (the guest stays silent).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX=""
TIMEOUT=600
COMMON=()
HOST_ONLY=()
HOST_SAVES=""
GUEST_SAVES=""
while [ $# -gt 0 ]; do
  case "$1" in
    -n) PREFIX="$2"; shift 2 ;;
    -t) TIMEOUT="$2"; shift 2 ;;
    --gpu|--window) COMMON+=("$1"); shift ;;
    --audio) HOST_ONLY+=(--audio); shift ;;
    --host-saves) HOST_SAVES="$(realpath -e "$2")"; shift 2 ;;
    --guest-saves) GUEST_SAVES="$(realpath -e "$2")"; shift 2 ;;
    --pc-settings-from) COMMON+=(--pc-settings-from "$(realpath -e "$2")"); shift 2 ;;
    --inside) INSIDE=1; shift ;;
    --) shift; break ;;
    *) echo "unknown option $1 (see the header of $0)" >&2; exit 2 ;;
  esac
done
[ -n "$PREFIX" ] || { echo "-n <prefix> is required" >&2; exit 2; }
[[ "$PREFIX" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "the prefix may only use letters, digits, '.', '_' and '-'" >&2; exit 2; }

if [ -z "${INSIDE:-}" ]; then
  # The outer namespace: the whole file system as it is, only the network replaced by an empty loopback.
  AGAIN=(-n "$PREFIX" -t "$TIMEOUT" "${COMMON[@]}" "${HOST_ONLY[@]}")
  [ -n "$HOST_SAVES" ] && AGAIN+=(--host-saves "$HOST_SAVES")
  [ -n "$GUEST_SAVES" ] && AGAIN+=(--guest-saves "$GUEST_SAVES")
  exec bwrap --dev-bind / / --unshare-net --die-with-parent -- "$0" --inside "${AGAIN[@]}" -- "$@"
fi

SAVES_H=(); [ -n "$HOST_SAVES" ] && SAVES_H=(--saves-from "$HOST_SAVES")
SAVES_G=(); [ -n "$GUEST_SAVES" ] && SAVES_G=(--saves-from "$GUEST_SAVES")

COOP=(--kuf_coop_port_base=37000 --kuf_coop_lobby_port=36961)
"$ROOT/tools/run_sandboxed.sh" --share-net -n "$PREFIX-host" -t "$TIMEOUT" "${COMMON[@]}" "${HOST_ONLY[@]}" \
  "${SAVES_H[@]}" -- --kuf_coop=host --kuf_coop_ip=127.0.0.10 --kuf_coop_name=Host "${COOP[@]}" "$@" &
HOST_PID=$!
sleep 5
"$ROOT/tools/run_sandboxed.sh" --share-net -n "$PREFIX-guest" -t "$TIMEOUT" "${COMMON[@]}" "${SAVES_G[@]}" \
  -- --kuf_coop=join --kuf_coop_ip=127.0.0.11 --kuf_coop_peer=127.0.0.10 --kuf_coop_name=Guest "${COOP[@]}" "$@" &
GUEST_PID=$!
status=0
wait "$HOST_PID" || status=$?
wait "$GUEST_PID" || status=$?
echo "co-op pair finished -- logs in build/runs/$PREFIX-host and build/runs/$PREFIX-guest ([kuf-coop] lines)"
exit "$status"
