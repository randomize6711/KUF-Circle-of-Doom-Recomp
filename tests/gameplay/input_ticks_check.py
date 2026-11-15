#!/usr/bin/env python3
"""M15C: input_ticks_check.py <run> <run> ...: the game's input events between the marks, relative to the first press, are
the same in every run (tests/gameplay/input_ticks.toml at several frame rates). Prints each run's count; exit 1 on a difference."""
import re
import sys


def events(run):
    text = open(run + "/kuf_cod.log", errors="replace").read()
    s, e = text.find("log mark inputs"), text.find("log mark inputs_end")
    found = re.findall(r"\[kuf-game-input\] tick (\d+): user 0 event (\d+ (?:up|down))", text[s:e])
    if not found:
        return []
    start = int(found[0][0])
    return [(int(t) - start, ev) for t, ev in found]


def main():
    runs = sys.argv[1:]
    base = events(runs[0])
    failed = not base
    print(f"{runs[0]}: {len(base)} events")
    for r in runs[1:]:
        ev = events(r)
        same = ev == base
        print(f"{r}: {len(ev)} events, {'the same ticks' if same else 'DIFFERENT'}")
        if not same:
            failed = True
            for x, y in zip(base, ev):
                if x != y:
                    print(f"  first difference: {x} vs {y}")
                    break
    print("FAIL" if failed else "ok")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
