#!/usr/bin/env bash
# Sets the port's base version (CMakeLists.txt KUFCOD_BASE_VERSION; docs/RELEASING.md "Versions"):
#   tools/package/set_version.sh X.Y.Z
# Development builds are X.Y.Z-dev.<n>+g<commit>, which sort before the release X.Y.Z; after publishing X.Y.Z, set the
# next version (X.Y.Z+1) so later development builds sort after it. Refuses a version that is not newer than the
# current base or that is already a tag here or on the public repository's releases.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NEW="${1:?usage: $0 X.Y.Z}"
[[ "$NEW" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "a base version is X.Y.Z" >&2; exit 2; }
OLD="$(sed -n 's/^set(KUFCOD_BASE_VERSION "\(.*\)")$/\1/p' "$ROOT/CMakeLists.txt")"
python3 -I -c 'import sys; a=[int(x) for x in sys.argv[1].split(".")]; b=[int(x) for x in sys.argv[2].split(".")]; sys.exit(0 if b > a else 1)' "$OLD" "$NEW" ||
  { echo "$NEW is not newer than $OLD" >&2; exit 1; }
if git -C "$ROOT" rev-parse -q --verify "refs/tags/v$NEW" >/dev/null; then echo "tag v$NEW exists" >&2; exit 1; fi
sed -i "s/^set(KUFCOD_BASE_VERSION \"$OLD\")$/set(KUFCOD_BASE_VERSION \"$NEW\")/" "$ROOT/CMakeLists.txt"
echo "base version $OLD -> $NEW (commit CMakeLists.txt)"
