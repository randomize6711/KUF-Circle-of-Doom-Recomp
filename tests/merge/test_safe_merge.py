#!/usr/bin/env python3
"""Self-test of tools/safe_merge.sh (the fail-closed milestone merge): the real script runs in a throwaway
repository (with an SDK submodule and a patch) whose build, ctest, game-folder and tracked-content steps are
stubs on PATH / in tools/. Each case injects one failure and checks that the script printed MERGE ABORTED,
exited non-zero and left main where it was; the last case passes every gate and must fast-forward main.
Never touches this repository. Exit status 0 = every case behaved as expected.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GIT = ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "protocol.file.allow=always",
       "-c", "init.defaultBranch=main", "-c", "commit.gpgsign=false"]
PASS_SUMMARY = "100% tests passed out of 1700"


def sh(cwd, *args, check=True):
    return subprocess.run(list(args), cwd=cwd, check=check, capture_output=True, text=True)


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)


def make_repo(base):
    """A repository shaped like this one: tools/safe_merge.sh, an SDK submodule with one patch applied."""
    sdk_src = os.path.join(base, "sdk-src")
    os.makedirs(sdk_src)
    sh(sdk_src, *GIT, "init", "-q")
    write(os.path.join(sdk_src, "a.txt"), "original\n")
    sh(sdk_src, *GIT, "add", "a.txt")
    sh(sdk_src, *GIT, "commit", "-qm", "sdk")

    repo = os.path.join(base, "repo")
    os.makedirs(repo)
    sh(repo, *GIT, "init", "-q")
    sh(repo, *GIT, "submodule", "add", "-q", sdk_src, "thirdparty/rexglue-sdk")
    write(os.path.join(repo, "patches/rexglue-sdk/0001-test.patch"),
          "--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-original\n+patched\n")
    os.makedirs(os.path.join(repo, "tools"))
    shutil.copy(os.path.join(ROOT, "tools", "safe_merge.sh"), os.path.join(repo, "tools", "safe_merge.sh"))
    exe = 0o755
    write(os.path.join(repo, "tools/apply_sdk_patches.sh"),
          "#!/bin/sh\ngrep -q patched thirdparty/rexglue-sdk/a.txt\n", exe)
    write(os.path.join(repo, "tools/verify_game_dir.sh"),
          '#!/bin/sh\n[ ! -e "$STUB_DIR/game_dir_changed" ]\n', exe)
    write(os.path.join(repo, "tools/check_tracked.py"),
          "import os, sys\nsys.exit(1 if os.path.exists(os.path.join(os.environ['STUB_DIR'], 'tracked_bad')) else 0)\n")
    write(os.path.join(repo, ".gitignore"), "build/\n")
    sh(repo, *GIT, "add", "-A")
    sh(repo, *GIT, "commit", "-qm", "base")
    write(os.path.join(repo, "thirdparty/rexglue-sdk/a.txt"), "patched\n")  # the patch, applied
    sh(repo, *GIT, "checkout", "-qb", "m9/feature")
    write(os.path.join(repo, "feature.txt"), "work\n")
    sh(repo, *GIT, "add", "feature.txt")
    sh(repo, *GIT, "commit", "-qm", "feature")
    return repo


def make_stubs(stub_dir):
    """ninja/systemd-run succeed (or fail on request); ctest prints $STUB_DIR/ctest_output, exits with
    $STUB_DIR/ctest_rc, and runs $STUB_DIR/ctest_side_effect if present."""
    exe = 0o755
    write(os.path.join(stub_dir, "bin/systemd-run"),
          '#!/bin/sh\nwhile [ "${1#-}" != "$1" ]; do case "$1" in -p) shift 2;; *) shift;; esac; done\nexec "$@"\n',
          exe)
    write(os.path.join(stub_dir, "bin/ninja"), '#!/bin/sh\n[ ! -e "$STUB_DIR/build_fails" ]\n', exe)
    write(os.path.join(stub_dir, "bin/ctest"),
          '#!/bin/sh\n[ -x "$STUB_DIR/ctest_side_effect" ] && "$STUB_DIR/ctest_side_effect"\n'
          'cat "$STUB_DIR/ctest_output"\nexit "$(cat "$STUB_DIR/ctest_rc")"\n', exe)


CASES = [
    # name, setup(repo, stub_dir), expect_merge
    ("dirty tree", lambda r, s: write(os.path.join(r, "feature.txt"), "uncommitted\n"), False),
    ("SDK patch missing", lambda r, s: write(os.path.join(r, "thirdparty/rexglue-sdk/a.txt"), "original\n"), False),
    ("build fails", lambda r, s: write(os.path.join(s, "build_fails"), ""), False),
    ("a test fails", lambda r, s: (write(os.path.join(s, "ctest_output"),
                                         "86% tests passed, 2 tests failed out of 14\n"),
                                   write(os.path.join(s, "ctest_rc"), "8\n")), False),
    ("exit 0 but no summary", lambda r, s: write(os.path.join(s, "ctest_output"), "Total Test time 1 sec\n"), False),
    ("a test not run", lambda r, s: write(os.path.join(s, "ctest_output"),
                                          "13/14 Test #9: x ...***Not Run   0.00 sec\n" + PASS_SUMMARY + "\n"), False),
    ("too few tests", lambda r, s: write(os.path.join(s, "ctest_output"), "100% tests passed out of 12\n"), False),
    ("HEAD moves during the tests", lambda r, s: write(
        os.path.join(s, "ctest_side_effect"),
        "#!/bin/sh\n" + " ".join(GIT) + " commit -q --allow-empty -m during >/dev/null\n", 0o755), False),
    ("tree changes during the tests", lambda r, s: write(
        os.path.join(s, "ctest_side_effect"), "#!/bin/sh\necho x >> feature.txt\n", 0o755), False),
    ("game folder changed", lambda r, s: write(os.path.join(s, "game_dir_changed"), ""), False),
    ("game content tracked", lambda r, s: write(os.path.join(s, "tracked_bad"), ""), False),
    ("every gate passes", lambda r, s: None, True),
]


def run_case(name, setup, expect_merge):
    with tempfile.TemporaryDirectory(prefix="safe-merge-test-") as base:
        repo = make_repo(base)
        stub_dir = os.path.join(base, "stubs")
        make_stubs(stub_dir)
        write(os.path.join(stub_dir, "ctest_output"), PASS_SUMMARY + "\n")
        write(os.path.join(stub_dir, "ctest_rc"), "0\n")
        setup(repo, stub_dir)
        main_before = sh(repo, "git", "rev-parse", "main").stdout.strip()
        branch_head = sh(repo, "git", "rev-parse", "m9/feature").stdout.strip()
        env = dict(os.environ, STUB_DIR=stub_dir, PATH=os.path.join(stub_dir, "bin") + os.pathsep + os.environ["PATH"],
                   GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="protocol.file.allow", GIT_CONFIG_VALUE_0="always")
        p = subprocess.run(["bash", "tools/safe_merge.sh", "m9/feature"], cwd=repo, env=env, capture_output=True,
                           text=True, timeout=120)
        main_after = sh(repo, "git", "rev-parse", "main").stdout.strip()
        out = p.stdout + p.stderr
        if expect_merge:
            ok = p.returncode == 0 and "MERGE DONE" in out and main_after == branch_head
        else:
            ok = p.returncode != 0 and "MERGE ABORTED" in out and main_after == main_before
        reason = next((l for l in out.splitlines() if "MERGE ABORTED" in l or "MERGE DONE" in l), "(no verdict)")
        print(f"[{'ok' if ok else 'FAIL'}] {name}: exit {p.returncode}, {reason}")
        if not ok:
            print(out[-2000:])
        return ok


def main():
    results = [run_case(*c) for c in CASES]
    print(f"{sum(results)}/{len(results)} cases as expected")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
