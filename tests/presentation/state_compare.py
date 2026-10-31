#!/usr/bin/env python3
"""The game's state per tick in two runs (M6D D3b-3, docs/HIGH_FPS.md section 15): presentation must not change it.

Runs: tools/menu_test.py with --kuf_trace_game_state=true (per tick, at its scene build: the rand() seed and every
listed object's position as the game wrote it) and --kuf_trace_camera_orbit=true (per camera update: the simulation
camera, as the game and the modern camera computed it). Ticks are aligned by the tick in which each run entered
gameplay ("context: gameplay (tick T, ...)").

  python3 -I tests/presentation/state_compare.py RUN_A RUN_B [--ticks N] [--shift S] [--expect-different]

Checks (exit 1 if any fails), over the first N ticks of gameplay (default 600: past that, runs of the same build may
part through the game's own timing, docs/HIGH_FPS.md 14.4):
  seed     the game thread's rand() seed equal at every tick
  objects  the same objects at every tick, each at exactly the same position (the trace's 6 decimals). Objects are
           paired at their first tick by class and position (within 0.01 units), not by their renderer object index:
           the level load does not always number them alike (runs with render-only frames sometimes number the
           distant scenery one higher, docs/HIGH_FPS.md 15.5); every object of each run must have its pair
  camera   the simulation camera equal at every tick: the view object (class 0x82102A90) and the camera trace's eye,
           pivot, pitch and distance. Since K43 the modern camera is timed by the tick, not the host clock, so runs of
           the same route are identical; before, they differed by 0.05-0.4 units with the host's load and the player
           by up to 0.0002 (docs/KNOWN_ISSUES.md K43)
--shift S compares each tick of RUN_A with the tick S later in RUN_B; with --expect-different (inverted exit
status) it shows that the comparison catches a game only S ticks apart. --expect-different-in CHECK requires that
check (seed, objects or camera) to fail: the camera timed by the host clock again (--kuf_camera_host_dt) changes the
player's position by about 0.0001 units, which the comparison must catch.
Only statistics are printed.
"""
import argparse
import os
import re
import sys

STATE = re.compile(r"\[kuf-state\] tick (\d+) counter \d+ seed (\w+) objects (\d+):(.*)$")
OBJECT = re.compile(r"(\d+):(\w+):(-?[\d.]+|nan|-nan|inf|-inf),(-?[\d.]+|nan|-nan|inf|-inf),(-?[\d.]+|nan|-nan|inf|-inf)")
VIEW_CLASS = "82102A90"  # the gameplay view: the camera's matrix
PAIR_LIMIT = 0.01  # units: an object of the other run at the same position (pairing only)
ORBIT = re.compile(r"\[kuf-cam\] orbit: pitch (-?[\d.]+) deg, distance ([\d.]+),.* eye \((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\), "
                   r"pivot \((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")
GAMEPLAY = re.compile(r"context: gameplay \(tick (\d+)")


def log_lines(run):
    """The run's log, rotated files first (a run with a large trace fills more than one)."""
    for name in ("kuf_cod.2.log", "kuf_cod.1.log", "kuf_cod.log"):
        path = os.path.join(run, name)
        if os.path.exists(path):
            yield from open(path, errors="replace")


def load(run):
    entry, states, cameras, pending = None, {}, {}, []
    for line in log_lines(run):
        if entry is None:
            m = GAMEPLAY.search(line)
            if m:
                entry = int(m.group(1))
        m = ORBIT.search(line)
        if m:
            pending.append(tuple(float(x) for x in m.groups()))
            continue
        m = STATE.search(line)
        if m:
            tick = int(m.group(1))
            objects = {(int(i), c): (float(x), float(y), float(z)) for i, c, x, y, z in OBJECT.findall(m.group(4))}
            states[tick] = (m.group(2), objects)
            cameras[tick] = pending
            pending = []
    if entry is None:
        sys.exit(f"{run}: no gameplay entry in the log")
    return entry, states, cameras


def pair_objects(a, b):
    """{key in a: key in b}: each object of a with the object of b of the same class at the same position (the
    view, which moves with the camera's timing, by its class alone)."""
    pairs, free = {}, set(b)
    for key, pos in sorted(a.items()):
        same = [o for o in free if o[1] == key[1] and (key[1] == VIEW_CLASS or close(pos, b[o], PAIR_LIMIT))]
        if same:
            match = min(same, key=lambda o: (o != key, max(abs(x - y) for x, y in zip(pos, b[o]))))
            pairs[key] = match
            free.discard(match)
    return pairs


def close(a, b, limit=0.0):
    return len(a) == len(b) and all(abs(x - y) <= limit or (x != x and y != y) for x, y in zip(a, b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_a")
    ap.add_argument("run_b")
    ap.add_argument("--ticks", type=int, default=600)
    ap.add_argument("--shift", type=int, default=0)
    ap.add_argument("--expect-different", action="store_true")
    ap.add_argument("--expect-different-in", choices=["seed", "objects", "camera"])
    args = ap.parse_args()
    ea, sa, ca = load(args.run_a)
    eb, sb, cb = load(args.run_b)
    first = {"seed": None, "objects": None, "camera": None}
    pairs, paired_b = {}, set()
    compared = 0
    worst_object = worst_camera = 0.0
    for k in range(args.ticks):
        ta, tb = ea + k, eb + k + args.shift
        if ta not in sa or tb not in sb:
            continue
        compared += 1
        (seed_a, obj_a), (seed_b, obj_b) = sa[ta], sb[tb]
        if seed_a != seed_b and first["seed"] is None:
            first["seed"] = k
        # New objects are paired when they first appear (with an object of the other run not yet paired).
        new_a = {x: obj_a[x] for x in obj_a if x not in pairs}
        new_b = {y: obj_b[y] for y in obj_b if y not in paired_b}
        for x, y in pair_objects(new_a, new_b).items():
            pairs[x] = y
            paired_b.add(y)
        unpaired = [x for x in obj_a if pairs.get(x) not in obj_b] + [y for y in obj_b if y not in paired_b]
        if unpaired and first["objects"] is None:
            first["objects"] = k
        for key in obj_a.keys() & pairs.keys():
            other = pairs[key]
            if other not in obj_b:
                continue
            d = max(abs(x - y) for x, y in zip(obj_a[key], obj_b[other]))
            if key[1] == VIEW_CLASS:
                worst_camera = max(worst_camera, d if d == d else 0.0)
                if not close(obj_a[key], obj_b[other]) and first["camera"] is None:
                    first["camera"] = k
                continue
            worst_object = max(worst_object, d if d == d else 0.0)
            if not close(obj_a[key], obj_b[other]) and first["objects"] is None:
                first["objects"] = k
        cam_a, cam_b = ca.get(ta, []), cb.get(tb, [])
        if len(cam_a) != len(cam_b):
            first["camera"] = first["camera"] if first["camera"] is not None else k
        for x, y in zip(cam_a, cam_b):
            worst_camera = max(worst_camera, max(abs(p - q) for p, q in zip(x, y)))
            if not close(x, y) and first["camera"] is None:
                first["camera"] = k
    ok = compared >= args.ticks * 0.9
    print(f"gameplay entered at tick {ea} / {eb}; {compared} ticks compared (of the first {args.ticks})")
    for name, value in first.items():
        good = value is None
        ok &= good
        same = "equal at every tick"
        print(f"  [{'ok' if good else 'FAIL'}] {name}: " +
              (same if good else f"first different {value} ticks into gameplay"))
    print(f"  largest object difference {worst_object:.6f}, camera {worst_camera:.6f}")
    renumbered = sum(1 for x, y in pairs.items() if x != y)
    if renumbered:
        print(f"  {renumbered} objects numbered differently by the level load (paired by class and position)")
    if args.expect_different_in:
        caught = compared >= args.ticks * 0.9 and first[args.expect_different_in] is not None
        print(f"STATE COMPARE (expected to differ in {args.expect_different_in}): " + ("caught" if caught else "NOT CAUGHT"))
        return 0 if caught else 1
    if args.expect_different:
        print("STATE COMPARE (expected to differ): " + ("caught" if not ok else "NOT CAUGHT"))
        return 0 if not ok else 1
    print("STATE COMPARE: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
