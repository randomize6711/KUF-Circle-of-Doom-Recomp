#!/usr/bin/env python3
"""The loot drops of the scripted fight under the test fixture (M6D K24, docs/KNOWN_ISSUES.md K24).

Runs: gameplay.combat_loot, combat_loot_native, combat_loot_120 (--kuf_test_drop_seed: each dead unit's drop decision,
sub_8256C280, starts from that seed and runs the game's own rolls and tables), and gameplay.combat_loot_no_drops
(--kuf_debug_skip_drop_decisions: the decisions skipped, the fault).

  python3 -I tests/gameplay/k24_check.py RUN
  python3 -I tests/gameplay/k24_check.py RUN --expect-no-loot

Checks (one line each, exit 1 if any fails):
  decisions  at least one drop decision, every one started from the test seed ([kuf-loot] lines)
  gold path  a Red Lizardman's decision (class 82012C00) took the game's gold path (6 rand() draws: four chance rolls,
             the amount, the pile), as the seed was chosen to give
  report     the run's menu test passed (its loot rule: the Gold number changed or items are listed)
With --expect-no-loot (the fault): decisions were skipped, the menu test failed on the loot rule and on nothing else
(the run, the script, EXP gained all passed): the loot rule catches a game that drops nothing.
"""
import argparse
import os
import re
import sys

DECISION = re.compile(r"\[kuf-loot\] tick \d+: drop decision for unit \w+ \(class (\w+)\) from the test seed (\w+) "
                      r"\(it met \w+\): (-?\d+) rand\(\) draws")
SKIPPED = re.compile(r"\[kuf-loot\] tick \d+: drop decision for unit \w+ \(class \w+\) skipped")
GOLD_CLASS, GOLD_DRAWS = "82012C00", 6


def log_lines(run):
    for name in ("kuf_cod.2.log", "kuf_cod.1.log", "kuf_cod.log"):
        path = os.path.join(run, name)
        if os.path.exists(path):
            yield from open(path, errors="replace")


def report(name, results):
    ok = True
    for check, good, text in results:
        print(f"  [{'ok' if good else 'FAIL'}] {check}: {text}")
        ok = ok and good
    print(f"K24 CHECK {name}: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--expect-no-loot", action="store_true")
    args = ap.parse_args()
    name = os.path.basename(args.run)
    try:
        menu = open(os.path.join(args.run, "menu-report.txt"), errors="replace").read()
    except OSError as e:
        return report(name, [("report", False, f"no menu-report.txt: {e}")])
    decisions, skipped = [], 0
    for line in log_lines(args.run):
        m = DECISION.search(line)
        if m:
            decisions.append((m.group(1), m.group(2), int(m.group(3))))
        elif SKIPPED.search(line):
            skipped += 1
    if args.expect_no_loot:
        fails = [l.strip() for l in menu.splitlines() if l.strip().startswith("[FAIL]")]
        only_loot = (len(fails) == 1 and fails[0].startswith("[FAIL] images:")
                     and fails[0].count("[FAIL]") == 2 and "any_of[loot]: 0 of 2" in fails[0])
        return report(name, [
            ("decisions", skipped > 0 and not decisions, f"{skipped} drop decisions skipped, {len(decisions)} made"),
            ("report", only_loot, "the menu test failed on the loot rule alone" if only_loot
             else f"failures: {fails or 'none (the loot rule did not catch the fault)'}"),
        ])
    seeds = {d[1] for d in decisions}
    gold = sum(1 for d in decisions if d[0] == GOLD_CLASS and d[2] == GOLD_DRAWS)
    lizard = sum(1 for d in decisions if d[0] == GOLD_CLASS)
    passed = re.search(r"^MENU TEST \S+: PASS", menu, re.M) is not None
    return report(name, [
        ("decisions", len(decisions) > 0 and len(seeds) == 1 and skipped == 0,
         f"{len(decisions)} drop decisions from the test seed {', '.join(sorted(seeds)) or '-'}"),
        ("gold path", gold > 0 and gold == lizard,
         f"{gold} of {lizard} Red Lizardman decisions took the gold path ({GOLD_DRAWS} draws)"),
        ("report", passed, "the menu test passed" if passed else "the menu test failed"),
    ])


if __name__ == "__main__":
    sys.exit(main())
