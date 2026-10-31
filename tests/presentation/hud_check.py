#!/usr/bin/env python3
"""HUD elements placed from the world (M6D D4b-4A, docs/HIGH_FPS.md 20).

Runs traced with --kuf_trace_hud_from / --kuf_trace_hud_ticks: every HUD entry of each tick's frame as the UI submitted
it ([kuf-hud] lines: its list, type and screen rectangle in the 1280x720 layout). Render-only frames draw the tick's
entries as they are, so an element the UI placed from a world position (a marker over a character, a label on an item)
would stay where the tick put it while the camera and the objects are interpolated.

  python3 -I tests/presentation/hud_check.py RUN [RUN ...] [--turn] [--max-step PX]
  python3 -I tests/presentation/hud_check.py --self-test

Checks per run (one line each, exit 1 if any fails):
  traced   at least 100 ticks traced
  moving   no element keeps its size and moves by more than --max-step pixels (default 2) on 3 or more of any 10
           consecutive ticks (paired by list, type and size, nearest first): an element placed from the world moves
           with the camera and the units on nearly every tick (on every second one if the UI placed it at 15 Hz). A
           gauge filling changes its size, not its place; the minimap's markers step by 1 pixel over seconds; a menu's
           highlight frame jumps between fixed slots now and then (the isolated jumps are listed, not failed)
  turn     with --turn: through the route's camera turn (its log marks "turn" to the next mark; the camera turns 6
           degrees per tick, the player stands) every rectangle stays the same
--self-test: the moving check on made-up ticks finds an element that moves with the camera, and passes one that does
not (the check can catch what it is for).
"""
import argparse
import collections
import os
import re
import sys

HUD = re.compile(r"\[kuf-hud\] tick (\d+) list (\d+) type (\d+) rect (-?\d+),(-?\d+),(-?\d+),(-?\d+)")
MARK = re.compile(r"\[kuf-input-script\] tick (\d+): .*log mark (\w+)")


def log_lines(run):
    for name in ("kuf_cod.2.log", "kuf_cod.1.log", "kuf_cod.log"):
        path = os.path.join(run, name)
        if os.path.exists(path):
            yield from open(path, errors="replace")


def largest_move(a, b):
    """The largest move of an element of the same list, type and size between two ticks' entries (and where)."""
    groups = collections.defaultdict(lambda: (collections.Counter(), collections.Counter()))
    for side, entries in ((0, a), (1, b)):
        for lst, typ, x0, y0, x1, y1 in entries:
            groups[(lst, typ, x1 - x0, y1 - y0)][side][(x0, y0)] += 1
    worst, where = 0, None
    for key, (ca, cb) in groups.items():
        if sum(ca.values()) != sum(cb.values()):
            continue  # appeared or went: not a move
        pa, pb = sorted((ca - cb).elements()), sorted((cb - ca).elements())
        for p in pb:
            if not pa:
                break
            q = min(pa, key=lambda q: abs(q[0] - p[0]) + abs(q[1] - p[1]))
            pa.remove(q)
            d = max(abs(q[0] - p[0]), abs(q[1] - p[1]))
            if d > worst:
                worst, where = d, (key, q, p)
    return worst, where


def check(ticks, max_step, window=10, most=2):
    """The jumps (ticks where an element moved by more than max_step) and the first run of more than `most` jumps of
    one list, type and size within `window` ticks (None if there is none)."""
    keys = sorted(ticks)
    jumps, recent = [], collections.defaultdict(list)
    for t0, t1 in zip(keys, keys[1:]):
        if t1 != t0 + 1:
            continue
        d, where = largest_move(ticks[t0], ticks[t1])
        if d <= max_step:
            continue
        jumps.append((t1, d, where))
        seen = [t for t in recent[where[0]] if t > t1 - window] + [t1]
        recent[where[0]] = seen
        if len(seen) > most:
            return jumps, (t1, d, where, len(seen))
    return jumps, None


def self_test():
    still = [(88, 2, 124, 538, 220, 634), (88, 3, 500, 20, 780, 40)]
    ticks_ok = {t: still + [(88, 2, 257, 645, 257 + 10 + t % 6, 649)]  # a gauge filling
                + [(88, 2, x, 156, x + 1, 196) for x in ((979, 1019) if t % 70 < 35 else (1019, 1059))]  # a highlight
                for t in range(200)}
    ticks_bad = {t: still + [(88, 2, 300 + 9 * t, 300, 316 + 9 * t, 316)] for t in range(200)}  # follows the camera
    ticks_bad15 = {t: still + [(88, 2, 300 + 9 * (t // 2), 300, 316 + 9 * (t // 2), 316)] for t in range(200)}  # 15 Hz
    ok_jumps, ok_run = check(ticks_ok, 2)
    ok = ok_jumps and ok_run is None and check(ticks_bad, 2)[1] and check(ticks_bad15, 2)[1]
    print(f"  [{'ok' if ok else 'FAIL'}] self-test: a still HUD, a gauge and a highlight jumping between slots every "
          f"35 ticks pass, a marker moving 9 px every tick or every second tick fails")
    print(f"HUD CHECK self-test: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="*")
    ap.add_argument("--turn", action="store_true")
    ap.add_argument("--max-step", type=int, default=2)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    failed = False
    for run in args.runs:
        ticks, marks = collections.defaultdict(list), []
        for line in log_lines(run):
            m = HUD.search(line)
            if m:
                ticks[int(m.group(1))].append(tuple(int(m.group(i)) for i in range(2, 8)))
                continue
            m = MARK.search(line)
            if m:
                marks.append((int(m.group(1)), m.group(2)))
        results = [("traced", len(ticks) >= 100, f"{len(ticks)} ticks traced, "
                    f"{max((len(v) for v in ticks.values()), default=0)} entries at most")]
        jumps, run_ = check(ticks, args.max_step)
        where = lambda j: (f"tick {j[0]}: list {j[2][0][0]} type {j[2][0][1]} size {j[2][0][2]}x{j[2][0][3]}, "  # noqa: E731
                           f"{j[2][1]} -> {j[2][2]}, {j[1]} px")
        results.append(("moving", run_ is None,
                        (f"an element moved on {run_[3]} of 10 ticks ({where(run_)})" if run_ else
                         f"no element moved with the world; {len(jumps)} isolated jumps of more than {args.max_step} px"
                         + (f" ({'; '.join(where(j) for j in jumps[:6])})" if jumps else ""))))
        if args.turn:
            turn = [i for i, (_, name) in enumerate(marks) if name == "turn"]
            if not turn or turn[0] + 1 >= len(marks):
                results.append(("turn", False, "no 'turn' log mark followed by another"))
            else:
                t0, t1 = marks[turn[0]][0], marks[turn[0] + 1][0]
                inside = [t for t in range(t0, t1 + 1) if t in ticks]
                same = all(sorted(ticks[t]) == sorted(ticks[inside[0]]) for t in inside) if inside else False
                results.append(("turn", len(inside) >= t1 - t0 and same,
                                f"ticks {t0}-{t1} (the camera turning): {len(inside)} traced, every rectangle the same: "
                                f"{same}"))
        ok = all(r[1] for r in results)
        for name, good, text in results:
            print(f"  [{'ok' if good else 'FAIL'}] {name}: {text}")
        print(f"HUD CHECK {os.path.basename(run)}: {'PASS' if ok else 'FAIL'}")
        failed = failed or not ok
    return 1 if failed or not args.runs else 0


if __name__ == "__main__":
    sys.exit(main())
