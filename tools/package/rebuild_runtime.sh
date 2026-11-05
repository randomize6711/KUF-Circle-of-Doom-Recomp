#!/usr/bin/env bash
# Rebuilds bin/librexruntimerd.so from the source archive, for example with a modified FFmpeg or libmspack
# (docs/LOCAL_BUILD.md "Replacing the LGPL libraries"). Needs no game file and no generated code; needs Clang, CMake,
# Ninja and the Steam Runtime 3 sysroot (tools/toolchain/setup_sniper.sh).
#   tools/package/rebuild_runtime.sh     output: build/runtime-only/bin/librexruntimerd.so
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
B="$ROOT/build/runtime-only"
cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_FLAGS=-march=x86-64-v2 \
  -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/steamrt3-sniper.cmake" -DREXSDK_DIR="$ROOT/thirdparty/rexglue-sdk" -DKUFCOD_RUNTIME_ONLY=ON
ninja -C "$B" -j"${JOBS:-4}" rexruntime
echo "$B/bin/librexruntimerd.so"
