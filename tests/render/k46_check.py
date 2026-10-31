#!/usr/bin/env python3
"""Released object indices in render-only builds (M6D K46, docs/HIGH_FPS.md 14.8).

Runs: render.k46_released_120 (--kuf_debug_restore_stale_every=30 at 120 fps: test records added to the tick's saved
scene setup list, an index whose renderer object entry is empty and an index now holding another object) and
render.k46_released_fault (the same with --kuf_debug_restore_released: the correction off).

  python3 -I tests/render/k46_check.py RUN
  python3 -I tests/render/k46_check.py RUN --expect-crash

Checks (one line each, exit 1 if any fails):
  run       no crash report, the run reached its end
  records   at least 100 test records added (run-summary restore_injected), every one left out (restore_dropped at
            least restore_injected: each record is left out by every build of its tick), no restore failure
  saves     every save the game committed (save message 0x41127: the Quit save and the new game's autosaves) had a
            tick with test records within 2 s after it (the 120 fps session crashed 17 ms after its save; a
            loading tick after an autosave can take over a second)
With --expect-crash (the fault test): the run crashed reading guest address 0 in sub_821A2FD8 (the scene setup list's
consumer), as the session did; the check fails if it did not (the regression test would not catch the fault).
"""
import argparse
import datetime
import json
import os
import re
import sys

STAMP = re.compile(r"^\[([\d-]+ [\d:.]+)\]")
RECORDS = "test records in the saved scene setup list"
COMMITTED = re.compile(r"\(save message 0x41127\) .*: committed")


def log_lines(run):
    for name in ("kuf_cod.2.log", "kuf_cod.1.log", "kuf_cod.log"):
        path = os.path.join(run, name)
        if os.path.exists(path):
            yield from open(path, errors="replace")


def when(line):
    m = STAMP.match(line)
    return datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() if m else None


def report(name, results):
    ok = True
    for check, good, text in results:
        print(f"  [{'ok' if good else 'FAIL'}] {check}: {text}")
        ok = ok and good
    print(f"K46 CHECK {name}: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--expect-crash", action="store_true")
    args = ap.parse_args()
    name = os.path.basename(args.run)
    crash_path = os.path.join(args.run, "user", "crash-report.txt")
    crash = open(crash_path, errors="replace").read() if os.path.exists(crash_path) else ""
    if args.expect_crash:
        good = "fault_guest: 0x00000000" in crash and "nearest_recompiled_function: sub_821A2FD8" in crash
        frame = next((l.strip() for l in crash.splitlines() if l.strip().startswith("#0")), "no crash report")
        return report(name, [("crash", good, f"the fault crashed in sub_821A2FD8 reading object 0 ({frame})")])
    results = [("run", not crash, "no crash report" if not crash else "crash report present")]
    try:
        pacing = json.load(open(os.path.join(args.run, "user", "run-summary.json")))["pacing"]
    except (OSError, ValueError, KeyError) as e:
        return report(name, results + [("records", False, f"no run summary: {e}")])
    injected, dropped = pacing.get("restore_injected", 0), pacing.get("restore_dropped", 0)
    failures = pacing.get("restore_failures", 0)
    results.append(("records", injected >= 100 and dropped >= injected and failures == 0,
                    f"{injected} test records added, {dropped} records left out, {failures} restore failures"))
    saves, records = [], []
    for line in log_lines(args.run):
        if RECORDS in line:
            records.append(when(line))
        elif COMMITTED.search(line):
            saves.append(when(line))
    covered = sum(1 for s in saves if any(r is not None and 0 <= r - s <= 2.0 for r in records))
    results.append(("saves", saves and covered == len(saves),
                    f"{covered} of {len(saves)} committed saves followed by test records within 2 s"))
    return report(name, results)


if __name__ == "__main__":
    sys.exit(main())
