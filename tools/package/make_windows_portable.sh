#!/usr/bin/env bash
# The Windows portable edition (M16, docs/PORTABLE.md, docs/WINDOWS.md): a ZIP with Play KUF.exe, GameFiles/ and
# Runtime/ (the runtime DLLs, the code generator, the compiler for the local build, licences). No game code: the game
# is generated and compiled on the player's PC on first launch. Audited (tools/package/audit_portable.py).
#
#   tools/package/make_windows_portable.sh VERSION [--out DIR]
# Needs: build/mingw-amd64-relwithdebinfo (the game build, preset mingw-amd64-relwithdebinfo), build/installer-mingw
# (Play KUF.exe), the llvm-mingw release (tools/toolchain/setup_llvm_mingw.sh). Output: <out>/
# KUF-Circle-of-Doom-Portable-<VERSION>-windows-x86_64.zip (and the unpacked folder beside it, for tests).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${1:?usage: $0 VERSION [--out DIR]}"; shift
OUT="$ROOT/build/releases-local/windows-$VERSION"
[[ "${1:-}" == "--out" ]] && OUT="$2"
B="$ROOT/build/mingw-amd64-relwithdebinfo"
SETUP="$ROOT/build/installer-mingw"
LH="$ROOT/build/toolchains/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64"
LW="$ROOT/build/toolchains/llvm-mingw-20261006-ucrt-x86_64"
PY=python3
# On a Windows host (Git Bash) the Windows llvm-mingw does the host's work too, and Python is python.
case "$(uname -s)" in MINGW*|MSYS*) LH="$LW"; PY=python ;; esac
NAME="KUF-Circle-of-Doom-Portable"
D="$OUT/$NAME"
rm -rf "$OUT"; mkdir -p "$D/Runtime/bin" "$D/Runtime/assets" "$D/Runtime/licenses" "$D/GameFiles"
STRIP() { "$LH/bin/llvm-objcopy" --strip-debug "$1" "$2"; }
for f in librexruntimerd.dll rexgpu-xenosrd.dll libTracyClientrd.dll rexgluerd.exe; do STRIP "$B/bin/$f" "$D/Runtime/bin/$f"; done
cp "$LW/x86_64-w64-mingw32/bin/libc++.dll" "$LW/x86_64-w64-mingw32/bin/libunwind.dll" "$D/Runtime/bin/"
$PY -I "$ROOT/tools/package/make_builder_windows.py" --build "$B" --out "$D/Runtime" --llvm-mingw-host "$LH" \
  --llvm-mingw-windows "$LW" | tail -1
cp "$ROOT/installer/assets/game_v1.tsv" "$D/Runtime/assets/"
printf '%s\n' "$VERSION" > "$D/Runtime/VERSION"
cp "$ROOT/tools/package/THIRD-PARTY-NOTICES.txt" "$D/Runtime/licenses/"
cp -r "$ROOT/tools/package/licenses/." "$D/Runtime/licenses/"
cp "$LW/LICENSE.TXT" "$D/Runtime/licenses/llvm-mingw-LICENSE.txt"
cp "$ROOT/LICENSE" "$D/Runtime/licenses/kuf-cod-recomp-LICENSE.txt"
STRIP "$SETUP/Play KUF.exe" "$D/Play KUF.exe"
# Updates (M17G, docs/UPDATES_WINDOWS.md): the updater, the official release repository and the release key. This
# archive is itself the update package (asset prefix KUF-Circle-of-Doom-Portable).
STRIP "$SETUP/kuf-update.exe" "$D/Runtime/bin/kuf-update.exe"
printf '# Updates (Check for Updates in the game, Runtime/bin/kuf-update.exe): the release repository and key.\r\nrepo = randomize6711/KUF-Circle-of-Doom-Recomp\r\nkey_identity = release@kuf-cod-recomp\r\nasset_prefix = KUF-Circle-of-Doom-Portable\r\nplatform = windows-x86_64\r\n' \
  > "$D/Runtime/update.conf"
cp "$ROOT/tools/package/allowed_signers" "$D/Runtime/allowed_signers"
printf 'Copy your own game files into this folder: the folder that contains default.xex, or an Xbox 360 disc image\r\n(.iso). A retail copy works as it is: its encrypted default.xex is decrypted on this computer. Then start\r\nPlay KUF.exe.\r\n' \
  > "$D/GameFiles/Put your game files here.txt"
cat > "$D/README.txt" <<'EOF'
Kingdom Under Fire: Circle of Doom - native Windows port, portable edition (experimental)

1. Copy your own game files into the GameFiles folder (the folder with default.xex, or an .iso disc image; a retail
   copy works as it is, its encrypted default.xex is decrypted on this computer). Nothing of the game is included.
2. Double-click "Play KUF.exe". The first start prepares the native game on this PC (a few minutes); later starts
   are immediate.

Everything the game writes stays in this folder (Data\). Nothing is installed; no administrator rights, no registry
entries. Windows 10 (version 1809 or newer) or Windows 11, 64-bit, with a Vulkan graphics driver.
Licences: Runtime\licenses. This is an unofficial fan project.
EOF
sed -i 's/$/\r/' "$D/README.txt"
$PY -I "$ROOT/tools/package/audit_portable.py" "$D" --nm "$LH/bin/llvm-nm" | tee "$OUT/audit.txt" | tail -3
grep -q '^AUDIT: clean$' "$OUT/audit.txt" || { echo "audit failed: $OUT/audit.txt" >&2; exit 1; }
if command -v zip > /dev/null; then
  (cd "$OUT" && rm -f "$NAME-$VERSION-windows-x86_64.zip" && zip -qr -9 "$NAME-$VERSION-windows-x86_64.zip" "$NAME")
else  # Git for Windows has no zip
  (cd "$OUT" && rm -f "$NAME-$VERSION-windows-x86_64.zip" && $PY -I -c 'import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, fs in os.walk(sys.argv[2]):
        z.write(d)
        for f in fs: z.write(os.path.join(d, f))' "$NAME-$VERSION-windows-x86_64.zip" "$NAME")
fi
(cd "$OUT" && sha256sum "$NAME-$VERSION-windows-x86_64.zip" > SHA256SUMS && ls -la "$NAME-$VERSION-windows-x86_64.zip")
