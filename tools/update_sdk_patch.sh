#!/usr/bin/env bash
# Regenerate one patch in patches/rexglue-sdk from the SDK submodule's working tree, keeping its description.
# The files are those the patch already touches (its "+++ b/" lines), plus any given as arguments. Files new
# in the SDK are written as additions. Only valid for files no other patch touches (checked).
#
#   tools/update_sdk_patch.sh patches/rexglue-sdk/0005-....patch [new/file ...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/thirdparty/rexglue-sdk"
PATCH="$(realpath "$1")"
shift
mapfile -t FILES < <({ sed -n 's|^+++ b/||p' "$PATCH"; printf '%s\n' "$@"; } | grep -v '^$' | sort -u)
for other in "$ROOT"/patches/rexglue-sdk/*.patch; do
  [ "$other" = "$PATCH" ] && continue
  for f in "${FILES[@]}"; do
    if grep -qx "+++ b/$f" "$other"; then
      echo "$f is also changed by $(basename "$other"): edit that pair of patches by hand" >&2; exit 1
    fi
  done
done
G=(git -C "$SDK" -c diff.mnemonicPrefix=false -c diff.noprefix=false)
TMP="$(mktemp)"
sed '/^diff --git /,$d' "$PATCH" > "$TMP"  # the description
for f in "${FILES[@]}"; do
  if "${G[@]}" ls-files --error-unmatch -- "$f" > /dev/null 2>&1; then
    "${G[@]}" diff -- "$f" >> "$TMP"
  else
    (cd "$SDK" && "${G[@]}" diff --no-index -- /dev/null "$f" >> "$TMP" || true)
  fi
done
mv "$TMP" "$PATCH"
git -C "$SDK" apply --check --reverse "$PATCH"
echo "updated $(basename "$PATCH"): ${#FILES[@]} files; reverse-applies cleanly"
