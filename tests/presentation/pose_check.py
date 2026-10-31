#!/usr/bin/env python3
"""Skeletal poses interpolated between ticks (M6D D4b, docs/HIGH_FPS.md section 17).

Runs: game runs with --kuf_frame_rate above 30, --kuf_trace_presentation=true and
--kuf_trace_presentation_bones=true (every pose cut and every pose written into a frame between ticks: its alpha, its
progress from the earlier tick's pose to the later one, its bone length error).

  python3 -I tests/presentation/pose_check.py RUN [...] [--min-interpolated N] [--min-parts N] [--sweep LABEL]
                                                      [--summary-only] [--expect-physics-fault]

Checks per run (one line each, exit 1 if any fails); frames drawn while the game is held (freezes) are left out of
the motion check:
  summary  poses interpolated (at least --min-interpolated model-ticks), every palette written allocated, the game's
           own bone reads and its other palette readers (the physics step's parts) never finding a written palette
           (run-summary bones.bone_reads_written and physics_reads_written 0); with --summary-only (a run without the
           pose trace, e.g. the isolation run) only this
  cuts     no frame shows a pose interpolated for a model in a tick interval where its pose was cut
  motion   per model and tick interval, each frame's pose at the frame's alpha: its progress from the earlier tick's
           pose to the later one (recovered from the palette written, per turning bone in its parent's space) equal to
           the alpha within 0.01, so it never goes back as the frames go on
  lengths  bones whose offset from their parent the animation keeps keep their length (within 0.05 units)
  parts    physics-driven parts (D4b-3): at least --min-parts part-ticks interpolated; every part palette written
           at its frame's alpha (progress relative to its anchors within 0.01, run-summary progress_error_max), no
           bone's distance from its anchor outside its distances at the two ticks by more than 0.1 units
           (stretch_max), no part written in a tick interval where it was cut (trace)
  sweep    with --sweep LABEL (a freeze with --kuf_freeze_alpha_sweep): the five frames with only the skeletal poses
           moving (LABEL_b000 ... _b100) differ step by step (each step at least a fifth of the mean step), the middle
           one lies between the ends, and the last equals the full sweep's alpha 1 (LABEL_a100, within 0.1 levels
           mean: the same frame)
With --expect-physics-fault (a run with --kuf_debug_physics_sees_presented: the isolation fault injected) the summary
must show the physics step's readers finding written palettes; the check fails if it does not (the gate would not
catch the fault).
Only statistics are printed.
"""
import argparse
import json
import os
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from capture_check import read_png  # noqa: E402

WRITTEN = re.compile(r"\[kuf-present\] pose tick (\d+) alpha ([\d.]+) model (\w+) bones (\d+) (\w+) progress ([\d.]+) "
                     r"turning (\d+) length error ([\d.]+)")
CUT = re.compile(r"\[kuf-present\] tick (\d+) pose (\w+) cut (\w+)")
PART_WRITTEN = re.compile(r"\[kuf-present\] part tick (\d+) alpha [\d.]+ part (\w+) model \w+ bones \d+ (\w+)")
PART_CUT = re.compile(r"\[kuf-present\] tick (\d+) part (\w+) cut (\w+)")
PART_PROGRESS_LIMIT, PART_STRETCH_LIMIT = 0.01, 0.1
FREEZE_START = re.compile(r"\[kuf-pacing\] freeze \S+: game held")
FREEZE_END = re.compile(r"\[kuf-pacing\] freeze \S+: rand\(\) calls")
LENGTH_LIMIT, PROGRESS_LIMIT, PROGRESS_SLACK = 0.05, 0.01, 1e-3


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
        names = [f"{label}_b{p:03d}" for p in (0, 25, 50, 75, 100)] + [f"{label}_a100"]
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
        results.append((good, f"sweep {label}: poses alone alpha 0 to 1 {whole:.3f} levels mean, steps "
                              f"{' '.join(f'{x:.3f}' for x in steps)}, middle to the ends {mid[0]:.3f} / {mid[1]:.3f}, "
                              f"alpha 1 to the full sweep's {same:.3f}"))
    return results


def check(run, min_interpolated, sweep_labels=(), summary_only=False, min_parts=0, expect_physics_fault=False):
    cut, frames, held = {}, [], False
    part_cut, part_frames = {}, []
    for line in log_lines(run):
        if FREEZE_START.search(line):
            held = True
            continue
        if FREEZE_END.search(line):
            held = False
            continue
        m = WRITTEN.search(line)
        if m:
            frames.append((int(m.group(1)), float(m.group(2)), m.group(3), m.group(5), float(m.group(6)),
                           float(m.group(8)), held))
            continue
        m = CUT.search(line)
        if m:
            cut.setdefault(int(m.group(1)), set()).add(m.group(2))
            continue
        m = PART_WRITTEN.search(line)
        if m:
            part_frames.append((int(m.group(1)), m.group(2), m.group(3)))
            continue
        m = PART_CUT.search(line)
        if m:
            part_cut.setdefault(int(m.group(1)), set()).add(m.group(2))

    name = os.path.basename(run.rstrip("/"))
    results = []
    try:
        s = json.load(open(os.path.join(run, "user", "run-summary.json")))["presentation"]["bones"]
    except (OSError, KeyError, ValueError) as e:
        s = None
        results.append((False, f"summary: no run-summary bones ({e})"))
    if s is not None and expect_physics_fault:
        results.append((s["physics_reads_written"] > 0,
                        f"physics fault: the physics step's readers found a written palette in {s['physics_reads_written']}"
                        f" of {s['physics_reads']} calls (the fault injected must be seen)"))
        s = None
    if s is not None:
        ok = (s["enabled"] and s["interpolated"] >= min_interpolated and s["alloc_failed"] == 0
              and s["bone_reads_written"] == 0 and s["physics_reads_written"] == 0)
        cuts = ", ".join(f"{k} {v}" for k, v in sorted(s["cuts"].items())) or "none"
        results.append((ok, f"summary: {s['interpolated']} model-ticks interpolated (at least {min_interpolated}; "
                            f"{s['model_space']} in model space) of {s['ticks']}, cuts {cuts}, bones held "
                            f"{s['bones_held']}; up to {s['models_max']} models, {s['bones_max']} bones; largest turn "
                            f"{s['turn_max']:.3f} rad; {s['writes']} palettes written ({s['alloc_failed']} not "
                            f"allocated), {s['write_us_mean']:.1f} us per frame; put back {s['restored']}; the game's "
                            f"bone reads {s['bone_reads']}, of a written palette {s['bone_reads_written']}; its other palette readers "
                            f"{s['physics_reads']}, finding a written palette {s['physics_reads_written']}"))
        p = s.get("parts")
        if p is None:
            results.append((False, "parts: no run-summary bones.parts"))
        else:
            pcuts = ", ".join(f"{k} {v}" for k, v in sorted(p["cuts"].items())) or "none"
            results.append((p["interpolated"] >= min_parts and p["progress_error_max"] <= PART_PROGRESS_LIMIT
                            and p["stretch_max"] <= PART_STRETCH_LIMIT,
                            f"parts: {p['interpolated']} part-ticks interpolated (at least {min_parts}) of {p['ticks']}, "
                            f"cuts {pcuts}, bones held {p['held']}, without an anchor {p['unanchored']}; {p['writes']} "
                            f"palettes written, progress at most {p['progress_error_max']:.5f} from the frame's alpha "
                            f"(limit {PART_PROGRESS_LIMIT}), distance from the anchor at most {p['stretch_max']:.4f} "
                            f"units outside the ticks' (limit {PART_STRETCH_LIMIT})"))

    if frames:
        across, back, outside, checked = 0, 0, 0, 0
        length_max, off_alpha = 0.0, 0.0
        last = {}
        local = sum(1 for f in frames if f[3] == "local")
        for tick, alpha, model, space, progress, length, frozen in frames:
            checked += 1
            if model in cut.get(tick, set()):
                across += 1
            if progress < -PROGRESS_SLACK or progress > 1 + PROGRESS_SLACK:
                outside += 1
            off_alpha = max(off_alpha, abs(progress - alpha))
            length_max = max(length_max, length)
            if not frozen:
                key = (tick, model)
                pa = last.get(key)
                if pa and alpha > pa[0] and progress < pa[1] - PROGRESS_SLACK:
                    back += 1
                last[key] = (alpha, progress)
        results += [
            (across == 0, f"cuts: {across} poses written in a tick interval where the model's pose was cut"),
            (back == 0 and outside == 0 and off_alpha <= PROGRESS_LIMIT,
             f"motion: {checked} poses written ({local} in parent space), progress at most {off_alpha:.4f} from the "
             f"frame's alpha (limit {PROGRESS_LIMIT}), {back} went back from the frame before, {outside} outside the "
             f"two ticks' poses"),
            (length_max <= LENGTH_LIMIT, f"lengths: largest bone length error {length_max:.5f} units "
                                         f"(limit {LENGTH_LIMIT})"),
        ]
    elif min_interpolated and not summary_only and not expect_physics_fault:
        results.append((False, "trace: no pose written (--kuf_trace_presentation_bones)"))
    if part_frames:
        across = sum(1 for tick, part, kind in part_frames if part in part_cut.get(tick, set()) and kind != "tick")
        results.append((across == 0, f"part cuts: {len(part_frames)} part palettes written, {across} interpolated in a "
                                     f"tick interval where the part was cut"))
    elif min_parts and not summary_only:
        results.append((False, "trace: no part palette written (--kuf_trace_presentation_bones)"))
    results += sweeps(run, sweep_labels)
    lines = [f"  [{'ok' if ok else 'FAIL'}] {name} {text}" for ok, text in results]
    return lines, all(ok for ok, _ in results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--min-interpolated", type=int, default=1)
    ap.add_argument("--sweep", action="append", default=[])
    ap.add_argument("--summary-only", action="store_true")
    ap.add_argument("--min-parts", type=int, default=0)
    ap.add_argument("--expect-physics-fault", action="store_true")
    args = ap.parse_args()
    ok = True
    for run in args.runs:
        lines, good = check(run, args.min_interpolated, args.sweep, args.summary_only, args.min_parts,
                            args.expect_physics_fault)
        print("\n".join(lines))
        ok &= good
    print("POSE CHECK: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
