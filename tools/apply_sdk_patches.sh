#!/usr/bin/env bash
# Apply our patches to the pinned ReXGlue SDK submodule (idempotent).
# Patches live in patches/rexglue-sdk/*.patch, in order. `--check` only reports the state (exit 1 if any
# patch is missing); CMake runs it at configure time so a build never silently lacks a patch.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/thirdparty/rexglue-sdk"
CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1
missing=0
# The SDK's console-key tables are deleted by pattern, before the patches, so that no patch has to quote them
# (docs/LOCAL_BUILD.md "Decryption keys"); patch 0019 removes the code that used them. The upstream files are unchanged.
for spec in 'src/system/xex_module.cpp|xe_xex2_[a-z]*_key\[16\] *=' 'src/kernel/xboxkrnl/xboxkrnl_crypt.cpp|uint8_t key19\[\] *='; do
  f="$SDK/${spec%%|*}"; rx="${spec#*|}"
  if grep -q "$rx" "$f"; then
    if [ "$CHECK" = 1 ]; then
      echo "MISSING:  console keys still in ${spec%%|*}"; missing=1
    else
      sed -i "/$rx/,/};/d" "$f"
      echo "removed:  console keys from ${spec%%|*}"
    fi
  fi
done
for p in "$ROOT"/patches/rexglue-sdk/*.patch; do
  [ -e "$p" ] || continue
  if git -C "$SDK" apply --check --reverse "$p" 2>/dev/null; then
    echo "applied:  $(basename "$p")"
  elif [ "$CHECK" = 1 ]; then
    echo "MISSING:  $(basename "$p")"; missing=1
  else
    git -C "$SDK" apply "$p"
    echo "applying: $(basename "$p")"
  fi
done
exit $missing
