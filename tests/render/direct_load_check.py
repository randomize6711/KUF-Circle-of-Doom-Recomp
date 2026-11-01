#!/usr/bin/env python3
"""M9 (SDK patch 0013): render-to-texture results loaded directly into images draw the same frames.

  direct_load_check.py <run with the staging buffer> <run with direct loads> [--max-diff N]

Both runs follow tests/perf/render_equivalence.toml at native 4K. The game's state at a tick is the same in both
(the fixed tick), but only when both entered gameplay at the same tick (the route's intro skip can land a few ticks
apart): that is checked first, and a mismatch fails as an anchor mismatch, not as a rendering difference. Then every
capture taken in gameplay (labels play_*, status, inventory, back_in_play, pause, resumed) must match pixel for pixel
within --max-diff levels (default 0: the loads write the same bits). The direct run must have loaded directly (the
texture cache logs its first direct load) and the staging run must not have. Reads logs and captures, writes nothing.
"""
import argparse
import importlib.util
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location("capture_check", os.path.join(ROOT, "tools", "capture_check.py"))
cc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cc)
GAMEPLAY = ("play_start", "play_walk", "play_turn", "status", "inventory", "back_in_play", "pause", "resumed")


def gameplay_tick(run):
    for line in open(os.path.join(run, "kuf_cod.log"), errors="replace"):
        m = re.search(r"\[kuf-input\] context: gameplay \(tick (\d+)", line)
        if m:
            return int(m.group(1))
    return None


def captures(run):
    d = os.path.join(run, "user", "captures")
    out = {}
    for line in open(os.path.join(d, "captures.jsonl")):
        e = json.loads(line)
        if e.get("label") in GAMEPLAY and e.get("ok", True):
            out[e["label"]] = os.path.join(d, e["file"])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("staging_run")
    ap.add_argument("direct_run")
    ap.add_argument("--max-diff", type=int, default=0)
    a = ap.parse_args()
    ok = True
    ta, tb = gameplay_tick(a.staging_run), gameplay_tick(a.direct_run)
    if ta is None or ta != tb:
        print(f"[FAIL] anchor: gameplay entered at tick {ta} and {tb}: the captures are not of the same game state "
              "(rerun; not a rendering result)")
        return 1
    print(f"[ok] anchor: both entered gameplay at tick {ta}")
    marker = "loading resolution-scaled textures directly into images"
    used = [marker in open(os.path.join(r, "kuf_cod.log"), errors="replace").read() for r in (a.staging_run, a.direct_run)]
    if used != [False, True]:
        print(f"[FAIL] direct loads used (staging run, direct run): {used}, expected [False, True]")
        return 1
    print("[ok] the direct run loaded directly, the staging run did not")
    ca, cb = captures(a.staging_run), captures(a.direct_run)
    missing = [l for l in GAMEPLAY if l not in ca or l not in cb]
    if missing:
        print(f"[FAIL] captures missing: {missing}")
        return 1
    for label in GAMEPLAY:
        x, _ = cc.read_png(ca[label])
        y, _ = cc.read_png(cb[label])
        if x.shape != y.shape:
            print(f"[FAIL] {label}: sizes {x.shape} and {y.shape}")
            ok = False
            continue
        diff = int(abs(x.astype("int16") - y.astype("int16")).max())
        good = diff <= a.max_diff
        ok &= good
        print(f"[{'ok' if good else 'FAIL'}] {label}: largest difference {diff} (allowed {a.max_diff})")
    print("DIRECT LOAD CHECK:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
