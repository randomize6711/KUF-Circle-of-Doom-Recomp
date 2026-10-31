#!/usr/bin/env bash
# Fail-closed milestone merge. Merges a feature branch into main only after every gate passed, all in one
# process with `set -e`: any failing step ends the script before the merge line is reached.
#
#   tools/safe_merge.sh <branch> [--new-branch <name>]
#
# Gates, in order (each aborts on failure):
#   1. working tree clean; the SDK submodule at its pinned commit with exactly our patches applied;
#   2. <branch> is checked out and main is its ancestor (the merge will be a fast-forward);
#   3. both build trees build (relwithdebinfo for ctest, audit for the game runs);
#   4. the full ctest suite passes: every test, no "failed" or "not run"; the test count is printed;
#   5. the commit under test is still HEAD and the tree is still clean (nothing changed during the tests);
#   6. the original game folder is unchanged (tools/verify_game_dir.sh);
#   7. no game-derived content is tracked (tools/check_tracked.py).
# Only then: `git merge --ff-only` into main (never a merge commit, never a history rewrite), and optionally
# create the next milestone branch. The log of the whole procedure goes to build/logs/merge-<branch>.log.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
branch="${1:?usage: tools/safe_merge.sh <branch> [--new-branch <name>]}"
shift
new_branch=""
if [ "${1:-}" = "--new-branch" ]; then new_branch="${2:?--new-branch needs a name}"; fi

mkdir -p build/logs
LOG="build/logs/merge-${branch//\//_}.log"
exec > >(tee "$LOG") 2>&1
fail() { echo "MERGE ABORTED: $*"; exit 1; }
trap 'echo "MERGE ABORTED: unexpected failure at line $LINENO: $BASH_COMMAND"' ERR
step() { echo; echo "== $*"; }

SDK=thirdparty/rexglue-sdk
clean_tree() {
  local status pinned sdk_head stray
  status="$(git status --porcelain=v1 --ignore-submodules=all)"
  [ -z "$status" ] || { echo "$status"; return 1; }
  # The SDK submodule: at the pinned commit, all our patches applied, no other change in it.
  pinned="$(git ls-tree HEAD "$SDK" | awk '{print $3}')"
  sdk_head="$(git -C "$SDK" rev-parse HEAD)"
  [ "$pinned" = "$sdk_head" ] || { echo "SDK at $sdk_head, pinned $pinned"; return 1; }
  tools/apply_sdk_patches.sh --check > /dev/null || { echo "SDK patches missing"; return 1; }
  stray="$(comm -23 <(git -C "$SDK" status --porcelain=v1 --untracked-files=all | cut -c4- | sort -u) \
                    <(cat patches/rexglue-sdk/*.patch | sed -n 's|^+++ b/||p' | sort -u))"
  [ -z "$stray" ] || { echo "SDK changes outside the patches: $stray"; return 1; }
}

step "1. working tree"
clean_tree || fail "working tree is not clean"

step "2. branch and fast-forward"
[ "$(git rev-parse --abbrev-ref HEAD)" = "$branch" ] || fail "check out $branch first"
git merge-base --is-ancestor main "$branch" || fail "main is not an ancestor of $branch (no fast-forward)"
tested="$(git rev-parse HEAD)"
echo "commit under test: $tested ($(git log -1 --format=%s))"

step "3. build"
for tree in linux-amd64-relwithdebinfo linux-amd64-audit; do
  systemd-run --user --scope -p MemoryMax=14G -q ninja -C "build/$tree" || fail "build of $tree failed"
done

step "4. full regression suite"
CTEST_LOG="build/logs/merge-ctest.log"
ctest_rc=0
ctest --test-dir build/linux-amd64-relwithdebinfo -j8 --output-on-failure > "$CTEST_LOG" 2>&1 || ctest_rc=$?
tail -n 25 "$CTEST_LOG"
[ "$ctest_rc" = 0 ] || fail "ctest exit $ctest_rc (details in $CTEST_LOG)"
# ctest prints "100% tests passed out of N", or "P% tests passed, F tests failed out of N".
summary="$(grep -E '^[0-9]+% tests passed' "$CTEST_LOG" | tail -n1 || true)"
if [[ "$summary" =~ ^100%\ tests\ passed(,\ 0\ tests\ failed)?\ out\ of\ ([0-9]+)$ ]]; then
  total="${BASH_REMATCH[2]}"
else
  fail "no 100% summary line in $CTEST_LOG (got: '${summary:-none}')"
fi
! grep -qE '\*\*\*Not Run|\(Disabled\)|\*\*\*Skipped' "$CTEST_LOG" || fail "tests not run, disabled or skipped"
min_tests="${KUF_MIN_TESTS:-1615}"
[ "$total" -ge "$min_tests" ] || fail "only $total tests ran (expected at least $min_tests)"
echo "all $total tests passed"

step "5. nothing changed during the tests"
[ "$(git rev-parse HEAD)" = "$tested" ] || fail "HEAD moved during the tests"
clean_tree || fail "working tree changed during the tests"

step "6. original game folder"
tools/verify_game_dir.sh v1 || fail "game folder changed"

step "7. tracked content"
python3 -I tools/check_tracked.py HEAD || fail "suspicious tracked content"

step "merge $branch into main (fast-forward only)"
git checkout -q main
git merge --ff-only "$tested" || { git checkout -q "$branch"; fail "fast-forward failed"; }
echo "main is now $(git rev-parse --short HEAD)"
if [ -n "$new_branch" ]; then
  git checkout -q -b "$new_branch"
  echo "created and checked out $new_branch"
fi
echo "MERGE DONE"
