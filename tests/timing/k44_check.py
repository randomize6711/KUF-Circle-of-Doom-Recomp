#!/usr/bin/env python3
"""The presentation clock after a late tick (M6D K44, docs/HIGH_FPS.md section 18).

Runs: game runs with --kuf_frame_rate at a whole multiple of 30 (60, 90, 120), one tick held by
--kuf_debug_tick_stall_at / --kuf_debug_tick_stall_us so that it starts late by nearly a frame step (as the host's load
makes one), and --kuf_trace_frame_clock from before that tick (one [kuf-clock] line per tick: its frames and what
became of each frame step).

  python3 -I tests/timing/k44_check.py RUN --rate 60 [--expect-plateau]
  python3 -I tests/timing/k44_check.py RUN --rate 60 --summary-only [--from-s 60]

Checks (one line each, exit 1 if any fails), over the traced ticks from the third after the held one:
  stall     the held tick is in the log
  frames    at least 95 % of the ticks drew all rate / 30 frames (the tick's own and the render-only ones): before K44
            the clock kept the late tick's phase and every following tick lost the same frame step (60 -> 30 frames
            per second, 120 -> 90)
  late      at most 2 % of the ticks dropped a frame step as late
  busy      at most 5 % of the ticks dropped a frame step because the render thread was still drawing (starvation)
  ticks     30 game ticks per second over those ticks (29.7 to 30.3, from the log lines' times)
With --expect-plateau (the fault injected: --kuf_present_clock_keep_late_phase) the run must show the lock instead: at
least 80 % of the ticks drew exactly one frame fewer than rate / 30; the check fails if it does not (the test would
not catch the fault).
With --summary-only (a long run without the held tick and the trace, the lock left to the host's load as in K44's
sessions): at most 2 % of the ticks dropped a frame step as late (run-summary late_ticks; locked, every tick did), and
no three 10-second windows in a row from --from-s on at the plateau (rate - 30 frames per second, within 1).
"""
import argparse
import datetime
import json
import os
import re
import sys

CLOCK = re.compile(r"^\[([\d-]+ [\d:.]+)\] .*\[kuf-clock\] tick (\d+) .*; frames (\d+):(.*)$")
STALL = re.compile(r"\[kuf-clock\] test stall: tick (\d+)")


def log_lines(run):
    for name in ("kuf_cod.2.log", "kuf_cod.1.log", "kuf_cod.log"):
        path = os.path.join(run, name)
        if os.path.exists(path):
            yield from open(path, errors="replace")


def report(name, results):
    ok = True
    for check, good, text in results:
        print(f"  [{'ok' if good else 'FAIL'}] {check}: {text}")
        ok = ok and good
    print(f"K44 CHECK {name}: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def summary_only(args):
    try:
        s = json.load(open(os.path.join(args.run, "user", "run-summary.json")))
    except (OSError, ValueError) as e:
        return report(os.path.basename(args.run), [("summary", False, f"no run-summary.json: {e}")])
    pacing, results = s["pacing"], []
    ticks, late = pacing.get("ticks", 0), pacing.get("late_ticks")
    results.append(("late", late is not None and late <= 0.02 * ticks,
                    f"{late} of {ticks} ticks dropped a frame step as late"))
    windows = [f / 10 for f in s.get("frame_timeline", [])][args.from_s // 10:-1]
    plateau, run, longest = args.rate - 30, 0, 0
    for fps in windows:
        run = run + 1 if abs(fps - plateau) <= 1 else 0
        longest = max(longest, run)
    shown = " ".join(f"{f:.0f}" for f in windows)
    results.append(("plateau", longest < 3, f"longest run of 10 s windows at {plateau} fps: {longest} (fps: {shown})"))
    timeline = s.get("sim_tick_timeline", [])[args.from_s // 10:-1]
    rate = sum(timeline) / (10 * len(timeline)) if timeline else 0
    results.append(("ticks", 29.7 <= rate <= 30.3, f"{rate:.2f} ticks per second from {args.from_s} s"))
    return report(os.path.basename(args.run), results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--rate", type=int, required=True)
    ap.add_argument("--expect-plateau", action="store_true")
    ap.add_argument("--summary-only", action="store_true")
    ap.add_argument("--from-s", type=int, default=60)
    args = ap.parse_args()
    if args.summary_only:
        return summary_only(args)
    full = args.rate // 30
    stall, ticks = None, []
    for line in log_lines(args.run):
        m = STALL.search(line)
        if m:
            stall = int(m.group(1))
        m = CLOCK.search(line)
        if m:
            steps = m.group(4)
            when = datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
            ticks.append((int(m.group(2)), int(m.group(3)), ":late" in steps, ":busy" in steps, when))
    results = []
    results.append(("stall", stall is not None, f"held tick {stall}"))
    after = [t for t in ticks if stall is not None and t[0] >= stall + 2]
    n = len(after)
    if n < 300:
        results.append(("trace", False, f"{n} traced ticks after the held one (at least 300)"))
    else:
        complete = sum(1 for t in after if t[1] == full)
        one_less = sum(1 for t in after if t[1] == full - 1)
        late = sum(1 for t in after if t[2])
        busy = sum(1 for t in after if t[3])
        counts = {}
        for t in after:
            counts[t[1]] = counts.get(t[1], 0) + 1
        shown = ", ".join(f"{k}: {v}" for k, v in sorted(counts.items()))
        if args.expect_plateau:
            results.append(("plateau", one_less >= 0.8 * n,
                            f"{one_less} of {n} ticks drew {full - 1} frames (the lock; frames per tick {shown})"))
        else:
            results.append(("frames", complete >= 0.95 * n, f"{complete} of {n} ticks drew {full} frames ({shown})"))
            results.append(("late", late <= 0.02 * n, f"{late} of {n} ticks dropped a step as late"))
            results.append(("busy", busy <= 0.05 * n, f"{busy} of {n} ticks dropped a step, render thread busy"))
        wall = after[-1][4] - after[0][4]
        rate = (after[-1][0] - after[0][0]) / wall if wall > 0 else 0
        results.append(("ticks", 29.7 <= rate <= 30.3, f"{rate:.2f} ticks per second"))
    return report(os.path.basename(args.run), results)


if __name__ == "__main__":
    sys.exit(main())
