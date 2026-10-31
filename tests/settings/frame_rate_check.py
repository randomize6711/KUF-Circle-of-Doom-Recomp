#!/usr/bin/env python3
"""The frame-rate settings reach the frame scheduler (M6D D5, docs/HIGH_FPS.md section 21).

Reads a run's log: each change made in the PC Settings menu ("[kuf-pc-menu] frame rate ...") and each change the
scheduler made ("[kuf-pacing] tick N: frame rate A -> B, ... (... ms; presents P, guest vblank V)"), and the frames
presented at the script's log marks ("[kuf-frames] ...: N frames presented").

  python3 -I tests/settings/frame_rate_check.py RUN [--window MARK_A MARK_B MIN MAX ...] [--no-changes]
  python3 -I tests/settings/frame_rate_check.py RUN --expect-not-applied

Checks (one line each, exit 1 if any fails):
  applied    every menu change reached the scheduler, in order, with the same frame rate and interpolation, within
             0.15 s (the scheduler applies it when the next tick is handed to the render thread)
  vblank     after each change the SDK's guest vblank is 60 Hz at 30 fps (the original pacing) and 1000 Hz above
  rate       from each change to the next (the last: to the last log mark after it), if at least 3 s apart, the
             frames presented per second: 30 (original) 27-31.5; a limit R
             at most R * 1.05 + 1 and at least 0.9 * min(R, 60) (render-only frames drawn; no lock at 30, K44);
             unlimited at least 54
  window     with --window: frames per second between the two log marks within MIN..MAX
  --no-changes: the route changes no frame-rate setting (applied: none made, none applied)
  --min-fps F: the rate floor above 30 fps is F (the title screen, render-thread bound by its movie)
  isolation  the run summary: no rand() or PhysX call in a render-only frame, no restore failure, no effect-clock leak,
             the scheduler's change count as logged
--expect-not-applied (the fault run, --kuf_debug_frame_rate_ignored): the menu changed the frame rate, nothing
reached the scheduler, and the applied check above fails on it (it catches a menu not connected to the scheduler).
"""
import argparse
import datetime
import json
import os
import re
import sys

STAMP = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
MENU = re.compile(r"\[kuf-pc-menu\] frame rate (.+?) interpolation (\w+) objects (\w+) animation (\w+) effects (\w+)")
SWITCH = re.compile(r"\[kuf-pacing\] tick (\d+): frame rate (.+?) -> (.+?), interpolation (\w+) objects (\w+) "
                    r"animation (\w+) effects (\w+) \([\d.]+ ms; presents (\d+), guest vblank (\d+) Hz\)")
MARK = re.compile(r"\[kuf-frames\] .*log mark (\w+): (\d+) frames presented")
DROPPED = "[kuf-pacing] frame rate request dropped"


def log_lines(run):
    for name in ("kuf_cod.2.log", "kuf_cod.1.log", "kuf_cod.log"):
        path = os.path.join(run, name)
        if os.path.exists(path):
            yield from open(path, errors="replace")


def when(line):
    m = STAMP.match(line)
    return datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() if m else None


def rate_of(name):
    """The scheduler's and the menu's names: '30 (original)' / '30 (Original)' = 30, 'unlimited' = 0, else the number."""
    name = name.lower()
    return 30 if name.startswith("30") else 0 if name == "unlimited" else int(name)


def rate_ok(rate, fps, floor=None):
    if rate == 30:
        return 27 <= fps <= 31.5
    if rate == 0:
        return fps >= (54 if floor is None else floor)
    return (0.9 * min(rate, 60) if floor is None else floor) <= fps <= rate * 1.05 + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--window", nargs=4, action="append", default=[], metavar=("MARK_A", "MARK_B", "MIN", "MAX"))
    ap.add_argument("--expect-not-applied", action="store_true")
    ap.add_argument("--no-changes", action="store_true")
    ap.add_argument("--min-fps", type=float, help="the rate check's floor above 30 fps instead of 0.9 * min(R, 60) "
                    "(the title screen's movie keeps the render thread busy, docs/HIGH_FPS.md 21.6)")
    args = ap.parse_args()
    name = os.path.basename(args.run)
    menu, switches, marks, dropped = [], [], {}, 0
    for line in log_lines(args.run):
        t = when(line)
        if DROPPED in line:
            dropped += 1
        m = MENU.search(line)
        if m:
            menu.append((t, rate_of(m.group(1)), tuple(m.group(i) == "true" for i in range(2, 6))))
            continue
        m = SWITCH.search(line)
        if m:
            switches.append((t, int(m.group(1)), rate_of(m.group(3)), tuple(m.group(i) == "true" for i in range(4, 8)),
                             int(m.group(8)), int(m.group(9))))
            continue
        m = MARK.search(line)
        if m:
            marks[m.group(1)] = (t, int(m.group(2)))
    results = []
    # applied: the scheduler's changes are the menu's, in order (a change back to the state in effect before the next
    # tick would merge two requests; the routes leave a tick between presses).
    problems = []
    if not menu and not args.no_changes:
        problems.append("no frame-rate change in the menu")
    if menu and args.no_changes:
        problems.append("the route was not to change the frame rate")
    for i, (t, rate, what) in enumerate(menu):
        if i >= len(switches):
            problems.append(f"change {i + 1} ({rate} fps) never reached the scheduler")
            break
        st, _, srate, swhat, _, _ = switches[i]
        if (srate, swhat) != (rate, what) or not (0 <= st - t <= 0.15):
            problems.append(f"change {i + 1}: menu {rate} {what}, scheduler {srate} {swhat} after {st - t:.3f} s")
    if len(switches) > len(menu):
        problems.append(f"{len(switches) - len(menu)} scheduler changes without a menu change")
    applied = (not problems, f"{len(menu)} menu changes, {len(switches)} applied"
               + (f": {'; '.join(problems[:3])}" if problems else ", each within 0.15 s"))
    if args.expect_not_applied:
        ok = bool(menu) and not switches and dropped == len(menu) and not applied[0]
        results.append(("fault", ok, f"{len(menu)} menu changes dropped before the scheduler ({dropped} logged); "
                        f"the applied check fails on it: {not applied[0]} ({applied[1]})"))
    else:
        results.append(("applied",) + applied)
        bad = [s for s in switches if s[5] != (60 if s[2] == 30 else 1000)]
        results.append(("vblank", not bad, f"{len(switches) - len(bad)} of {len(switches)} changes with the guest "
                        f"vblank for their rate" + (f" (first wrong: tick {bad[0][1]})" if bad else "")))
        segments, wrong = [], []
        last_mark = max(marks.values(), default=None)
        ends = [(b[0], b[4]) for b in switches[1:]]
        if switches:
            ends.append(last_mark if last_mark and last_mark[0] > switches[-1][0] else (switches[-1][0], switches[-1][4]))
        for a, (end, presents) in zip(switches, ends):
            seconds = end - a[0]
            if seconds < 3:
                continue
            fps = (presents - a[4]) / seconds
            segments.append(f"{a[2] or 'unlimited'}: {fps:.1f}")
            if not rate_ok(a[2], fps, args.min_fps):
                wrong.append(f"{a[2] or 'unlimited'} at tick {a[1]}: {fps:.1f}")
        results.append(("rate", (bool(segments) or not switches) and not wrong,
                        f"frames per second held {', '.join(segments) or '(no change to measure)'}"
                         + (f"; outside the limits: {', '.join(wrong)}" if wrong else "")))
    for a, b, lo, hi in args.window:
        if a not in marks or b not in marks:
            results.append(("window", False, f"{a} -> {b}: mark missing"))
            continue
        fps = (marks[b][1] - marks[a][1]) / max(1e-6, marks[b][0] - marks[a][0])
        results.append(("window", float(lo) <= fps <= float(hi), f"{a} -> {b}: {fps:.1f} frames per second "
                        f"(required {lo}-{hi})"))
    try:
        p = json.load(open(os.path.join(args.run, "user", "run-summary.json")))["pacing"]
        counters = {k: p.get(k) for k in ("rand_in_builds", "physx_in_builds", "restore_failures", "effect_clock_leaks")}
        ok = all(v == 0 for v in counters.values()) and p.get("frame_rate_switches") == len(switches)
        results.append(("isolation", ok, f"{', '.join(f'{k} {v}' for k, v in counters.items())}, "
                        f"frame_rate_switches {p.get('frame_rate_switches')} (logged {len(switches)})"))
    except (OSError, ValueError, KeyError) as e:
        results.append(("isolation", False, f"run summary: {e}"))
    ok = all(r[1] for r in results)
    for check, good, text in results:
        print(f"  [{'ok' if good else 'FAIL'}] {check}: {text}")
    print(f"FRAME RATE CHECK {name}: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
