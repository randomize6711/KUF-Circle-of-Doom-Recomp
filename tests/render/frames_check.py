#!/usr/bin/env python3
"""Render-only frames are complete (M6D D3b-1, docs/HIGH_FPS.md section 14): a frame drawn between ticks from a tick's
retained render data shows that tick exactly, HUD included, and its effects do not move on.

Runs: tests/render/frames_complete*.toml (tools/menu_test.py). Each holds the game for 3 s (the input script's freeze)
while render-only frames draw the last tick again, and captures the tick's own frame (idle_tick) and three render-only
frames 0.6, 1.5 and 2.7 s later (idle_1-3).

  python3 -I tests/render/frames_check.py build/runs/ctest-render-frames_complete [...]
  python3 -I tests/render/frames_check.py --expect-fail build/runs/ctest-render-k34_build_frames_fbo

Checks per run (one line each, exit 1 if any fails):
  same scene   each render-only frame against the tick's frame: mean absolute difference within 0.1 levels and every
               16x16 block's mean within 2.0 (measured 0.006 and 0.5; a missing HUD element, character or object makes
               blocks of tens of levels, the K34 runs without the HUD 30-60)
  held         the render-only frames against each other: no pixel more than 2 levels apart (effects that advanced per
               drawn frame moved by 2-40 levels between them)
--expect-fail inverts the exit status (the check must catch frames known to be incomplete).
The images stay in the run folders (game content, not committed); only statistics are printed.
"""
import argparse
import json
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from capture_check import read_png  # noqa: E402

FRAMES = ("idle_1", "idle_2", "idle_3")


def captures(run):
    """{label: image (float64)} of the run's labelled captures."""
    out = {}
    index = os.path.join(run, "user", "captures", "captures.jsonl")
    if os.path.exists(index):
        for line in open(index):
            r = json.loads(line)
            if r.get("ok") and r.get("label"):
                out[r["label"]] = read_png(os.path.join(run, "user", "captures", r["file"]))[0].astype(np.float64)
    return out


def blocks(diff, size=16):
    h, w = diff.shape[0] // size * size, diff.shape[1] // size * size
    return diff[:h, :w].reshape(h // size, size, w // size, size).mean(axis=(1, 3))


def check(run):
    caps = captures(run)
    name = os.path.basename(run.rstrip("/"))
    lines, ok = [], True
    missing = [k for k in ("idle_tick",) + FRAMES if k not in caps]
    if missing:
        return [f"  [FAIL] {name}: captures missing: {missing}"], False
    tick = caps["idle_tick"]
    for label in FRAMES:
        diff = np.abs(caps[label] - tick).max(axis=2)
        mean, worst = diff.mean(), blocks(diff).max()
        good = mean <= 0.1 and worst <= 2.0
        ok &= bool(good)
        lines.append(f"  [{'ok' if good else 'FAIL'}] same scene {name} {label}: mean {mean:.3f}, largest 16x16 block "
                     f"{worst:.2f} (limits 0.1, 2.0)")
    spread = max(np.abs(caps[a] - caps[b]).max() for a in FRAMES for b in FRAMES if a < b)
    good = spread <= 2
    ok &= bool(good)
    lines.append(f"  [{'ok' if good else 'FAIL'}] held {name}: render-only frames at most {spread:.0f} levels apart "
                 "(limit 2)")
    return lines, ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect-fail", action="store_true", help="the runs' frames are known to be incomplete")
    ap.add_argument("runs", nargs="+")
    args = ap.parse_args()
    all_ok = True
    for run in args.runs:
        lines, ok = check(run)
        all_ok &= ok
        print("\n".join(lines))
    if args.expect_fail:
        print("RENDER-ONLY FRAMES CHECK (expected to fail): " + ("caught" if not all_ok else "NOT CAUGHT"))
        return 0 if not all_ok else 1
    print("RENDER-ONLY FRAMES CHECK: " + ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
