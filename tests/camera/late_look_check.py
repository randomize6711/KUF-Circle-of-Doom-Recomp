#!/usr/bin/env python3
"""M15B: the mouse's delay in the view, from a run with --kuf_test_look_rate (tests/camera/late_look_*.toml).

  late_look_check.py <run directory> [--max-delay-ms N] [--min-delay-ms N] [--counts-per-radian X]

Each frame logs the counts sent so far and the view's azimuth. The delay is the shift of the counts that fits the view
best (residual in degrees): the turn's size checks that the view takes in the whole movement. Exit 1 if outside limits.
"""
import argparse
import math
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--max-delay-ms", type=float)
    ap.add_argument("--min-delay-ms", type=float)
    ap.add_argument("--radians-per-count", type=float, default=0.1 * math.pi / 180)
    a = ap.parse_args()
    text = open(a.run + "/kuf_cod.log", errors="replace").read()
    start = text.find("log mark start")
    stop = text.find("log mark end")
    rows = [(float(m[1]), float(m[2]), float(m[3])) for m in
            re.finditer(r"\[kuf-late-look\] t ([\d.]+) counts (-?[\d.]+) azimuth (-?[\d.]+)",
                        text[start:stop if stop > 0 else None])]
    if len(rows) < 200:
        print(f"only {len(rows)} frames")
        return 1
    t = [r[0] for r in rows]
    counts = [r[1] for r in rows]
    az, last, turn = [], None, 0.0
    for r in rows:
        if last is not None:
            turn += math.remainder(r[2] - last, 2 * math.pi)
        last = r[2]
        az.append(turn)

    def counts_at(when, i):
        while i > 0 and t[i] > when:
            i -= 1
        if i + 1 >= len(t) or t[i] > when:
            return counts[i]
        f = (when - t[i]) / max(t[i + 1] - t[i], 1e-9)
        return counts[i] + (counts[i + 1] - counts[i]) * f

    best = None
    for delay in range(0, 301, 2):
        want = [counts_at(t[i] - delay, i) * a.radians_per_count for i in range(len(t))]
        for sign in (1, -1):
            diff = [az[i] - sign * want[i] for i in range(len(t))]
            mean = sum(diff) / len(diff)
            rms = math.sqrt(sum((d - mean) ** 2 for d in diff) / len(diff))
            if best is None or rms < best[0]:
                best = (rms, delay, sign)
    rms, delay, sign = best
    gaps = sorted(t[i + 1] - t[i] for i in range(len(t) - 1))
    print(f"frames {len(rows)}, frame time median {gaps[len(gaps) // 2]:.1f} ms; view delay {delay} ms behind the mouse, "
          f"residual {math.degrees(rms):.3f} degrees")
    failed = (a.max_delay_ms is not None and delay > a.max_delay_ms) or (
        a.min_delay_ms is not None and delay < a.min_delay_ms) or math.degrees(rms) > 1.5
    print("FAIL" if failed else "ok")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
