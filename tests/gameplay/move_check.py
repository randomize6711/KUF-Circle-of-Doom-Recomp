#!/usr/bin/env python3
"""M15C: the player's distance and speed between log marks, from a run with --kuf_trace_presentation at a fixed frame rate.

  move_check.py <run directory> <mark>:<end mark> ...   prints distance and units per second for each window
  --expect <mark>=<min>:<max>   the speed (units per second) must lie in the range (exit 1 otherwise)
  --ratio <mark>/<mark>=<min>:<max>   the speeds' ratio must lie in the range
"""
import argparse
import math
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("windows", nargs="+")
    ap.add_argument("--expect", action="append", default=[])
    ap.add_argument("--ratio", action="append", default=[])
    a = ap.parse_args()
    speeds, failed = {}, False
    text = open(a.run + "/kuf_cod.log", errors="replace").read()
    for w in a.windows:
        first, last = w.split(":")
        s = text.find(f"log mark {first}\n") if f"log mark {first}\n" in text else text.find(f"log mark {first}")
        e = text.find(f"log mark {last}", s + 1)
        rows = re.findall(r"player \d+ \w+ pos \((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)", text[s:e])
        ticks = re.findall(r"\[kuf-present\] (?:tick|build) tick (\d+)", text[s:e])
        if len(rows) < 2 or len(ticks) < 2:
            print(f"{first}: no positions ({len(rows)})")
            failed = True
            continue
        p = [tuple(map(float, r)) for r in rows]
        path = sum(math.dist(p[i], p[i + 1]) for i in range(len(p) - 1))
        seconds = max((int(ticks[-1]) - int(ticks[0])) / 30.0, 1 / 30)
        # The steady part: the second half of the window (the game's own start of a walk is not part of this measure).
        half = p[len(p) // 2:]
        steady = sum(math.dist(half[i], half[i + 1]) for i in range(len(half) - 1)) / max(seconds / 2, 1 / 30)
        speeds[first] = steady
        print(f"{first}: {len(p)} frames, path {path:.1f} units in {seconds:.2f} s, steady {steady:.1f} units/s, "
              f"straight line {math.dist(p[0], p[-1]):.1f}")
    for e in a.expect:
        k, r = e.split("=")
        lo, hi = map(float, r.split(":"))
        if k not in speeds or not lo <= speeds[k] <= hi:
            print(f"FAIL {k}: {speeds.get(k)} outside {lo}..{hi}")
            failed = True
    for e in a.ratio:
        k, r = e.split("=")
        x, y = k.split("/")
        lo, hi = map(float, r.split(":"))
        q = speeds.get(x, 0) / max(speeds.get(y, 1e-9), 1e-9)
        print(f"{k} = {q:.3f}")
        if not lo <= q <= hi:
            print(f"FAIL {k}: outside {lo}..{hi}")
            failed = True
    print("FAIL" if failed else "ok")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
