#!/usr/bin/env python3
"""The effect clock at each frame's moment (M6D D4b-4, docs/HIGH_FPS.md 20).

Runs at 120 fps with --kuf_trace_effect_clock: one [kuf-effects] line per frame drawn with the clock changed (the frame
timer's count, the frame's lag behind its tick, the time the effect shaders got, the tick's own time).

  python3 -I tests/presentation/effect_check.py RUN [--sweep LABEL ...] [--min-frames N]
  python3 -I tests/presentation/effect_check.py RUN --expect-leak

Checks (one line each, exit 1 if any fails):
  frames     at least --min-frames frames (default 1000) drew their effects at their own moment (run-summary
             effect_clock_frames), and the game never found the timer's rate changed (effect_clock_leaks 0)
  range      each frame's time between the tick before's and its tick's ((count - 1) / 30 .. count / 30, within 1e-4 s),
             its lag between 0 and 1
  monotonic  outside the freezes, the time never goes back from one drawn frame to the next (within 1e-4 s)
  sweep      with --sweep LABEL (a freeze with --kuf_freeze_alpha_sweep): the five frames with only the effect clock
             moving (LABEL_e000 ... _e100) end on the full sweep's alpha 1 (LABEL_a100, within 0.1 levels mean), and if
             they differ at all (0.02 levels or more from alpha 0 to 1) step evenly (each step at least a fifth of the
             mean step) and the middle lies between the ends
With --expect-leak (--kuf_debug_effect_rate_kept: the changed rate not put back) the run summary must count leaks: the
isolation check catches a rate left changed.
"""
import argparse
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from capture_check import read_png  # noqa: E402

LINE = re.compile(r"\[kuf-effects\] frame (\d+) tick (\d+) count (\d+) lag ([\d.]+) rate ([\d.]+) time ([\d.]+) "
                  r"\(tick's ([\d.]+)\)")
FREEZE_START = re.compile(r"\[kuf-pacing\] freeze \S+: game held")
FREEZE_END = re.compile(r"\[kuf-pacing\] freeze \S+: \d+ render-only builds")


def log_lines(run):
    for name in ("kuf_cod.2.log", "kuf_cod.1.log", "kuf_cod.log"):
        path = os.path.join(run, name)
        if os.path.exists(path):
            yield from open(path, errors="replace")


def sweeps(run, labels):
    caps = {}
    index = os.path.join(run, "user", "captures", "captures.jsonl")
    if os.path.exists(index):
        for line in open(index):
            r = json.loads(line)
            if r.get("ok") and r.get("label"):
                caps[r["label"]] = os.path.join(run, "user", "captures", r["file"])
    results = []
    for label in labels:
        names = [f"{label}_e{p:03d}" for p in (0, 25, 50, 75, 100)] + [f"{label}_a100"]
        if not all(n in caps for n in names):
            results.append(("sweep", False, f"{label}: captures missing"))
            continue
        img = [read_png(caps[n])[0].astype(np.float64) for n in names]
        diff = lambda a, b: float(np.abs(a - b).mean())  # noqa: E731
        steps = [diff(img[i], img[i + 1]) for i in range(4)]
        whole = diff(img[0], img[4])
        mid = (diff(img[2], img[0]), diff(img[2], img[4]))
        same = diff(img[4], img[5])
        moving = whole >= 0.02
        even = min(steps) >= sum(steps) / 4 / 5 and max(mid) < whole if moving else True
        results.append(("sweep", same <= 0.1 and even,
                        f"{label}: effect clock alone alpha 0 to 1 {whole:.3f} levels mean"
                        + (f", steps {' '.join(f'{x:.3f}' for x in steps)}, middle to the ends {mid[0]:.3f} / "
                           f"{mid[1]:.3f}" if moving else " (nothing in view moves with it)")
                        + f", alpha 1 to the full sweep's {same:.3f}"))
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--sweep", action="append", default=[])
    ap.add_argument("--min-frames", type=int, default=1000)
    ap.add_argument("--expect-leak", action="store_true")
    args = ap.parse_args()
    name = os.path.basename(args.run)
    try:
        pacing = json.load(open(os.path.join(args.run, "user", "run-summary.json")))["pacing"]
    except (OSError, ValueError, KeyError) as e:
        print(f"  [FAIL] summary: {e}\nEFFECT CHECK {name}: FAIL")
        return 1
    frames, leaks = pacing.get("effect_clock_frames", 0), pacing.get("effect_clock_leaks", -1)
    held = pacing.get("alpha_held", 0)
    results = []
    if args.expect_leak:
        results.append(("leak", leaks > 0, f"{leaks} tick starts found the timer's rate changed (the fault must be seen)"))
    else:
        results.append(("frames", frames >= args.min_frames and leaks == 0,
                        f"{frames} frames drew their effects at their own moment, {leaks} rate leaks ({held} frames "
                        f"held at their tick's previous frame's moment, frame_pacing.cpp FrameAlpha)"))
        out_of_range, back, n = [], [], 0
        prev, frozen = None, False
        for line in log_lines(args.run):
            if FREEZE_START.search(line):
                frozen, prev = True, None
                continue
            if FREEZE_END.search(line):
                frozen, prev = False, None
                continue
            m = LINE.search(line)
            if not m:
                continue
            n += 1
            count, lag, time = int(m.group(3)), float(m.group(4)), float(m.group(6))
            if not (0 <= lag <= 1 and (count - 1) / 30 - 1e-4 <= time <= count / 30 + 1e-4):
                out_of_range.append((m.group(1), count, lag, time))
            if not frozen:
                if prev is not None and time < prev - 1e-4:
                    back.append((m.group(1), prev, time))
                prev = time
        results.append(("range", n > 0 and not out_of_range,
                        f"{n} frames traced, {len(out_of_range)} outside their tick interval"
                        + (f" (first: frame {out_of_range[0][0]})" if out_of_range else "")))
        results.append(("monotonic", not back, f"{len(back)} frames went back in time"
                        + (f" (first: frame {back[0][0]}, {back[0][1]:.5f} -> {back[0][2]:.5f})" if back else "")))
        results += sweeps(args.run, args.sweep)
    ok = all(r[1] for r in results)
    for check, good, text in results:
        print(f"  [{'ok' if good else 'FAIL'}] {check}: {text}")
    print(f"EFFECT CHECK {name}: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
