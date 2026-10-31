#!/usr/bin/env bash
# Assemble the SDK's PPC instruction test sources with the SDK's prebuilt binutils, inside a bubblewrap
# sandbox: no network, read-only view of the system and the SDK, write access only to the output dir.
# Mirrors cmake/ppc_test_pipeline.cmake (same flags) so the generated tests are identical.
# Usage: tools/ppc_asm_sandboxed.sh <out_dir> [asm_dir]   (asm_dir defaults to the SDK suite)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/thirdparty/rexglue-sdk"
BIN="$SDK/tools/binutils"
OUT="$(realpath -m "$1")"
ASM="$(realpath "${2:-$SDK/tests/ppc/asm}")"
mkdir -p "$OUT/obj" "$OUT/bin"
run() {
  bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
        --ro-bind "$SDK" "$SDK" --ro-bind "$ASM" "$ASM" --bind "$OUT" "$OUT" --proc /proc --dev /dev --tmpfs /tmp \
        --unshare-all --die-with-parent --new-session --clearenv "$@"
}
n=0
for s in "$ASM"/*.s; do
  b=$(basename "$s" .s)
  run "$BIN/powerpc-none-elf-as" -a32 -be -mregnames -mpower7 -maltivec -mvsx -mvmx128 -R \
      -o "$OUT/obj/$b.o" "$s"
  run "$BIN/powerpc-none-elf-ld" -A powerpc:common32 -melf32ppc -EB -nostdlib --oformat=binary \
      -Ttext=0x82010000 -e 0x82010000 -o "$OUT/bin/$b.bin" "$OUT/obj/$b.o"
  run "$BIN/powerpc-none-elf-nm" --numeric-sort "$OUT/obj/$b.o" > "$OUT/bin/$b.map"
  n=$((n+1))
done
echo "assembled $n test sources into $OUT"
