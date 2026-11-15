#!/usr/bin/env bash
# The Linux portable edition (M16, docs/PORTABLE.md): a .tar.gz with "Play KUF" (the launcher, built against the
# Steam Runtime 3 sysroot like the setup), GameFiles/ and Runtime/ (a release's game package, unchanged: runtime
# libraries, code generator, compiler, licences). No game code. Audited (tools/package/audit_portable.py).
#
#   tools/package/make_linux_portable.sh RELEASE_DIR [--launcher FILE]
# RELEASE_DIR: tools/package/make_release.sh's output (its kuf-cod-recomp-<v>-linux-x86_64.tar.gz). Output beside it:
# KUF-Circle-of-Doom-Portable-<v>-linux-x86_64.tar.gz.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REL="$(realpath "${1:?usage: $0 RELEASE_DIR [--launcher FILE]}")"; shift
LAUNCHER="$ROOT/build/installer/kuf-launcher"
[[ "${1:-}" == "--launcher" ]] && LAUNCHER="$(realpath "$2")"
PKG="$(ls "$REL"/kuf-cod-recomp-*-linux-x86_64.tar.gz)"
V="$(basename "$PKG" | sed -E 's/^kuf-cod-recomp-(.*)-linux-x86_64\.tar\.gz$/\1/')"
NAME="KUF-Circle-of-Doom-Portable"
W="$REL/portable-linux"; D="$W/$NAME"
rm -rf "$W"; mkdir -p "$D/Runtime" "$D/GameFiles"
tar -xzf "$PKG" -C "$D/Runtime" --strip-components=1
mkdir -p "$D/Runtime/assets"; cp "$ROOT/installer/assets/game_v1.tsv" "$D/Runtime/assets/"
strip -o "$D/Play KUF" "$LAUNCHER"; chmod 755 "$D/Play KUF"
printf 'Copy your own game files into this folder: the folder that contains default.xex, or an Xbox 360 disc image\n(.iso). A retail copy works as it is: its encrypted default.xex is decrypted on this computer. Then start\n"Play KUF".\n' \
  > "$D/GameFiles/Put your game files here.txt"
cat > "$D/README.txt" <<'TXT'
Kingdom Under Fire: Circle of Doom - native Linux port, portable edition (experimental)

1. Copy your own game files into the GameFiles folder (the folder with default.xex, or an .iso disc image; a retail
   copy works as it is, its encrypted default.xex is decrypted on this computer). Nothing of the game is included.
2. Start "Play KUF" (file manager: double-click, allow running it if asked). The first start prepares the native
   game on this computer (a few minutes); later starts are immediate.

Everything the game writes stays in this folder (Data/). Nothing is installed. 64-bit Linux with glibc 2.31 or newer
and a Vulkan driver; on a Steam Deck use Desktop Mode, or add "Play KUF" to Steam as a non-Steam game.
Licences: Runtime/licenses. This is an unofficial fan project.
TXT
python3 -I "$ROOT/tools/package/audit_portable.py" "$D" --nm nm | tee "$W/audit.txt" | tail -3
grep -q '^AUDIT: clean$' "$W/audit.txt" || { echo "audit failed: $W/audit.txt" >&2; exit 1; }
OUT="$REL/$NAME-$V-linux-x86_64.tar.gz"
tar -C "$W" --owner=0 --group=0 --numeric-owner -czf "$OUT" "$NAME"
(cd "$REL" && sha256sum "$(basename "$OUT")" && ls -la "$(basename "$OUT")")
