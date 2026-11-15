#!/usr/bin/env python3
"""K78: how far Low Latency's frames lead the game's camera collision (docs/CAMERA.md 9), read only.

  python3 -I tests/camera/lead_check.py <boundary run> <run> [<run> ...] [--max-excess UNITS]

Runs with --kuf_trace_camera_lead, standing still. The boundary run turns slowly with Low Latency off, so at each azimuth the
game's own collision has settled: its largest eye distance per degree is where the collision lets the camera be (the most
generous value). Each other run's frames are compared with it: excess = the frame's distance - the boundary at its azimuth.
A positive excess is a frame further out than the game ever placed the camera there.
"""
import argparse
import math
import re
import statistics
import sys

ROW = re.compile(r"kuf-cam-lead\] alpha ([\d.]+) azimuth (-?[\d.]+) dist ([\d.]+) height (-?[\d.]+) "
                 r"pivot (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) pulled (\d)")


def frames(run):
    started = False
    rows = []
    with open(f"build/runs/{run}/kuf_cod.log", errors="replace") as f:
        for line in f:
            started = started or "log mark sweep" in line
            m = ROW.search(line) if started else None
            if m:
                rows.append([float(x) for x in m.groups()])
    return rows


def near(rows, x, z, radius=8.0):
    return [r for r in rows if math.hypot(r[4] - x, r[6] - z) <= radius]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("boundary")
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--max-excess", type=float)
    a = ap.parse_args()
    base = frames(a.boundary)
    x, z = statistics.median(r[4] for r in base), statistics.median(r[6] for r in base)
    bound = {}
    for r in near(base, x, z):
        k = int(math.degrees(r[1]) // 1)
        bound[k] = max(bound.get(k, 0.0), r[2])
    print(f"boundary {a.boundary}: {len(bound)} of 360 degrees mapped, distance {min(bound.values()):.0f} to {max(bound.values()):.0f}")
    short = {k for k, v in bound.items() if v < max(bound.values()) - 30}
    ok = True
    for run in a.runs:
        rows = near(frames(run), x, z)
        all_ex, wall_ex = [], []
        for r in rows:
            k = int(math.degrees(r[1]) // 1)
            if k not in bound:
                continue
            e = r[2] - bound[k]
            all_ex.append(e)
            if k in short:
                wall_ex.append(e)
        wall_ex.sort()
        if not wall_ex:
            print(f"{run}: no frames at the collision's azimuths")
            ok = False
            continue
        over = lambda t: sum(1 for e in wall_ex if e > t)
        worst = wall_ex[-1]
        print(f"{run}: {len(rows)} frames, {len(wall_ex)} where the collision limits the camera: excess median "
              f"{statistics.median(wall_ex):.1f}, p99 {wall_ex[len(wall_ex) * 99 // 100]:.1f}, max {worst:.1f} units; "
              f"over 25: {over(25)}, over 100: {over(100)} frames")
        if a.max_excess is not None and worst > a.max_excess:
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
