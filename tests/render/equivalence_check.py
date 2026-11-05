#!/usr/bin/env python3
"""M12: a performance change draws the same frames as its control build.

  equivalence_check.py <control run> <candidate run> [--max-diff N]

Both runs follow tests/perf/render_equivalence.toml with the same options, one with the control build and one with
the build under test. As in direct_load_check.py (M9): the captures show the same game state only when both runs
entered gameplay at the same tick, which is checked first (a mismatch is an anchor mismatch, not a rendering
result: rerun); then every gameplay capture must match pixel for pixel within --max-diff levels (default 0). Even
two runs of one build are not always identical (PERFORMANCE.md 8.5: up to 2 levels in a few captures), so a
candidate that differs should be compared with more than one control run before it is called different. Reads logs
and captures, writes nothing.
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("direct_load_check", os.path.join(HERE, "direct_load_check.py"))
dl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dl)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("control_run")
    ap.add_argument("candidate_run")
    ap.add_argument("--max-diff", type=int, default=0)
    a = ap.parse_args()
    ta, tb = dl.gameplay_tick(a.control_run), dl.gameplay_tick(a.candidate_run)
    if ta is None or ta != tb:
        print(f"[FAIL] anchor: gameplay entered at tick {ta} and {tb}: the captures are not of the same game state "
              "(rerun; not a rendering result)")
        return 2
    print(f"[ok] anchor: both entered gameplay at tick {ta}")
    ca, cb = dl.captures(a.control_run), dl.captures(a.candidate_run)
    missing = [l for l in dl.GAMEPLAY if l not in ca or l not in cb]
    if missing:
        print(f"[FAIL] captures missing: {missing}")
        return 1
    ok = True
    for label in dl.GAMEPLAY:
        x, _ = dl.cc.read_png(ca[label])
        y, _ = dl.cc.read_png(cb[label])
        if x.shape != y.shape:
            print(f"[FAIL] {label}: sizes {x.shape} and {y.shape}")
            ok = False
            continue
        d = abs(x.astype("int16") - y.astype("int16"))
        diff = int(d.max())
        good = diff <= a.max_diff
        ok &= good
        print(f"[{'ok' if good else 'FAIL'}] {label}: largest difference {diff} (allowed {a.max_diff}), "
              f"pixels differing {int((d.max(axis=-1) > 0).sum())}")
    print("EQUIVALENCE CHECK:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
