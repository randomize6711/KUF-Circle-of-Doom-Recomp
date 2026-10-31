#!/usr/bin/env python3
"""World objects interpolated between ticks (M6D D4a, docs/HIGH_FPS.md section 16).

Runs: game runs with --kuf_frame_rate above 30 and --kuf_trace_presentation=true; the path checks also need
--kuf_trace_presentation_objects=true (every object of every tick as written, every interpolated object of every
presented frame). The isolation check needs --kuf_trace_game_state=true and --kuf_presentation_test_offset.

  python3 -I tests/presentation/object_check.py RUN [...] [--min-interpolated N] [--isolation OFFSET] [--sweep LABEL]

Checks per run (one line each, exit 1 if any fails); frames drawn while the game is held (freezes) are left out of
the motion check:
  summary     objects interpolated (at least --min-interpolated object-ticks), every identity change explained by a
              release the game made (run-summary objects.unexplained 0), releases seen
  cuts        no frame shows an object interpolated in a tick interval where it was cut, new or not listed before
  path        every interpolated object in every frame on the straight path between its two ticks as written, at the
              frame's alpha (within 0.05 units); for objects whose forward is level at both ticks (characters; a
              tumbling object's heading is not linear in its rotation) the heading between the two ticks' headings
              the shorter way round (within 0.5 degrees)
  steps       no interpolated object moved more than 400 units in its tick (the teleport backstop)
  motion      per object and tick interval, the frames move on along the path, never back (alpha never decreases)
  sweep       with --sweep LABEL (a freeze with --kuf_freeze_alpha_sweep): the five frames with only the world objects
              moving (LABEL_o000 ... _o100) differ step by step (each step at least a fifth of the mean step), the
              middle one lies between the ends, and the last equals the full sweep's alpha 1 (LABEL_a100, within
              0.1 levels mean: the same frame)
  isolation   with --isolation OFFSET (every presented object raised by 3/4 of it): the game's own object positions
              per tick (the state trace, as the game wrote them) never step by more than a third of that from one tick
              to the next; a presented matrix taken into the game would jump by the whole raise
Only statistics are printed.
"""
import argparse
import json
import math
import os
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from capture_check import read_png  # noqa: E402

NUM = r"-?[\d.]+|-?nan|-?inf"
ENTRY = re.compile(rf"(\d+):\(({NUM}),({NUM}),({NUM})\):({NUM}):({NUM})")
WRITTEN = re.compile(r"\[kuf-present\] tick (\d+) objects as written:(.*)$")
FRAME = re.compile(r"\[kuf-present\] frame tick (\d+) alpha ([\d.]+) objects:(.*)$")
CUT = re.compile(r"\[kuf-present\] tick (\d+) object (\d+) class \w+ cut (\w+)")
COUNTS = re.compile(r"\[kuf-present\] tick (\d+) objects: listed (\d+), interpolated (\d+)")
STATE = re.compile(r"\[kuf-state\] tick (\d+) counter \d+ seed \w+ objects \d+:(.*)$")
STATE_OBJECT = re.compile(rf"(\d+):(\w+):({NUM}),({NUM}),({NUM})")
FREEZE_START = re.compile(r"\[kuf-pacing\] freeze \S+: game held")
FREEZE_END = re.compile(r"\[kuf-pacing\] freeze \S+: rand\(\) calls")
VIEW_CLASS = "82102A90"
PATH_LIMIT, HEADING_LIMIT, STEP_LIMIT = 0.05, 0.5, 400.0
LEVEL = 0.02  # radians: the forward level at both ticks, so the rotation between them is about the vertical axis


def entries(text):
    return {int(m.group(1)): ((float(m.group(2)), float(m.group(3)), float(m.group(4))), float(m.group(5)),
                              float(m.group(6)))
            for m in ENTRY.finditer(text)}


def wrap(a):
    return math.remainder(a, 2 * math.pi)


def log_lines(run):
    """The run's log, rotated files first (the sandbox keeps the newest files of a long trace)."""
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
        names = [f"{label}_o{p:03d}" for p in (0, 25, 50, 75, 100)] + [f"{label}_a100"]
        if not all(n in caps for n in names):
            results.append((False, f"sweep {label}: captures missing"))
            continue
        img = [read_png(caps[n])[0].astype(np.float64) for n in names]
        diff = lambda a, b: float(np.abs(a - b).mean())  # noqa: E731
        steps = [diff(img[i], img[i + 1]) for i in range(4)]
        whole = diff(img[0], img[4])
        mid = (diff(img[2], img[0]), diff(img[2], img[4]))
        same = diff(img[4], img[5])
        good = min(steps) >= sum(steps) / 4 / 5 and min(steps) > 0 and max(mid) < whole and same <= 0.1
        results.append((good, f"sweep {label}: objects alone alpha 0 to 1 {whole:.3f} levels mean, steps "
                              f"{' '.join(f'{x:.3f}' for x in steps)}, middle to the ends {mid[0]:.3f} / {mid[1]:.3f}, "
                              f"alpha 1 to the full sweep's {same:.3f}"))
    return results


def check(run, min_interpolated, isolation, sweep_labels=()):
    written, cut, frames, interpolated_ticks = {}, {}, [], 0
    state_steps_max, state_pairs = (0.0, ()), 0
    previous_state, previous_tick = {}, None
    held = False
    for line in log_lines(run):
        if FREEZE_START.search(line):
            held = True
            continue
        if FREEZE_END.search(line):
            held = False
            continue
        m = FRAME.search(line)
        if m:
            frames.append((int(m.group(1)), float(m.group(2)), entries(m.group(3)), held))
            continue
        m = WRITTEN.search(line)
        if m:
            written[int(m.group(1))] = entries(m.group(2))
            continue
        m = CUT.search(line)
        if m:
            cut.setdefault(int(m.group(1)), set()).add(int(m.group(2)))
            continue
        m = COUNTS.search(line)
        if m:
            interpolated_ticks += int(m.group(3))
            continue
        if isolation:
            m = STATE.search(line)
            if m:
                tick = int(m.group(1))
                current = {(int(i), c): (float(x), float(y), float(z))
                           for i, c, x, y, z in STATE_OBJECT.findall(m.group(2)) if c != VIEW_CLASS}
                if previous_tick == tick - 1:
                    for key, p in current.items():
                        q = previous_state.get(key)
                        if q and all(math.isfinite(v) for v in p + q):
                            state_pairs += 1
                            state_steps_max = max(state_steps_max, (abs(p[1] - q[1]), (tick, key[0])))
                previous_state, previous_tick = current, tick

    name = os.path.basename(run.rstrip("/"))
    results = []
    try:
        summary = json.load(open(os.path.join(run, "user", "run-summary.json")))["presentation"]["objects"]
    except (OSError, KeyError, ValueError) as e:
        summary = None
        results.append((False, f"summary: no run-summary objects ({e})"))
    if summary is not None:
        ok = (summary["interpolated"] >= min_interpolated and summary["unexplained"] == 0 and summary["releases"] > 0)
        cuts = ", ".join(f"{k} {v}" for k, v in sorted(summary["cuts"].items()))
        results.append((ok, f"summary: {summary['interpolated']} object-ticks interpolated (at least {min_interpolated}), "
                            f"{summary['unchanged']} unchanged of {summary['ticks']}, unexplained identity changes "
                            f"{summary['unexplained']}, releases {summary['releases']}, indices reused {summary['reused']}, taken "
                            f"back {summary['restored']}; "
                            f"cuts {cuts}; largest step {summary['step_max']:.1f} units, turn "
                            f"{math.degrees(summary['turn_max']):.1f} degrees; writes {summary['writes']}, "
                            f"{summary['write_us_mean']:.1f} us per frame (max {summary['write_us_max']:.0f})"))

    if frames:
        across, off_path, off_heading, back, checked, tilted = 0, (0.0, ()), (0.0, ()), 0, 0, 0
        steps_max = (0.0, ())
        last_alpha = {}
        for tick, alpha, objects, frozen in frames:
            a, b = written.get(tick - 1, {}), written.get(tick, {})
            for index, (pos, yaw, _) in objects.items():
                if index in cut.get(tick, set()) or index not in a or index not in b:
                    across += 1
                    continue
                (pa, ya, ea), (pb, yb, eb) = a[index], b[index]
                expect = tuple(pa[k] + (pb[k] - pa[k]) * alpha for k in range(3))
                off_path = max(off_path, (math.dist(expect, pos), (tick, index)))
                if abs(ea) < LEVEL and abs(eb) < LEVEL:
                    heading = ya + wrap(yb - ya) * alpha
                    off_heading = max(off_heading, (abs(math.degrees(wrap(yaw - heading))), (tick, index)))
                else:
                    tilted += 1
                steps_max = max(steps_max, (math.dist(pa, pb), (tick, index)))
                checked += 1
                if not frozen:
                    key = (tick, index)
                    if alpha < last_alpha.get(key, -1.0):
                        back += 1
                    last_alpha[key] = alpha
        results += [
            (across == 0, f"cuts: {across} object frames interpolated across a cut or a gap"),
            (off_path[0] <= PATH_LIMIT and off_heading[0] <= HEADING_LIMIT,
             f"path: {checked} object frames, largest distance from the path {off_path[0]:.4f} units at {off_path[1]}, "
             f"heading {off_heading[0]:.3f} degrees at {off_heading[1]} ({tilted} frames of objects not level "
             f"left out of the heading)"),
            (steps_max[0] <= STEP_LIMIT, f"steps: largest interpolated step {steps_max[0]:.1f} units at {steps_max[1]} "
                                         f"(limit {STEP_LIMIT:.0f})"),
            (back == 0, f"motion: {back} frames went back along an object's path"),
        ]
    elif min_interpolated:
        results.append((interpolated_ticks >= min_interpolated,
                        f"trace: {interpolated_ticks} object-ticks interpolated (no per-object frame trace)"))

    if isolation:
        limit = isolation * 0.75 / 3
        results.append((state_pairs > 0 and state_steps_max[0] <= limit,
                        f"isolation: {state_pairs} object steps in the game's state, largest vertical "
                        f"{state_steps_max[0]:.2f} units at {state_steps_max[1]} (limit {limit:.0f}; presented "
                        f"objects raised by {isolation * 0.75:.0f})"))
    results += sweeps(run, sweep_labels)
    lines = [f"  [{'ok' if ok else 'FAIL'}] {name} {text}" for ok, text in results]
    return lines, all(ok for ok, _ in results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--min-interpolated", type=int, default=1)
    ap.add_argument("--isolation", type=int, default=0)
    ap.add_argument("--sweep", action="append", default=[])
    args = ap.parse_args()
    ok = True
    for run in args.runs:
        lines, good = check(run, args.min_interpolated, args.isolation, args.sweep)
        print("\n".join(lines))
        ok &= good
    print("OBJECT CHECK: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
