#!/usr/bin/env python3
"""K34 regression check (M6D D3b-0, docs/HIGH_FPS.md section 13, SDK patch 0010): frames drawn without the HUD's
draws on the host render target path (fbo) must show the scene, as on the EDRAM-exact path (fsi).

Runs: tests/render/k34_build_frames_fbo.toml and k34_build_frames_fsi.toml (tools/menu_test.py). Each holds the game
for 3 s (the input script's freeze) while render-only frames draw the last tick's scene again without its HUD
(--kuf_render_build_hud=false), and captures the tick's own frame (idle_tick) and three build frames (idle_1-3);
'start' and 'after' are build frames too (the captured frame after a tick's is a build's).

  python3 -I tests/render/k34_check.py --fbo build/runs/ctest-render-k34_build_frames_fbo \\
          --fsi build/runs/ctest-render-k34_build_frames_fsi

Checks (one line each, exit 1 if any fails):
  not black    every build frame on fbo has content: standard deviation over 5 levels (K34 gave all zeros)
  tick         each fbo build frame of the held tick against the tick's own frame, in 16x16 blocks (largest channel
               difference): median within 4 levels and mean within 6. The builds lack the HUD (a few per cent of
               the blocks); the scene is the same. A black frame differs by the scene's brightness (median about 18)
  fsi          each fbo capture against the same capture of the fsi run: mean absolute difference within 1.0 level,
               8x8 block means within 0.6 (measured 0.18-0.20 and 0.09-0.12; a black or partly wrong frame is far
               outside)
The images stay in the run folders (game content, not committed); only statistics are printed.
"""
import argparse
import json
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from capture_check import block_average, read_png  # noqa: E402

BUILD_FRAMES = ("start", "idle_1", "idle_2", "idle_3", "after")
HELD_FRAMES = ("start", "idle_1", "idle_2", "idle_3")  # built from the held tick ('after' is 100 ticks later)
ALL_FRAMES = ("idle_tick",) + BUILD_FRAMES


def captures(run):
    """{label: image (float64)} of the run's labelled captures."""
    out = {}
    index = os.path.join(run, "user", "captures", "captures.jsonl")
    if os.path.exists(index):
        for line in open(index):
            r = json.loads(line)
            if r.get("ok") and r.get("label"):
                out[r["label"]] = read_png(os.path.join(run, "user", "captures", r["file"]))[0].astype(np.float64)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fbo", required=True, help="the run on the host render target path")
    ap.add_argument("--fsi", required=True, help="the same scenario on the EDRAM-exact path")
    args = ap.parse_args()
    fbo, fsi = captures(args.fbo), captures(args.fsi)
    lines, failed = [], False

    def add(name, ok, detail):
        nonlocal failed
        lines.append(f"  [{'ok' if ok else 'FAIL'}] {name}: {detail}")
        failed |= not ok

    missing = [k for k in ALL_FRAMES if k not in fbo] + [f"fsi {k}" for k in ALL_FRAMES if k not in fsi]
    add("captures", not missing, f"missing {missing}" if missing else f"{len(ALL_FRAMES)} captures in each run")
    if missing:
        print("K34 CHECK: FAIL")
        print("\n".join(lines))
        return 1

    stds = {k: float(fbo[k].std()) for k in BUILD_FRAMES}
    add("not black", all(s > 5.0 for s in stds.values()),
        ", ".join(f"{k} std {s:.1f}" for k, s in stds.items()))

    tick_blocks = block_average(fbo["idle_tick"], 16)
    details, ok = [], True
    for k in HELD_FRAMES:
        d = np.abs(block_average(fbo[k], 16) - tick_blocks)
        d = d.max(axis=-1) if d.ndim == 3 else d
        median, mean = float(np.median(d)), float(d.mean())
        ok &= median <= 4.0 and mean <= 6.0
        details.append(f"{k} median {median:.2f} mean {mean:.2f}")
    add("tick", ok, "16x16 block difference from the tick's frame: " + ", ".join(details))

    details, ok = [], True
    for k in ALL_FRAMES:
        mean = float(np.abs(fbo[k] - fsi[k]).mean())
        blocks = float(np.abs(block_average(fbo[k], 8) - block_average(fsi[k], 8)).mean())
        ok &= mean <= 1.0 and blocks <= 0.6
        details.append(f"{k} {mean:.2f}/{blocks:.2f}")
    add("fsi", ok, "mean / 8x8 block difference from the EDRAM-exact path: " + ", ".join(details))

    print(f"K34 CHECK {os.path.basename(args.fbo.rstrip('/'))}: {'FAIL' if failed else 'PASS'}")
    print("\n".join(lines))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
