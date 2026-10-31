#!/usr/bin/env python3
"""Native rendering check (M6C follow-up, KNOWN_ISSUES K31): compares a run rendered at a render scale with the
reference run of the same output rendered at 1x (tests/display/render_*.toml, run by tools/menu_test.py with the
GPU trace of SDK patch 0009). Both runs CONTINUE from the same save and capture the same scripted moments.

  python3 -I tests/display/render_compare.py --ref build/runs/ctest-display-render_ref_16_9 \\
          --run build/runs/ctest-display-render_native_4k --scale 3x3

Checks (one line each, exit 1 if any fails):
  anchor      both runs traced the same game moment (KNOWN_ISSUES K35): in each run the frame traced is the tick frame
              after the script's `trace` (the app logs "[kuf-trace] GPU trace of tick frame F (requested at tick R,
              ...): swap S"; F = R + 1, and the GPU trace's frame is swap S), and it is the same
              number of ticks after the tick in which the run entered gameplay ("context: gameplay (tick T, ...)") in
              both runs (a swap or frame number alone is a different moment in different runs: loading takes a
              different number of frames)
  internal    the run's log names the render scale and its frame (1280x720 times the scale); its captures have
              that size and the reference's are 1280x720
  resolves    the traced gameplay frame made the same resolves in both runs (addresses, sizes, formats), the run's
              all to the scaled resolve buffer with exactly scale_x * scale_y times the data, the reference's not. The
              game's bloom alternates between two buffers by frame parity, and the frame traced at a game moment
              (anchor) can have the other parity in the other run: the sequences may then differ by exchanging that
              one pair of buffers (both used by the reference, same sizes and formats), which is reported
  cube maps   the cube textures the game renders at level load (its lighting) are, in every scale_x x scale_y
              block of the run's, exactly the reference's texel: bit for bit (K31: they came out wrong, red)
  hdr         the traced frame's floating-point resolves (the HDR scene, its downsampled and blurred copies, the
              composited image): per-channel means within 3 % (relative) or 0.004 of the reference's
  colours     the captures reduced to 1280x720 (block averages): per-channel mean within 1.5 levels of the
              reference's, mean absolute difference of 8x8 block averages within 4 levels
The images and dumps stay in the run folders (game content, not committed); only statistics are printed.
"""
import argparse
import glob
import json
import os
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from capture_check import GUEST_HEIGHT, GUEST_WIDTH, block_average, read_png, to_guest  # noqa: E402

RESOLVE = re.compile(r"\[gputrace\] swap (\d+) resolve (\d+) before_draw \d+ written ([0-9A-F]{8})\+([0-9A-F]{8}) "
                     r"scaled (true|false) copy_control ([0-9A-F]{8}) dest_info ([0-9A-F]{8})")
CUBE = re.compile(r"cube_(\d+)_([0-9A-F]{8})_f(\d+)\.bin$")
TEXTURE = re.compile(r"\[gputrace\] texture load (\d+) base ([0-9A-F]{8}) format (\d+) (\d+)x(\d+)x(\d+)")
COLOR_FORMAT_16_16_16_16_FLOAT = 32  # Xenos ColorFormat (RB_COPY_DEST_INFO bits 7-12)
ANCHOR = re.compile(r"\[kuf-trace\] GPU trace of tick frame (\d+) \(requested at tick (\d+), game counter (\d+)\): "
                    r"swap (\d+)")
TRACED_SWAP = re.compile(r"\[gputrace\] swap (\d+) (?:draw|resolve) ")
GAMEPLAY = re.compile(r"\[kuf-input\] context: gameplay \(tick (\d+), frame \d+\)")


class Report:
    def __init__(self):
        self.lines, self.failed = [], False

    def add(self, name, ok, detail):
        self.lines.append(f"  [{'ok' if ok else 'FAIL'}] {name}: {detail}")
        self.failed |= not ok


def log_text(run):
    with open(os.path.join(run, "kuf_cod.log"), errors="replace") as f:
        return f.read()


def captures(run):
    """{label: (path, width, height)} of the run's labelled captures."""
    out = {}
    index = os.path.join(run, "user", "captures", "captures.jsonl")
    if os.path.exists(index):
        for line in open(index):
            r = json.loads(line)
            if r.get("ok") and r.get("label"):
                out[r["label"]] = (os.path.join(run, "user", "captures", r["file"]), r["width"], r["height"])
    return out


def traced_resolves(run, log):
    """The first traced frame's resolves: [(index, address, length, scaled, dest_info, dump path)]."""
    rows = [m.groups() for m in RESOLVE.finditer(log)]
    if not rows:
        return []
    swap = rows[0][0]
    out = []
    for s, i, addr, length, scaled, _, dest in rows:
        if s != swap:
            break
        dump = os.path.join(run, "user", f"resolve_s{s}_{int(i):03d}_{addr}.bin")
        out.append((int(i), addr, int(length, 16), scaled == "true", int(dest, 16), dump))
    return out


def cube_dumps(run, log):
    """{(address, format): (path, face width)} of the cube textures the run loaded (the last load of each)."""
    widths = {int(m.group(1)): int(m.group(4)) for m in TEXTURE.finditer(log)}
    out = {}
    for path in sorted(glob.glob(os.path.join(run, "user", "cube_*.bin"))):
        m = CUBE.search(path)
        out[(m.group(2), int(m.group(3)))] = (path, widths.get(int(m.group(1))))
    return out


def channel_means_f16(path):
    v = np.fromfile(path, dtype=">f2").astype(np.float32).reshape(-1, 4)
    v = v[np.all(np.isfinite(v), axis=1)]
    nz = np.any(v != 0, axis=1)
    return v[nz].mean(axis=0) if nz.any() else v.mean(axis=0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True, help="the reference run (Render 1x, same output)")
    ap.add_argument("--run", required=True, help="the run rendered at the scale")
    ap.add_argument("--scale", required=True, help="the run's render scale, XxY")
    ap.add_argument("--cube-ref", help="the run whose cube maps are the reference (default --ref)")
    args = ap.parse_args()
    sx, sy = (int(v) for v in args.scale.lower().split("x"))
    ref_log, run_log = log_text(args.ref), log_text(args.run)
    rep = Report()

    # The traced game moment (K35).
    moments, problems = [], []
    for name, log in (("reference", ref_log), ("run", run_log)):
        found = ANCHOR.findall(log)
        traced = TRACED_SWAP.search(log)
        gameplay = GAMEPLAY.search(log)
        if len(found) != 1 or not traced or not gameplay:
            problems.append(f"{name}: {len(found)} trace lines, traced swap {traced.group(1) if traced else None}, "
                            f"gameplay entry {gameplay.group(1) if gameplay else None}")
            continue
        frame, requested, _, swap = (int(v) for v in found[0])
        if frame != requested + 1 or int(traced.group(1)) != swap:
            problems.append(f"{name}: tick frame {frame} requested at {requested}, swap {swap}, traced {traced.group(1)}")
        moments.append((frame - int(gameplay.group(1)), frame, swap))
    if len(moments) == 2 and moments[0][0] != moments[1][0]:
        problems.append(f"gameplay + {moments[0][0]} in the reference, + {moments[1][0]} in the run")
    rep.add("anchor", not problems,
            "; ".join(problems) if problems else
            f"gameplay + {moments[0][0]} in both runs (reference tick frame {moments[0][1]} swap {moments[0][2]}, "
            f"run tick frame {moments[1][1]} swap {moments[1][2]})")

    # Internal resolution.
    fw, fh = GUEST_WIDTH * sx, GUEST_HEIGHT * sy
    want = f"render scale {sx}x{sy} (frame {fw}x{fh})"
    ref_caps, run_caps = captures(args.ref), captures(args.run)
    sizes = {k: (w, h) for k, (_, w, h) in run_caps.items()}
    ref_sizes = {k: (w, h) for k, (_, w, h) in ref_caps.items()}
    rep.add("internal", want in run_log and "render scale 1x1 (frame 1280x720)" in ref_log and run_caps
            and all(s == (fw, fh) for s in sizes.values())
            and all(s == (GUEST_WIDTH, GUEST_HEIGHT) for s in ref_sizes.values()),
            f"run: '{want}' {'logged' if want in run_log else 'NOT logged'}, captures {sorted(set(sizes.values()))}; "
            f"reference captures {sorted(set(ref_sizes.values()))}")

    # Resolves of the traced frame.
    ref_res, run_res = traced_resolves(args.ref, ref_log), traced_resolves(args.run, run_log)
    ref_seq = [(a, l, d) for _, a, l, _, d, _ in ref_res]
    run_seq = [(a, l, d) for _, a, l, _, d, _ in run_res]
    same, exchanged = ref_seq == run_seq, None
    if not same and len(ref_seq) == len(run_seq):
        differing = sorted({(x[0], y[0]) for x, y in zip(ref_seq, run_seq) if x != y})
        if len(differing) == 2 and differing[0] == differing[1][::-1]:
            pair = {differing[0][0]: differing[0][1], differing[0][1]: differing[0][0]}
            same = [(pair.get(a, a), l, d) for a, l, d in ref_seq] == run_seq
            exchanged = differing[0] if same else None
    problems = []
    for i, addr, length, scaled, _, dump in run_res:
        size = os.path.getsize(dump) if os.path.exists(dump) else -1
        if not scaled or size != length * sx * sy:
            problems.append(f"{i} {addr}: scaled={scaled} {size} bytes (want {length * sx * sy})")
    for i, addr, length, scaled, _, dump in ref_res:
        size = os.path.getsize(dump) if os.path.exists(dump) else -1
        if scaled or size != length:
            problems.append(f"reference {i} {addr}: scaled={scaled} {size} bytes (want {length})")
    rep.add("resolves", bool(run_res) and same and not problems,
            f"{len(run_res)} resolves in the traced frame (reference {len(ref_res)}), same sequence: {same}"
            + (f" (bloom buffers {exchanged[0]} and {exchanged[1]} exchanged: the other frame parity)" if exchanged
               else "")
            + (f"; {problems[:4]}" if problems else f"; each {sx}x{sy} times the data, in the scaled buffer"))

    # The lighting cube maps (and any other cube made from a resolve).
    cube_ref = args.cube_ref or args.ref
    ref_cubes = cube_dumps(cube_ref, ref_log if cube_ref == args.ref else log_text(cube_ref))
    run_cubes = cube_dumps(args.run, run_log)
    checked, bad = 0, []
    for key, (run_path, _) in sorted(run_cubes.items()):
        ref_path, side = ref_cubes.get(key, (None, None))
        if not ref_path or not side:
            continue
        a = np.fromfile(ref_path, dtype=np.uint8)
        b = np.fromfile(run_path, dtype=np.uint8)
        if b.size == a.size:
            continue  # not made from a resolve (loaded from the game's files): not scaled
        if b.size != a.size * sx * sy:
            bad.append(f"{key[0]}: {b.size} bytes, want {a.size * sx * sy}")
            continue
        texel = a.size // (6 * side * side)
        ra = a.view(np.uint8).reshape(6, side, side, texel)
        rb = b.view(np.uint8).reshape(6, side * sy, side * sx, texel)
        mismatch = 0
        for oy in range(sy):
            for ox in range(sx):
                mismatch += int(np.any(rb[:, oy::sy, ox::sx] != ra, axis=-1).sum())
        checked += 1
        if mismatch:
            bad.append(f"{key[0]}: {mismatch} of {ra.shape[0] * side * side * sx * sy} host texels differ")
    rep.add("cube maps", checked > 0 and not bad,
            f"{checked} cube maps built from resolves compared texel by texel" + (f"; {bad[:4]}" if bad else
                                                                                 ": identical"))

    # HDR resolves of the traced frame.
    hdr, hdr_bad = 0, []
    for (ri, ra_, rl, _, rd, rdump), (_, _, _, _, _, ndump) in zip(ref_res, run_res):
        if (rd >> 7) & 0x3F != COLOR_FORMAT_16_16_16_16_FLOAT or not (os.path.exists(rdump) and os.path.exists(ndump)):
            continue
        ma, mb = channel_means_f16(rdump), channel_means_f16(ndump)
        hdr += 1
        for c, name in enumerate("RGB"):
            if abs(ma[c] - mb[c]) > max(0.03 * abs(ma[c]), 0.004):
                hdr_bad.append(f"{ri} {ra_} {name}: {mb[c]:.4f} vs {ma[c]:.4f}")
    rep.add("hdr", hdr > 0 and not hdr_bad,
            f"{hdr} floating-point resolves (scene, downsampled, blurred, composited)"
            + (f"; {hdr_bad[:6]}" if hdr_bad else ": channel means match"))

    # Final images.
    col_bad, compared = [], 0
    for label in ("scene", "hud"):
        if label not in ref_caps or label not in run_caps:
            col_bad.append(f"{label}: missing capture")
            continue
        a, _ = read_png(ref_caps[label][0])
        b, _ = read_png(run_caps[label][0])
        b = to_guest(b)
        if b is None or a.shape != b.shape:
            col_bad.append(f"{label}: sizes do not reduce to the reference's")
            continue
        compared += 1
        means_a = a.reshape(-1, 3).astype(np.float64).mean(axis=0)
        means_b = b.reshape(-1, 3).astype(np.float64).mean(axis=0)
        blocks = float(np.abs(block_average(a, 8) - block_average(b, 8)).mean())
        detail = (f"{label}: means {np.round(means_b, 2).tolist()} vs {np.round(means_a, 2).tolist()}, "
                  f"8x8 block difference {blocks:.2f}")
        if np.any(np.abs(means_a - means_b) > 1.5) or blocks > 4.0:
            col_bad.append(detail)
        else:
            rep.lines.append(f"         {detail}")
    rep.add("colours", compared == 2 and not col_bad,
            f"{compared} captures compared at 1280x720" + (f"; {col_bad}" if col_bad else ": within tolerance"))

    print(f"RENDER COMPARE {os.path.basename(args.run.rstrip('/'))} ({sx}x{sy}) against "
          f"{os.path.basename(args.ref.rstrip('/'))}: {'FAIL' if rep.failed else 'PASS'}")
    print("\n".join(rep.lines))
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
