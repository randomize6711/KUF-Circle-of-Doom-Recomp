#!/usr/bin/env bash
# Stage the user's executable for a known game version and build the analysis artefacts.
# The original game folder is only read (one cp of default.xex); everything is written under build/.
#
#   build/input/<ver>/default.xex      read-only copy (codegen input); the game folder holds an unencrypted one
#   build/analysis/<ver>/image.bin     decompressed memory image (tools/xeximage.py), SHA-256 verified
#   build/analysis/<ver>/image.elf     the image wrapped as ELF for llvm-objdump
#   build/analysis/<ver>/flat.dis      full disassembly of all code sections
#
# Usage: tools/stage_input.sh [version]     (default v1; KUF_GAME_DIR overrides the game folder)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1:-v1}"
GAME_DIR="${KUF_GAME_DIR:-$(cat "$ROOT/.game_dir" 2>/dev/null || true)}"  # your game folder (.game_dir: not committed)
[ -n "$GAME_DIR" ] || { echo "set KUF_GAME_DIR, or write your game folder's path into $ROOT/.game_dir" >&2; exit 1; }

declare -A SHA256=(
  [v1]=c502f6617bf1ef1112474d6eb1de5bb6752625f0b7688691c1f8ce0c50c90627  # the memory image of media 7092B5EC, version 1
)
want="${SHA256[$VER]:-}"
[ -n "$want" ] || { echo "unknown version '$VER' (known: ${!SHA256[*]})" >&2; exit 1; }

IN="$ROOT/build/input/$VER"
AN="$ROOT/build/analysis/$VER"
mkdir -p "$IN" "$AN"
if [ ! -f "$IN/default.xex" ]; then
  cp "$GAME_DIR/default.xex" "$IN/default.xex.tmp"
  chmod a-w "$IN/default.xex.tmp"
  mv "$IN/default.xex.tmp" "$IN/default.xex"
fi
python3 -I "$ROOT/tools/xeximage.py" "$IN/default.xex" "$AN" > "$AN/xeximage.txt"
got="$(sha256sum "$AN/image.bin" | cut -d' ' -f1)"
[ "$got" = "$want" ] || { echo "SHA-256 mismatch for $VER: $got" >&2; exit 1; }
llvm-objcopy -I binary -O elf32-powerpc --rename-section .data=.text,code,alloc,load,readonly \
  "$AN/image.bin" "$AN/image.elf"
llvm-objcopy --change-section-address .text=0x82000000 "$AN/image.elf" "$AN/image.elf"
# All executable sections: .text through the last .embsec_.
llvm-objdump -d --no-show-raw-insn --start-address=0x82150000 --stop-address=0x82a71200 "$AN/image.elf" \
  | tail -n +7 | sed 's/ <_binary[^>]*>//' > "$AN/flat.dis"
echo "staged $VER: $(wc -l < "$AN/flat.dis") instructions disassembled into $AN"
