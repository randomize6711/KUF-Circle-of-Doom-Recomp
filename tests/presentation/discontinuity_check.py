#!/usr/bin/env python3
"""Discontinuities of the presented camera and player (M6D D3b-3, docs/HIGH_FPS.md section 15.5).

Runs: game runs with --kuf_frame_rate=120 --kuf_trace_presentation=true (gameplay.new_game_120: a new game, its
opening cameras and the level loads; gameplay.combat_loot_120: the scripted fight).

  python3 -I tests/presentation/discontinuity_check.py build/runs/ctest-gameplay-120-new_game [...]
      [--expect-opening]

Checks per run (one line each, exit 1 if any fails), from each tick as written and every presented frame:
  cuts         no frame between two ticks across a cut is interpolated (camera or player)
  camera       no interpolated tick interval in which the camera's eye moved more than 600 units (a cut the rules
               missed: the new game's opening handover is 1,615, a pull-in that also tilted the view 840; the
               largest the modern camera makes in the fight, a collision pull-in, 313; the route's one-tick pitch
               flick, not in these runs, 567)
  player       no interpolated tick interval in which the player moved more than 150 units (the fight's largest
               step 77)
  identity     the player model changed (cut "object") at most once in gameplay (it stays the same character:
               another character coming close to the attach point in a fight must not take its place)
  opening      with --expect-opening (runs that start a new game): its opening's cuts the game does not flag as such,
               the handover between the game's camera and the modern camera and the game camera's change of shot
               (its step from tick to tick broken), each at least once
Only statistics are printed.
"""
import argparse
import math
import os
import re
import sys

VEC = r"\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)"
WRITTEN = re.compile(rf"\[kuf-present\] tick (\d+) as written: camera (?:yes|no) eye {VEC}.*?; player (?:none|{VEC}); "
                     r"camera cut ([^,]+), player cut ([^,;\s]+)")
FRAME = re.compile(r"\[kuf-present\] (?:tick|build) tick (\d+) alpha \S+ camera (\S+(?: \S+)?) eye .*?; player "
                   r"(?:none|\d+ (\S+) pos)")
CONTEXT = re.compile(r"\[kuf-input\] context: (\S+) \(tick (\d+)")
CAMERA_LIMIT, PLAYER_LIMIT, IDENTITY_LIMIT = 600.0, 150.0, 1


def check(run, expect_opening):
    written, frames_bad, contexts = {}, 0, []
    cut_counts = {}
    for line in open(os.path.join(run, "kuf_cod.log"), errors="replace"):
        m = CONTEXT.search(line)
        if m:
            contexts.append((int(m.group(2)), m.group(1)))
            continue
        m = WRITTEN.search(line)
        if m:
            g = m.groups()
            eye = tuple(map(float, g[1:4]))
            pos = tuple(map(float, g[4:7])) if g[4] else None
            camera_cut, player_cut = g[7].strip(), g[8].strip()
            written[int(g[0])] = (eye, pos, camera_cut, player_cut)
            for kind, cut in (("camera", camera_cut), ("player", player_cut)):
                if cut != "-":
                    cut_counts[(kind, cut)] = cut_counts.get((kind, cut), 0) + 1
            continue
        m = FRAME.search(line)
        if m:
            w = written.get(int(m.group(1)))
            if w and ((w[2] != "-" and m.group(2) == "interpolated") or
                      (w[3] != "-" and m.group(3) == "interpolated")):
                frames_bad += 1

    def context(tick):
        c = "boot"
        for start, name in contexts:
            if start <= tick:
                c = name
        return c

    worst_camera = worst_player = (0.0, 0)
    identity = 0
    for n, (eye, pos, camera_cut, player_cut) in written.items():
        p = written.get(n - 1)
        if not p:
            continue
        if camera_cut == "-":
            d = math.dist(p[0], eye)
            worst_camera = max(worst_camera, (d, n))
        if player_cut == "-" and pos and p[1]:
            d = math.dist(p[1], pos)
            worst_player = max(worst_player, (d, n))
        if player_cut == "object" and p[1] and context(n) == "gameplay":
            identity += 1
    name = os.path.basename(run.rstrip("/"))
    results = [
        (frames_bad == 0, f"cuts: {frames_bad} frames interpolated across a cut"),
        (worst_camera[0] <= CAMERA_LIMIT,
         f"camera: largest interpolated eye step {worst_camera[0]:.1f} units at tick {worst_camera[1]} "
         f"(limit {CAMERA_LIMIT:.0f})"),
        (worst_player[0] <= PLAYER_LIMIT,
         f"player: largest interpolated step {worst_player[0]:.1f} units at tick {worst_player[1]} "
         f"(limit {PLAYER_LIMIT:.0f})"),
        (identity <= IDENTITY_LIMIT, f"identity: the player model changed {identity} times in gameplay "
                                     f"(limit {IDENTITY_LIMIT})"),
    ]
    if expect_opening:
        handovers, shots = cut_counts.get(("camera", "handover"), 0), cut_counts.get(("camera", "shot"), 0)
        results.append((handovers > 0 and shots > 0, f"opening: {handovers} handover and {shots} shot cuts"))
    lines = [f"  [{'ok' if ok else 'FAIL'}] {name} {text}" for ok, text in results]
    cuts = ", ".join(f"{k} {c}: {v}" for (k, c), v in sorted(cut_counts.items()))
    lines.append(f"  {name}: {len(written)} ticks; cuts {cuts}")
    if not written:
        lines.append(f"  [FAIL] {name}: no presentation trace")
        return lines, False
    return lines, all(ok for ok, _ in results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--expect-opening", action="store_true")
    args = ap.parse_args()
    ok = True
    for run in args.runs:
        lines, good = check(run, args.expect_opening)
        print("\n".join(lines))
        ok &= good
    print("DISCONTINUITY CHECK: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
