#!/usr/bin/env bash
# The Windows setup (M16B): "Play KUF.exe" with the Windows portable edition's Runtime/ appended as a .tar.gz, then the
# reference list and a trailer (installer/src/frontend/launcher.cpp ReadPayloadTrailer). Started, it is the setup: drag
# and drop your game files, choose the folder, Install (unpacked with Windows' own tar.exe, Windows 10 1803+ / 11).
#
#   tools/package/make_windows_setup.sh PORTABLE_DIR OUT.exe [LAUNCHER]
# PORTABLE_DIR: make_windows_portable.sh's unpacked folder (audited); LAUNCHER: default its Play KUF.exe.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
P="$(realpath "${1:?usage: $0 PORTABLE_DIR OUT.exe [LAUNCHER]}")"; OUT="$(realpath -m "${2:?}")"
LAUNCHER="${3:-$P/Play KUF.exe}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/kuf-setup-XXXXXX")"; trap 'rm -rf "$TMP"' EXIT
tar -C "$P" --owner=0 --group=0 --numeric-owner -czf "$TMP/payload.tar.gz" Runtime
cp "$LAUNCHER" "$OUT.part"
OFF=$(stat -c %s "$OUT.part"); cat "$TMP/payload.tar.gz" >> "$OUT.part"; SIZE=$(stat -c %s "$TMP/payload.tar.gz")
MOFF=$(stat -c %s "$OUT.part"); cat "$P/Runtime/assets/game_v1.tsv" >> "$OUT.part"; MSIZE=$(stat -c %s "$P/Runtime/assets/game_v1.tsv")
python3 -I - "$OUT.part" "$OFF" "$SIZE" "$MOFF" "$MSIZE" <<'PY'
import struct, sys
with open(sys.argv[1], "ab") as f:
    f.write(b"KUFSETUP" + struct.pack("<QQQQ", *map(int, sys.argv[2:6])))
PY
mv "$OUT.part" "$OUT"
sha256sum "$OUT"; ls -la "$OUT"
