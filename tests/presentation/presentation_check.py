#!/usr/bin/env python3
"""Camera and player interpolation between ticks (M6D D3b-3, docs/HIGH_FPS.md section 15).

Runs: tests/presentation/presentation_*.toml (tools/menu_test.py, tests/presentation/presentation_route.kufscript)
with --kuf_trace_presentation=true (every presented frame: alpha, camera, player; every tick as written) and
--kuf_freeze_alpha_sweep=true (each freeze draws the held interval at alpha 0, 0.25, 0.5, 0.75 and 1 and captures it).

  python3 -I tests/presentation/presentation_check.py build/runs/ctest-presentation-presentation_120 [...]

Checks per run (one line each, exit 1 if any fails); frames drawn while the game is held (freezes) are left out of
the motion checks:
  summary      interpolation on; camera and player interpolated in most gameplay frames; no rand() or PhysX call
               and no restore failure in render-only builds
  game time    alpha within 0..1; the game time shown (tick - 1 + alpha) never goes back from one frame to the next
  aim          every interpolated modern-camera frame aimed at the interpolated pivot (within 0.05 degrees)
  walk         every frame shows the character on the path between the two ticks around it as the game wrote them, at
               its alpha (within 0.05 units); the camera (which moves around the pivot, not along a straight line) at
               its distance from the pivot on that path (within 0.05 units; after a collision pull-in, which the
               frames show at once, the second tick's distance) and its direction (within 0.2 degrees);
               the character and the camera's eye move on in every frame (no repeated frame), in one direction
  turn         6 degrees per tick with the mouse (the camera's own smoothing ramps it up over the first ticks): the
               view's direction on the path between the ticks (within 0.2 degrees), moving on in every frame, one way
  flick        270 degrees in one tick: every frame between the ticks that follow shows the view direction at its
               alpha on the path between the two ticks as the game wrote them, within 1 degree, and no tick-to-tick
               turn is shown as more than half a circle (a spin the game's camera does not make)
  orbit        looking up 3 degrees per tick: the view's elevation on the path between the ticks (within 0.2 degrees),
               rising in every frame
  centring     the right-stick click is a cut: the game's own turn is shown at once (no frames in between)
  sweep        per freeze: the five frames differ step by step (each step at least 0.3 levels mean), the middle one
               lies between the two ticks' images, and the frames drawn after it while held equal alpha 1 (within 0.1)
Only statistics are printed; the images stay in the run folders (game content, not committed).
"""
import json
import math
import os
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from capture_check import read_png  # noqa: E402

NUM = r"(-?[\d.]+|-?nan|-?inf)"
VEC = rf"\({NUM},{NUM},{NUM}\)"
FRAME = re.compile(rf"\[kuf-present\] (tick|build) tick (\d+) alpha ([\d.]+) camera (\S+(?: in)?) eye {VEC} az {NUM} "
                   rf"el {NUM} fov {NUM}(?: pivot {VEC} distance {NUM} aim {NUM})?; player (?:none|(\d+) (\S+(?: in)?) "
                   rf"pos {VEC} yaw {NUM})")
TICK = re.compile(r"\[kuf-present\] tick (\d+) as written: camera \S+ eye \(([^)]*)\) az (\S+) el (\S+?)(?: pivot \(([^)]*)\))?; player "
                  r"(none|\(([^)]*)\)); camera cut ([^,]+), player cut ([^,;]+)(?:, turn (\S+?))?(,|$)")
MARK = re.compile(r"\[kuf-input-script\] tick (\d+): .*log mark (\S+)")
FREEZE_START = re.compile(r"\[kuf-pacing\] freeze \S+: game held")
FREEZE_END = re.compile(r"\[kuf-pacing\] freeze \S+: rand\(\) calls")


def load(run):
    frames, ticks, marks, held = [], {}, {}, False
    for line in open(os.path.join(run, "kuf_cod.log"), errors="replace"):
        if FREEZE_START.search(line):
            held = True
            continue
        if FREEZE_END.search(line):
            held = False
            continue
        m = MARK.search(line)
        if m:
            marks[m.group(2)] = int(m.group(1))
            continue
        m = TICK.search(line)
        if m:
            eye = tuple(map(float, m.group(2).split(",")))
            pivot = tuple(map(float, m.group(5).split(","))) if m.group(5) else None
            ticks[int(m.group(1))] = dict(eye=eye, az=float(m.group(3)), el=float(m.group(4)),
                                          distance=(math.dist(eye, pivot),) if pivot else None,
                                          pos=tuple(map(float, m.group(7).split(","))) if m.group(7) else None,
                                          camera_cut=m.group(8), player_cut=m.group(9).strip(),
                                          turn=float(m.group(10)) if m.group(10) else None)
            continue
        m = FRAME.search(line)
        if not m:
            continue
        g = m.groups()
        f = dict(kind=g[0], tick=int(g[1]), alpha=float(g[2]), camera=g[3], eye=tuple(map(float, g[4:7])),
                 az=float(g[7]), el=float(g[8]), aim=float(g[14]) if g[14] else None, held=held,
                 distance=(float(g[13]),) if g[13] else None, pulled="; pulled in" in line,
                 player=g[16], pos=tuple(map(float, g[17:20])) if g[17] else None)
        interpolated = f["camera"] == "interpolated" or f["player"] == "interpolated"
        f["game"] = f["tick"] - 1 + f["alpha"] if interpolated else float(f["tick"])
        frames.append(f)
    return frames, ticks, marks


def segment(frames, marks, start, end):
    if start not in marks or end not in marks:
        return None
    return [f for f in frames if marks[start] < f["tick"] <= marks[end] and not f["held"]]


def on_path(name, seg, ticks, frame_value, tick_value, limit, angle=False, unit="", motion=None):
    """Each interpolated frame against the path between the two ticks around it as the game wrote them (the value
    at tick N - 1 plus alpha times the change to tick N; an angle the shorter way, or by the turn the camera
    reported); and every frame moves on from the one before (no frame repeats it) in one direction."""
    if not seg:
        return False, f"{name}: no frames"
    errors, signs, stalls, count = [], set(), 0, 0
    for f in seg:
        a, b = ticks.get(f["tick"] - 1), ticks.get(f["tick"])
        if not a or not b or frame_value(f) is None or tick_value(a) is None or tick_value(b) is None:
            continue
        va, vb, v = tick_value(a), tick_value(b), frame_value(f)
        if angle:
            d = b["turn"] if b["turn"] is not None else math.remainder(vb - va, 2 * math.pi)
            errors.append(abs(math.remainder(v - (va + d * f["alpha"]), 2 * math.pi)))
        elif f.get("pulled") and len(v) == 1:
            errors.append(math.dist(v, vb))  # a collision pull-in: the second tick's distance at once
        else:
            errors.append(math.dist(v, tuple(x + (y - x) * f["alpha"] for x, y in zip(va, vb))))
        count += 1
    motion = motion or frame_value
    for p, q in zip(seg, seg[1:]):
        vp, vq = motion(p), motion(q)
        if vp is None or vq is None:
            continue
        step = math.remainder(vq - vp, 2 * math.pi) if angle and motion is frame_value else math.dist(
            *(((vp,), (vq,)) if isinstance(vp, float) else (vp, vq)))
        if abs(step) <= 1e-6:
            stalls += 1
        elif angle and motion is frame_value:
            signs.add(step > 0)
    worst = max(errors) if errors else float("nan")
    shown = math.degrees(worst) if angle else worst
    ok = count >= 8 and worst <= limit and stalls == 0 and len(signs) <= 1
    direction = {frozenset([True]): ", direction +", frozenset([False]): ", direction -"}.get(frozenset(signs),
                                                                                            ", direction mixed" if signs else "")
    return ok, (f"{name}: {count} frames, largest distance from the path between the ticks {shown:.3f}{unit}, frames "
                f"repeating the one before {stalls}{direction}")


def mean_diff(a, b):
    return float(np.abs(a - b).mean())


def check_run(run):
    frames, ticks, marks = load(run)
    name = os.path.basename(run.rstrip("/"))
    lines, ok = [], True

    def report(good, text):
        nonlocal ok
        ok &= bool(good)
        lines.append(f"  [{'ok' if good else 'FAIL'}] {name}: {text}")

    summary = json.load(open(os.path.join(run, "user", "run-summary.json")))
    pr, pc = summary.get("presentation", {}), summary.get("pacing", {})
    play = segment(frames, marks, "start", "end") or []
    cam_share = sum(f["camera"] == "interpolated" for f in play) / max(1, len(play))
    pl_share = sum(f["player"] == "interpolated" for f in play) / max(1, len(play))
    good = (pr.get("interpolation") is True and cam_share > 0.9 and pl_share > 0.9 and pc.get("rand_in_builds") == 0
            and pc.get("physx_in_builds") == 0 and pc.get("restore_failures") == 0)
    report(good, f"summary: {len(play)} gameplay frames, camera interpolated in {cam_share:.1%}, player in "
                 f"{pl_share:.1%}; rand {pc.get('rand_in_builds')}, PhysX {pc.get('physx_in_builds')}, restore failures "
                 f"{pc.get('restore_failures')} in builds")

    moving = [f for f in frames if not f["held"]]
    back = sum(1 for a, b in zip(moving, moving[1:]) if b["game"] < a["game"] - 1e-9)
    alphas = all(0 <= f["alpha"] <= 1 for f in frames)
    report(back == 0 and alphas, f"game time: {len(moving)} frames, alpha within 0..1: {alphas}, steps back {back}")

    aims = [f["aim"] for f in play if f["aim"] is not None and f["camera"] == "interpolated"]
    report(bool(aims) and max(aims) <= 0.05, f"aim: {len(aims)} frames, largest {max(aims) if aims else float('nan'):.3f} "
                                             "degrees off the pivot")

    walk = segment(frames, marks, "walk", "walk_end")
    report(*on_path("walk (player)", walk, ticks, lambda f: f["pos"] if f["player"] == "interpolated" else None,
                    lambda t: t["pos"], 0.05, unit=" units"))
    camera = lambda f: f["camera"] == "interpolated"  # noqa: E731
    eye = lambda f: f["eye"] if camera(f) else None  # noqa: E731
    report(*on_path("walk (camera distance from the pivot)", walk, ticks,
                    lambda f: f["distance"] if camera(f) else None, lambda t: t["distance"], 0.05, unit=" units",
                    motion=eye))
    report(*on_path("walk (camera direction)", walk, ticks, lambda f: f["az"] if camera(f) else None,
                    lambda t: t["az"], math.radians(0.2), angle=True, unit=" degrees", motion=eye))
    az = lambda a, b: math.remainder(b["az"] - a["az"], 2 * math.pi)  # noqa: E731
    turn = segment(frames, marks, "turn", "turn_end")
    report(*on_path("turn", turn, ticks, lambda f: f["az"] if f["camera"] == "interpolated" else None,
                    lambda t: t["az"], math.radians(0.2), angle=True, unit=" degrees"))

    flick = [t for t in ticks if "flick" in marks and marks["flick"] < t <= marks.get("flick_end", 0)]
    if flick and all(t - 1 in ticks for t in flick):
        # The view between each two ticks as the game wrote them: the turn when the camera reports it (taken whole),
        # else the shorter way (the camera's smoothing turns less than half a circle per tick; the flick of 270
        # degrees to the right turns the camera 90 degrees to the left, the shorter way, over several ticks).
        errors, count, spin = [], 0, 0.0
        for t in flick:
            a, b = ticks[t - 1]["az"], ticks[t]["az"]
            d = ticks[t]["turn"] if ticks[t]["turn"] is not None else math.remainder(b - a, 2 * math.pi)
            spin = max(spin, abs(d))
            for f in frames:
                if f["tick"] == t and not f["held"] and f["camera"] == "interpolated":
                    count += 1
                    errors.append(abs(math.remainder(f["az"] - (a + d * f["alpha"]), 2 * math.pi)))
        report(count > 0 and max(errors) < math.radians(1) and spin < math.pi,
               f"flick: {len(flick)} ticks after a 270-degree flick, {count} frames between them; largest turn in a tick "
               f"{math.degrees(spin):.1f} degrees, largest difference from the path between the ticks "
               f"{math.degrees(max(errors)) if errors else float('nan'):.2f} degrees")
    else:
        report(False, "flick: no ticks in the flick segment")

    orbit = segment(frames, marks, "orbit", "orbit_end")
    report(*on_path("orbit", orbit, ticks, lambda f: f["el"] if f["camera"] == "interpolated" else None,
                    lambda t: t["el"], math.radians(0.2), angle=True, unit=" degrees"))

    centre = [t for t, v in ticks.items() if "centre" in marks and marks["centre"] < t <= marks.get("centred", 0)
              and v["camera_cut"] == "snapped"]
    between = [f for f in frames if centre and f["tick"] == centre[0] and f["camera"] == "interpolated"]
    report(len(centre) >= 1 and not between,
           f"centring: {len(centre)} tick(s) cut as 'snapped'; frames interpolated across it: {len(between)}")

    # The capture files by label; each sweep's eight are read when it is checked (all 72 of a 4K run as float64 took
    # 14 GB, enough to exhaust memory beside a game run: docs/TESTING.md).
    caps = {}
    index = os.path.join(run, "user", "captures", "captures.jsonl")
    if os.path.exists(index):
        for line in open(index):
            r = json.loads(line)
            if r.get("ok") and r.get("label"):
                caps[r["label"]] = os.path.join(run, "user", "captures", r["file"])
    for seg_name in ("walk", "turn", "orbit"):
        labels = [f"{seg_name}_a{p:03d}" for p in (0, 25, 50, 75, 100)]
        if not all(k in caps for k in labels):
            report(False, f"sweep {seg_name}: captures missing")
            continue
        img = [read_png(caps[k])[0].astype(np.float64) for k in labels]
        steps = [mean_diff(img[i], img[i + 1]) for i in range(4)]
        whole = mean_diff(img[0], img[4])
        mid = (mean_diff(img[2], img[0]), mean_diff(img[2], img[4]))
        held = [mean_diff(read_png(caps[k])[0].astype(np.float64), img[4])
                for k in (f"{seg_name}_1", f"{seg_name}_2", f"{seg_name}_3") if k in caps]
        good = min(steps) >= 0.3 and max(mid) < whole and held and max(held) <= 0.1
        report(good, f"sweep {seg_name}: alpha 0 to 1 {whole:.2f} levels mean, steps "
                     f"{' '.join(f'{s:.2f}' for s in steps)}, middle to the ends {mid[0]:.2f} / {mid[1]:.2f}, held "
                     f"frames to alpha 1 {max(held) if held else float('nan'):.3f}")
    return lines, ok


def main():
    runs = sys.argv[1:]
    if not runs:
        sys.exit(__doc__)
    all_ok = True
    for run in runs:
        lines, ok = check_run(run)
        all_ok &= ok
        print("\n".join(lines))
    print("PRESENTATION CHECK: " + ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
