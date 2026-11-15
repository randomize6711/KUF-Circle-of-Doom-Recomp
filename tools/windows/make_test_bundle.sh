#!/usr/bin/env bash
# The player's private Windows test (M16 Stage 2, docs/WINDOWS.md "Testing on Windows"): the cross-built game with
# its DLLs and a start script, zipped. It CONTAINS THE TRANSLATED GAME CODE (kuf_cod.exe): for the player's own test on
# their own PC only, never published (the release model compiles on the player's PC: docs/LOCAL_BUILD.md).
#
#   tools/windows/make_test_bundle.sh [BUILD_DIR]     default build/mingw-amd64-relwithdebinfo
# Output: build/windows-test/KUF-Windows-Test.zip (nothing of the game files: the player copies them in).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
B="${1:-$ROOT/build/mingw-amd64-relwithdebinfo}"
LM="$ROOT/build/toolchains/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64"
OUT="$ROOT/build/windows-test"; D="$OUT/KUF-Windows-Test"
rm -rf "$D" "$OUT/KUF-Windows-Test.zip"; mkdir -p "$D/bin" "$D/GameFiles" "$D/licenses"
cp "$B/bin/kuf_cod.exe" "$B/bin/"lib*.dll "$D/bin/"
cp "$LM/x86_64-w64-mingw32/bin/libc++.dll" "$LM/x86_64-w64-mingw32/bin/libunwind.dll" "$D/bin/"
[[ -f "$B/bin/kuf_cod.toml" ]] && cp "$B/bin/kuf_cod.toml" "$D/bin/"
cp "$LM/LICENSE.TXT" "$D/licenses/llvm-mingw-LICENSE.txt" 2>/dev/null || true
cp "$ROOT/tools/package/THIRD-PARTY-NOTICES.txt" "$D/licenses/" 2>/dev/null || true
printf 'Copy your game files here: the folder that contains an UNENCRYPTED default.xex, with all its files.\r\n' \
  > "$D/GameFiles/Put your game files here.txt"
# The start script: every path relative to this folder (spaces and non-ASCII names work: quoted, %~dp0).
cat > "$D/Run KUF test.cmd" <<'CMD'
@echo off
setlocal
set "HERE=%~dp0"
set "DATA=%HERE%data"
if not exist "%HERE%GameFiles\default.xex" (
  echo No default.xex in "%HERE%GameFiles". Copy your game files there first.
  pause
  exit /b 1
)
mkdir "%DATA%\user" "%DATA%\saves" "%DATA%\cache" "%DATA%\logs" 2>nul
"%HERE%bin\kuf_cod.exe" --gpu_plugin=xenos --game_data_root="%HERE%GameFiles" --user_data_root="%DATA%\user" --content_root="%DATA%\saves" --cache_root="%DATA%\cache" --log_file="%DATA%\logs\kuf_cod.log" %*
echo The game exited with code %ERRORLEVEL%. The log is "%DATA%\logs\kuf_cod.log".
pause
CMD
sed -i 's/$/\r/' "$D/Run KUF test.cmd"
cp "$ROOT/docs/WINDOWS_TEST.md" "$D/README-TEST.md"
(cd "$OUT" && zip -qr KUF-Windows-Test.zip KUF-Windows-Test)
ls -la "$OUT/KUF-Windows-Test.zip"
