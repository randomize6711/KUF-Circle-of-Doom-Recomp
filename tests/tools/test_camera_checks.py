#!/usr/bin/env python3
"""Fault injection for the M6B camera tests' log checks (ctest camera.checks_fault_injection).

The camera tests ([[scenario.log_value]] in tests/camera/*.toml) measure the camera every frame from the game's
trace. This test shows that their checks catch the regressions they are meant to: for each case it takes the
log of the camera test's last run (build/runs/ctest-camera-<test>, written by the camera tests this one requires),
checks that the log as it is passes, then changes the trace the way a regression would (an eye that jumps, a
camera that creeps in, a collision release that snaps, a stale field of view, a scripted input that reaches the
game a tick late (K38), ...) and checks that the named check fails. Only copies of the logs are changed, in memory.

  python3 -I tests/tools/test_camera_checks.py [--runs build/runs] [--prefix ctest-camera]
"""
import argparse
import os
import re
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from menu_test import check_input_ticks, check_log_values  # noqa: E402


def window(lines, after, before=None):
    """The checker's window: from the first line with `after` to the next line with `before`."""
    start = next(i for i, l in enumerate(lines) if after in l)
    end = len(lines)
    if before:
        end = next((i for i in range(start + 1, len(lines)) if before in lines[i]), len(lines))
    return start, end


def change(lines, after, before, line_pattern, edit, which="middle"):
    """A copy of the log with one line (or all, which="all") in the window that matches line_pattern edited."""
    out = list(lines)
    start, end = window(out, after, before)
    hits = [i for i in range(start, end) if re.search(line_pattern, out[i])]
    if not hits:
        raise AssertionError(f"no line matching {line_pattern!r} between {after!r} and {before!r}")
    for i in hits if which == "all" else [hits[-1]] if which == "last" else [hits[len(hits) // 2]]:
        out[i] = edit(out[i])
    return out


def scale_number(label, factor):
    """Edit: multiply the number after `label` (e.g. 'distance ') by factor."""
    def edit(line):
        return re.sub(re.escape(label) + r"(-?[0-9.]+)", lambda m: f"{label}{float(m.group(1)) * factor:.1f}", line,
                      count=1)
    return edit


def turn_yaw(degrees):
    """Edit: the view's yaw turned by `degrees` (wrapped to -180..180)."""
    def edit(line):
        return re.sub(r"yaw (-?[0-9.]+) deg",
                      lambda m: f"yaw {(float(m.group(1)) + degrees + 180.0) % 360.0 - 180.0:.2f} deg", line, count=1)
    return edit


def set_number(label, value):
    def edit(line):
        return re.sub(re.escape(label) + r"-?[0-9.]+", f"{label}{value}", line, count=1)
    return edit


def late_item(lines, after, before, item_pattern, update_pattern):
    """A copy of the log in which one scripted item (the middle one in the window) reaches the game a tick late: it
    moves after the next per-tick trace line, keeping its scripted tick, so one tick sees no item and the next sees
    two (K38: items applied at presents)."""
    out = list(lines)
    start, end = window(out, after, before)
    items = [i for i in range(start, end) if re.search(item_pattern, out[i])]
    i = items[len(items) // 2]
    nxt = next(j for j in items if j > i)
    upd = next(j for j in range(i + 1, nxt) if re.search(update_pattern, out[j]))
    moved = out.pop(i)
    out.insert(upd, moved)  # after the trace line (now at upd - 1)
    return out


# (camera test, what the fault is, the change to its log, the checks that must fail)
CASES = [
    ("camera_orbit", "a scripted look reaches the game a tick late, with the next one (K38)",
     lambda l: late_item(l, "log mark slow_up", "log mark top_slow", r"\[kuf-input-script\] tick \d+: .* look ",
                         r"\[kuf-cam\] orbit:"),
     ["scripted camera input"]),
    ("camera_orbit_native", "a stick change reaches the game a tick late, with the next one (K38)",
     lambda l: late_item(l, "log mark stick", "log mark end", r"\[kuf-input-script\] tick \d+: .*pad axis",
                         r"\[kuf-cam\] orbit:"),
     ["scripted camera input"]),
    ("camera_orbit", "the eye jumps 75 units in one frame while the stick turns the camera (a snap)",
     lambda l: change(l, "log mark stick", "log mark end", r"\[kuf-cam\] orbit: .*step [0-9.]+, jerk",
                      lambda s: re.sub(r"step [0-9.]+, jerk [0-9.]+", "step 75.00, jerk 75.00", s, count=1)),
     ["stick: largest eye step", "stick: largest step change"]),
    ("camera_orbit", "the camera stops aiming at the character (3 degrees off) in the slow sweep up",
     lambda l: change(l, "log mark slow_up", "log mark top_slow", r"aim off pivot [0-9.]+ deg",
                      set_number("aim off pivot ", "3.00")),
     ["slow_up: aim off the pivot"]),
    ("camera_mouse", "the distance creeps in while looking down (the first M6B builds: 707 -> 616 units)",
     lambda l: change(l, "look 0 2000", "log mark down_limit", r"\[kuf-cam\] view: .*modern, offset",
                      scale_number("distance ", 616.0 / 707.0), which="all"),
     ["looking down keeps the distance"]),
    ("camera_sensitivity_max", "the mouse's top level turns only as fast as the old top level (0.25 degrees a count: "
     "100 counts turn 25 degrees, not 150)",
     lambda l: change(l, "log mark mouse_right", "look 0 -40", r"\[kuf-cam\] view: pitch", turn_yaw(-125.0),
                      which="last"),
     ["mouse right 100 counts"]),
    ("camera_distance_default", "the collision's pull-in is released at once (a snap back out of 300 units)",
     lambda l: change(l, "log mark level_turn", "log mark go_low", r"distance change [0-9.]+,",
                      set_number("distance change ", "300.00")),
     ["level_turn: largest distance increase in a frame (released gradually)"]),
    ("camera_distance_default", "the scenery is never hit (the collision does not run at the default distance)",
     lambda l: change(l, "log mark level_turn", "log mark go_low", r"collision: hit 1,",
                      lambda s: s.replace("collision: hit 1,", "collision: hit 0,"), which="all"),
     ["level_turn: the scenery was in the way (collision)"]),
    ("camera_distance_far", "the 200 % camera snaps back out after the collision (346 units in a frame, the "
     "M6B distance's first build)",
     lambda l: change(l, "log mark level_turn", "log mark go_low", r"distance change [0-9.]+,",
                      set_number("distance change ", "346.00")),
     ["level_turn: largest distance increase in a frame (released gradually)"]),
    ("camera_fov_max", "a frame written with the gameplay angle is drawn with the loading camera's (stale slot)",
     lambda l: change(l, "projection: gameplay view write #", "@gameplay+300 capture start",
                      r"projection: render, fov 1\.3535 .*\(gameplay view, 1\.3535\)",
                      lambda s: s.replace("render, fov 1.3535", "render, fov 0.5000", 1)),
     ["handover: frames written with the gameplay angle drawn with it (smallest)"]),
    ("camera_fov_max", "one gameplay projection is built with another angle (60 degrees)",
     lambda l: change(l, "@gameplay+300 capture start", "log mark end", r"projection: fov 1\.3535 rad",
                      lambda s: s.replace("fov 1.3535 rad", "fov 1.0472 rad", 1)),
     ["smallest field of view in gameplay"]),
    ("camera_original", "Field of View 110 acts with Modern Camera Off",
     lambda l: change(l, "log mark start", "log mark end", r"projection: fov 0\.6000 rad",
                      lambda s: s.replace("fov 0.6000 rad", "fov 1.3535 rad", 1)),
     ["field of view: the game's own (Field of View 110 does not act)"]),
    ("camera_original", "the gameplay frames are drawn with the loading camera's angle (Modern Camera Off)",
     lambda l: change(l, "projection: gameplay view write #", "@gameplay+300 capture start",
                      r"projection: render, fov 0\.6000 .*\(gameplay view, 0\.6000\)",
                      lambda s: s.replace("render, fov 0.6000", "render, fov 0.5000", 1), which="all"),
     ["handover: frames written with the gameplay angle drawn with it (smallest)",
      "handover: frames written with the gameplay angle drawn with it (largest)"]),
]


def load_log(run_dir):
    text = ""
    for name in sorted(os.listdir(run_dir)):
        if name.startswith("kuf_cod") and name.endswith(".log"):
            text += open(os.path.join(run_dir, name), errors="replace").read()
    return text.splitlines()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", default=os.path.join(ROOT, "build", "runs"))
    ap.add_argument("--prefix", default="ctest-camera")
    args = ap.parse_args()
    failures = 0
    baselines = {}
    for test, fault, inject, must_fail in CASES:
        with open(os.path.join(ROOT, "tests", "camera", f"{test}.toml"), "rb") as f:
            scenario = tomllib.load(f)["scenario"]
        rules, tick_rules = scenario["log_value"], scenario.get("input_ticks", [])

        def check(lines):
            return check_log_values(rules, lines)[0] + check_input_ticks(tick_rules, lines)[0]
        names = {r["name"] for r in rules + tick_rules}
        unknown = [n for n in must_fail if n not in names]
        if unknown:
            print(f"FAIL {test}: no check named {unknown}")
            failures += 1
            continue
        if test not in baselines:
            run_dir = os.path.join(args.runs, f"{args.prefix}-{test}")
            if not os.path.isdir(run_dir):
                print(f"FAIL {test}: no run {run_dir} (run ctest -R camera.{test} first)")
                failures += 1
                baselines[test] = None
                continue
            lines = load_log(run_dir)
            problems = check(lines)
            if problems:
                print(f"FAIL {test}: the recorded log does not pass as it is: {problems[:3]}")
                failures += 1
                baselines[test] = None
                continue
            baselines[test] = lines
            print(f"ok   {test}: the recorded log passes ({len(rules) + len(tick_rules)} checks)")
        lines = baselines[test]
        if lines is None:
            continue
        try:
            faulty = inject(lines)
        except (AssertionError, StopIteration) as e:
            print(f"FAIL {test}: cannot inject '{fault}': {e}")
            failures += 1
            continue
        problems = check(faulty)
        failed = {n for n in must_fail if any(p.startswith(n + " = ") or p.startswith(n + ":") for p in problems)}
        missed = [n for n in must_fail if n not in failed]
        if missed:
            print(f"FAIL {test}: '{fault}' not caught by {missed} (problems: {problems[:3]})")
            failures += 1
        else:
            print(f"ok   {test}: '{fault}' caught: {'; '.join(p for p in problems if any(p.startswith(n) for n in must_fail))}")
    print(f"{len(CASES) - failures}/{len(CASES)} fault cases caught" if not failures else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
