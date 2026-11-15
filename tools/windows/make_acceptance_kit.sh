#!/usr/bin/env bash
# The Windows acceptance kit (M17, docs/WINDOWS_TEST.md): everything the player's Windows PC needs to run the
# acceptance test (tools/windows/acceptance/Run-Acceptance.ps1) and, for Claude on Windows, to fix what fails.
#
#   tools/windows/make_acceptance_kit.sh VERSION OUT_DIR       e.g. 0.9.0-dev.m17d "~/Documents/KUF Windows port/M17-accept"
#
# OUT_DIR (new) gets:
#   Run-Acceptance.cmd, START_HERE.md, WINDOWS_TEST.md
#   packages/        the portable ZIP and the setup .exe of VERSION (built now from the current trees)
#   acceptance/      the test: Run-Acceptance.ps1, KufAcceptance.cs, plan.json (the game scenarios), scripts/,
#                    fixtures/ (the test saves the scenarios start with: build/m6-saves, the player's own)
#   update-test/     a local release VERSION.1 of the same build, signed with a throwaway key made now, and that key's
#                    allowed_signers (the test puts it into its own copy only)
#   native-tests/    the networking tests and the lobby peer as Windows programs
#   source/kuf-cod-recomp   the repository at this commit, the SDK with its history (the build reads the retail key from
#                    it), no remotes: for the native build test (E1) and for Claude on Windows
# PRIVATE: the source copy and the test saves are for the player's own PC only (never published, never pushed).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
V="${1:?usage: $0 VERSION OUT_DIR}"
OUT="${2:?usage: $0 VERSION OUT_DIR}"
[[ -e "$OUT" ]] && { echo "$OUT exists: give a new folder" >&2; exit 1; }
cd "$ROOT"
LW="$ROOT/build/toolchains/llvm-mingw-20261006-ucrt-x86_64/x86_64-w64-mingw32/bin"
SCENARIOS=(
  tests/menu/keyboard_mouse.toml tests/menu/quit_popup.toml tests/menu/pointer_sdl.toml
  tests/menu/settings_tour.toml tests/menu/settings_tour_ingame.toml tests/settings/controls_screen.toml
  tests/menu/ingame_mouse.toml tests/gameplay/keyboard_move.toml tests/stability/framerate_switch.toml
  tests/save/save_new.toml tests/save/save_continue.toml tests/save/save_reload.toml
  tests/windows/no_audio_device.toml tests/windows/display_modes_native.toml
  tests/windows/focus_native.toml tests/windows/focus_native_fullscreen.toml
)

echo "== builds"
ninja -C build/installer-mingw > /dev/null
nice ninja -C build/mingw-amd64-relwithdebinfo kuf_cod rexglue > /dev/null
nice ninja -C build/mingw-tests kuf_net_tests kuf_lobby_peer > /dev/null

echo "== packages $V"
tools/package/make_windows_portable.sh "$V" > /dev/null
PD="build/releases-local/windows-$V"
Z="$PD/KUF-Circle-of-Doom-Portable-$V-windows-x86_64.zip"
mkdir -p "$OUT/packages"
cp "$Z" "$OUT/packages/"
tools/package/make_windows_setup.sh "$PD/KUF-Circle-of-Doom-Portable" "$OUT/packages/KUF-Circle-of-Doom-Setup-$V-windows-x86_64.exe" > /dev/null

echo "== the test"
python3 -I tools/windows/acceptance/make_plan.py "$OUT/acceptance" "${SCENARIOS[@]}"
cp tools/windows/acceptance/Run-Acceptance.ps1 tools/windows/acceptance/KufAcceptance.cs "$OUT/acceptance/"
printf '@echo off\r\nrem The Windows acceptance test (START_HERE.md). A normal window, not as administrator.\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%%~dp0acceptance\\Run-Acceptance.ps1" %%*\r\npause\r\n' \
  > "$OUT/Run-Acceptance.cmd"

echo "== a signed local update release ($V.1, throwaway key)"
T="$(mktemp -d "${TMPDIR:-/tmp}/kuf-kit.XXXXXX")"
trap 'rm -rf "$T"' EXIT
U="$OUT/update-test"; R="$U/releases"
mkdir -p "$R"
V1="$V.1"; NAME="KUF-Circle-of-Doom-Portable-$V1-windows-x86_64"
python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$Z" "$T/tree"
printf '%s\n' "$V1" > "$T/tree/KUF-Circle-of-Doom-Portable/Runtime/VERSION"
(cd "$T/tree" && python3 -I -c 'import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    for d, ds, fs in os.walk("KUF-Circle-of-Doom-Portable"):
        ds.sort()
        for f in sorted(fs): z.write(os.path.join(d, f))' "$R/$NAME.zip")
printf 'Acceptance test release (local, not published).\n' > "$T/summary.txt"
python3 -I tools/package/make_manifest.py "$R/$NAME.zip" --summary "$T/summary.txt" > /dev/null
ssh-keygen -q -t ed25519 -N '' -C acceptance-throwaway -f "$T/key" < /dev/null > /dev/null
ssh-keygen -q -Y sign -f "$T/key" -n kuf-cod-recomp-release "$R/$NAME.zip" < /dev/null > /dev/null
ssh-keygen -q -Y sign -f "$T/key" -n kuf-cod-recomp-manifest "$R/$NAME.update.json" < /dev/null > /dev/null
printf 'release@kuf-cod-recomp namespaces="kuf-cod-recomp-release,kuf-cod-recomp-manifest" %s\n' "$(cut -d' ' -f1,2 "$T/key.pub")" \
  > "$U/allowed_signers"
python3 -I - "$R" "$V1" <<'PY'
import json, os, sys
d, v = sys.argv[1], sys.argv[2]
assets = [{"name": n, "size": os.path.getsize(os.path.join(d, n))} for n in sorted(os.listdir(d)) if n != "releases.json"]
json.dump([{"tag_name": "v" + v, "html_url": "https://example.invalid/local-acceptance-release", "published_at":
            "2026-11-10T00:00:00Z", "draft": False, "prerelease": False, "assets": assets}],
          open(os.path.join(d, "releases.json"), "w"), indent=1)
PY

echo "== Windows test programs"
mkdir -p "$OUT/native-tests"
cp build/mingw-tests/tests/kuf_net_tests.exe build/mingw-tests/tests/kuf_lobby_peer.exe "$LW/libc++.dll" "$LW/libunwind.dll" \
  "$OUT/native-tests/"

echo "== the repository (no remotes)"
S="$OUT/source/kuf-cod-recomp"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
git clone --quiet --no-hardlinks --branch "$BRANCH" "$ROOT" "$S"
git -C "$S" remote remove origin
git -C "$S" config core.autocrlf false
git -C "$S" submodule init --quiet
git -C "$S" config submodule.thirdparty/rexglue-sdk.url "$ROOT/.git/modules/thirdparty/rexglue-sdk"
git -C "$S" -c protocol.file.allow=always submodule update --quiet thirdparty/rexglue-sdk
git -C "$S/thirdparty/rexglue-sdk" remote remove origin
git -C "$S" config submodule.thirdparty/rexglue-sdk.url https://github.com/rexglue/rexglue-sdk
git -C "$S/thirdparty/rexglue-sdk" show HEAD:src/system/xex_module.cpp | grep -q "xe_xex2_retail_key" \
  || { echo "the SDK copy lacks the history the build needs" >&2; exit 1; }
[[ -z "$(git -C "$S" remote)" && -z "$(git -C "$S/thirdparty/rexglue-sdk" remote)" ]]

echo "== documents"
cp docs/WINDOWS_TEST.md "$OUT/"
cp tools/windows/acceptance/START_HERE.md "$OUT/"
printf 'commit %s\nbranch %s\nversion %s\nmade %s\n' "$(git rev-parse HEAD)" "$BRANCH" "$V" "$(date -Iseconds)" > "$OUT/KIT.txt"
(cd "$OUT" && find packages acceptance update-test native-tests Run-Acceptance.cmd START_HERE.md WINDOWS_TEST.md -type f -print0 \
  | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS)
du -sh "$OUT" | sed 's/^/kit: /'
echo "commit $(git rev-parse --short HEAD) on $BRANCH, version $V"
