#!/usr/bin/env bash
# Publishes a revision of this repository to the public GitHub repository as one new commit (the public history is
# separate: the local commits, their author address and the local paths are not published).
#
#   tools/publish_public.sh --public-dir DIR [--rev REV] [--message TEXT] [--push]
#
# DIR is a clone of the public repository (created once with `gh repo clone`). The revision's tracked files replace
# DIR's (the SDK submodule as a link to the same pinned upstream commit); the commit's author is the GitHub account's
# noreply address (gh api user). Refused, with nothing committed, when the revision contains game-derived files
# (tools/check_tracked.py) or this machine's home path or git e-mail address. Without --push it only commits.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PUBLIC=""
REV="HEAD"
MESSAGE=""
PUSH=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --public-dir) PUBLIC="$(realpath -e "$2")"; shift 2 ;;
    --rev) REV="$2"; shift 2 ;;
    --message) MESSAGE="$2"; shift 2 ;;
    --push) PUSH=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[[ -n "$PUBLIC" && -d "$PUBLIC/.git" ]] || { echo "--public-dir must be a clone of the public repository" >&2; exit 2; }
[[ "$PUBLIC" != "$ROOT" ]] || { echo "--public-dir is this repository" >&2; exit 2; }
SHA="$(git -C "$ROOT" rev-parse --verify "$REV^{commit}")"
(cd "$ROOT" && python3 -I tools/check_tracked.py "$SHA") >/dev/null || { echo "game-derived files in $REV: not published" >&2; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
git -C "$ROOT" archive "$SHA" | tar -x -C "$STAGE"
EMAIL="$(git -C "$ROOT" config user.email || true)"
for needle in "$HOME" ${EMAIL:+"$EMAIL"}; do
  if grep -rIlF -- "$needle" "$STAGE" | head -5 | grep -q .; then
    echo "personal data ('$needle') in:" >&2
    grep -rIlF -- "$needle" "$STAGE" | sed "s|$STAGE/|  |" >&2
    exit 1
  fi
done

# The public tree: everything but .git replaced by the revision's files.
find "$PUBLIC" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp -a "$STAGE/." "$PUBLIC/"
SDK="$(git -C "$ROOT" ls-tree "$SHA" thirdparty/rexglue-sdk | awk '{print $3}')"
cd "$PUBLIC"
git add -A
git rm -q --cached -r --ignore-unmatch thirdparty/rexglue-sdk >/dev/null
[[ -n "$SDK" ]] && git update-index --add --cacheinfo "160000,$SDK,thirdparty/rexglue-sdk"
if git diff --cached --quiet; then
  echo "nothing to publish (the public tree already matches $REV)"
  exit 0
fi
LOGIN="$(gh api user --jq .login)"
ID="$(gh api user --jq .id)"
AUTHOR="$LOGIN <$ID+$LOGIN@users.noreply.github.com>"
DESCRIBE="$(git -C "$ROOT" log -1 --format=%s "$SHA")"
git -c user.name="$LOGIN" -c user.email="$ID+$LOGIN@users.noreply.github.com" commit -q --author="$AUTHOR" \
  -m "${MESSAGE:-$DESCRIBE}" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
echo "committed $(git rev-parse --short HEAD) in $PUBLIC"
if [[ $PUSH -eq 1 ]]; then
  git push origin HEAD
fi
