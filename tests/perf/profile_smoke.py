#!/usr/bin/env python3
"""Profiler smoke test (M6A): a profiled main-menu run (tests/perf/menu_idle.toml with --kuf_profile and
--kuf_profile_samples), then tools/profile_report.py on it. Checks that the profile is complete and sane:
frames recorded at the game's 30 fps pace, CPU per thread, GPU busy readings, CPU samples attributed to
recompiled guest code and to named threads. No performance thresholds beyond "presents at its normal pace".
Run: python3 -I tests/perf/profile_smoke.py [--preset P] [--run-prefix ctest-perf]"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="linux-amd64-relwithdebinfo")
    ap.add_argument("--run-prefix", default="ctest-perf")
    args = ap.parse_args()
    scenario = os.path.join(ROOT, "tests", "perf", "menu_idle.toml")
    run = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "menu_test.py"), "--preset", args.preset,
                          "--run-prefix", args.run_prefix, "--game-arg=--kuf_profile=true",
                          "--game-arg=--kuf_profile_samples=true", scenario], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode != 0:
        print("PROFILE SMOKE: FAIL (the workload itself failed)")
        return 1
    run_dir = os.path.join(ROOT, "build", "runs", f"{args.run_prefix}-menu_idle")
    out = os.path.join(run_dir, "profile-report.json")
    rep = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "profile_report.py"), run_dir,
                          "--from-s", "35", "--json", out, "--bin-dir",
                          os.path.join(ROOT, "build", args.preset, "bin")], capture_output=True, text=True)
    print(rep.stdout.strip())
    if rep.returncode != 0:
        print(f"PROFILE SMOKE: FAIL (profile_report: {rep.stderr.strip()})")
        return 1
    r = json.load(open(out))
    f, s = r["frames"], r.get("samples") or {}
    kinds = dict(s.get("by_category", []))
    threads = [g for g, _ in r["threads_cpu_percent"]]
    checks = [
        ("frames recorded", f.get("frames", 0) > 1500),
        ("presents at its normal pace (29.5-30.5 fps)", 29.5 <= f.get("avg_fps", 0) <= 30.5),
        ("CPU per thread recorded (guest threads and the GPU command processor)",
         "XThread (guest)" in threads and "GPU Commands" in threads),
        ("GPU busy recorded", r.get("gpu_busy", {}).get("readings", 0) > 100),
        ("CPU samples taken", s.get("samples", 0) > 10000),
        ("samples attributed to recompiled guest code", kinds.get("guest code (recompiled)", 0) > 100),
        ("samples in threads that block signals (SDL's) too", any(g.startswith("SDL") for g, _ in s.get("by_thread", []))),
    ]
    ok = True
    for name, passed in checks:
        print(f"  [{'ok' if passed else 'FAIL'}] {name}")
        ok &= passed
    print(f"PROFILE SMOKE: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
