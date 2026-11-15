#!/usr/bin/env bash
# tools/windows/replay_recipe.sh: after tools/package/make_builder_windows.py --out build/m16-winpkg; then compare the
# objects with tools/package/function_hashes.py (docs/WINDOWS.md "The Windows local build").
# Replays the Windows builder's recipe on Linux (the same llvm-mingw release, Linux-hosted clang in place of
# clang-23.exe) through kuf-localbuild, to check the recipe and the builder's files. Not a Windows test.
set -euo pipefail

R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; L=$R/build/toolchains/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64
S=$R/build/m16-sim; rm -rf "$S"; mkdir -p "$S/pkg/bin"
cp -al "$R/build/m16-winpkg/builder" "$S/pkg/builder"
# The Linux stand-ins for the Windows compiler and linker (layout as llvm-mingw: bin/, lib/).
rm "$S/pkg/builder/toolchain/bin/clang-23.exe" "$S/pkg/builder/toolchain/bin/ld.lld.exe"
cp "$L/bin/clang-23" "$S/pkg/builder/toolchain/bin/clang-23.exe"
cp "$L/bin/lld" "$S/pkg/builder/toolchain/bin/lld"; ln -s lld "$S/pkg/builder/toolchain/bin/ld.lld"
# A Windows-hosted clang also searches <root>/include (it is not cross-compiling); the Linux stand-in only
# <root>/x86_64-w64-mingw32/include: the same folder, linked, for this replay only.
ln -s ../include "$S/pkg/builder/toolchain/x86_64-w64-mingw32/include"
mkdir -p "$S/pkg/builder/toolchain/lib"
for f in "$L"/lib/libclang-cpp.so* "$L"/lib/libLLVM.so*; do ln -sf "$f" "$S/pkg/builder/toolchain/lib/"; done
# The code generator: the Linux one (its output is the same C++).
cp "$R/build/linux-amd64-relwithdebinfo/bin/rexgluerd" "$R"/build/linux-amd64-relwithdebinfo/bin/lib*.so "$S/pkg/bin/"
cd "$S" && nice -n 5 "$R/build/installer/kuf-localbuild" --package "$S/pkg" --xex "$R/build/game-plain/default.xex" \
  --cache "$S/cache" --output "$S/out/kuf_cod.exe" --jobs 4 > "$S/localbuild.log" 2>&1
echo "kuf-localbuild exit $?"
