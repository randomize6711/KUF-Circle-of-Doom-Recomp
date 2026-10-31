#!/usr/bin/env python3
"""Sandboxed screenshot regression test (M3): run the game, capture boot and title-loop frames, validate them.

Usage: capture_test.py [--preset linux-amd64-relwithdebinfo] [--seconds 40] [--run-id capture-<time>]
                       [--references build/references/v1-title] [--baseline]

1. tools/run_sandboxed.sh --gpu with --kuf_capture_frames=<FRAMES> and a timed exit.
2. tools/capture_check.py check against tests/capture/title_v1.toml (committed statistics).
3. If local reference captures exist, tools/capture_check.py compare against them. They are game content
   and live only under build/ (create them once with --baseline after visually confirming a run).
Exit status 0 = PASS. Reports: <run>/user/captures/{capture,compare}-report.txt.
"""
import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAMES = "60,300,360,420,480,540,600,660,720,780,840,900"
EXPECTATIONS = os.path.join(ROOT, "tests", "capture", "title_v1.toml")
CHECKER = os.path.join(ROOT, "tools", "capture_check.py")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="linux-amd64-relwithdebinfo")
    ap.add_argument("--seconds", type=int, default=40)
    ap.add_argument("--run-id", default="capture-" + time.strftime("%Y%m%d-%H%M%S"))
    ap.add_argument("--references", default=os.path.join(ROOT, "build", "references", "v1-title"))
    ap.add_argument("--baseline", action="store_true", help="store this run's captures as the local references")
    args = ap.parse_args()

    run_dir = os.path.join(ROOT, "build", "runs", args.run_id)
    cap_dir = os.path.join(run_dir, "user", "captures")
    subprocess.run([os.path.join(ROOT, "tools", "run_sandboxed.sh"), "-p", args.preset, "-t", str(args.seconds + 60),
                    "-n", args.run_id, "--gpu", "--", f"--kuf_exit_after_seconds={args.seconds}",
                    f"--kuf_capture_frames={FRAMES}",
                    "--kuf_button_prompts=controller"], check=False)  # M7: the references show the original icons
    status = int(open(os.path.join(run_dir, "exit_status")).read().strip())
    if status != 0:
        print(f"CAPTURE TEST: FAIL (game exit status {status}; see {run_dir})")
        return 1

    py = [sys.executable, "-I", CHECKER]
    failed = subprocess.run(py + ["check", cap_dir, EXPECTATIONS]).returncode != 0
    if args.baseline:
        if failed:
            print("not storing references: the capture check failed")
            return 1
        return subprocess.run(py + ["baseline", cap_dir, args.references]).returncode
    if os.path.exists(os.path.join(args.references, "captures.jsonl")):
        failed |= subprocess.run(py + ["compare", cap_dir, args.references, EXPECTATIONS]).returncode != 0
    else:
        print(f"no local references in {args.references}: perceptual comparison skipped "
              f"(create them with --baseline after a visually confirmed run)")
    print(f"CAPTURE TEST: {'FAIL' if failed else 'PASS'} (run {args.run_id})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
