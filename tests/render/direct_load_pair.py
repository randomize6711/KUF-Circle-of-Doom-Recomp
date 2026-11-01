#!/usr/bin/env python3
"""M9 (SDK patch 0013): run the direct load routes until an off / on pair is comparable, then check it.

  direct_load_pair.py --preset <preset> [--attempts 3]

tests/render/direct_load_off.toml and direct_load_on.toml are run alternately (off, on, off, on, ...), each attempt in
its own run directory (build/runs/ctest-direct_load-<n>-direct_load_{off,on}). The route's intro skip lands the
gameplay entry on tick 1386, 1387 or 1388 whatever the setting (load timing), and only runs that entered on the same
tick show the same game state. Even then, two staging runs of the M8 build can differ by up to 2 levels in play_turn,
back_in_play and resumed (two end states, mean difference 0.0001; docs/PERFORMANCE.md 8.5). So every off / on pair
that entered on the same tick is compared by direct_load_check.py (unchanged: the direct load marker, gameplay
captures identical) as runs arrive, and the test passes when one pair is identical. Fails if a run fails or no pair
was identical within --attempts runs of each.
"""
import argparse
import importlib.util
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("direct_load_check", os.path.join(HERE, "direct_load_check.py"))
check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="linux-amd64-relwithdebinfo")
    ap.add_argument("--attempts", type=int, default=3)
    a = ap.parse_args()
    runs = {"off": [], "on": []}
    for n in range(1, a.attempts + 1):
        for variant in ("off", "on"):
            prefix = f"ctest-direct_load-{n}"
            toml = os.path.join(HERE, f"direct_load_{variant}.toml")
            r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "menu_test.py"), "--preset",
                                a.preset, "--run-prefix", prefix, toml])
            if r.returncode != 0:
                print(f"[FAIL] direct_load_{variant} (attempt {n}) failed")
                return 1
            run = os.path.join(ROOT, "build", "runs", f"{prefix}-direct_load_{variant}")
            tick = check.gameplay_tick(run)
            print(f"attempt {n} {variant}: gameplay entered at tick {tick}")
            runs[variant].append((tick, run))
            new = runs[variant][-1]
            for t_other, other in runs["on" if variant == "off" else "off"]:
                if new[0] is not None and new[0] == t_other:
                    off, on = (new[1], other) if variant == "off" else (other, new[1])
                    print(f"comparing {os.path.basename(off)} and {os.path.basename(on)}")
                    sys.argv = [sys.argv[0], off, on]
                    if check.main() == 0:
                        return 0
    print(f"[FAIL] no off / on pair entered gameplay on the same tick with identical captures in {a.attempts} "
          f"attempts each: off ticks {[t for t, _ in runs['off']]}, on ticks {[t for t, _ in runs['on']]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
