#!/usr/bin/env python3
"""M8 stability harness: repeated sandboxed game runs, judged by process health and memory.

  python3 -I tools/stability.py --tag <tag> [--stages boot,world,...] [--reps-scale 1] [--preset ...] [--dry]
  python3 -I tools/stability.py --tag <tag> --report-only

Every run goes through tools/menu_test.py (tools/run_sandboxed.sh: read-only game folder, no network, no display or
audio sockets, fresh saves and settings in build/runs/<run-id>, hard timeout) inside a transient systemd user scope
with a memory and CPU limit; the memory-pressure stage gives the game a memory.high below its charge (reclaim and
swap under pressure, within that scope only; --memory-high). Save stages start from copies of the fixture save (build/m6-saves; read only).

A run passes when the game exited by its timed exit (status 0), wrote no crash report, the harness's run, script and
log checks passed (screen checks are not used: the menu routes of some scenarios predate the M7 menus) and the log has
no [error] line outside config/v1/known_log_errors.toml. The memory trend is resident memory from the run's telemetry
(every 10 s): growth between a quarter of the run and its end, and per minute for runs of 10 minutes or more.
Results: build/stability/<tag>/summary.json and summary.md.
"""

import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_spec = importlib.util.spec_from_file_location("known_errors", os.path.join(ROOT, "tools", "known_errors.py"))
known_errors = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(known_errors)

# name: (scenarios run in order as one group, repetitions, extra game args, extra scope properties, what it covers)
STAGES = {
    "boot": (["tests/perf/bench_menu.toml"], 5, [], [], "startup, renderer initialisation, title, main menu, shutdown"),
    "boot120": (["tests/perf/bench_menu.toml"], 2, ["--kuf_frame_rate=120"], [], "the same at 120 fps"),
    "world": (["tests/perf/bench_explore.toml"], 3, ["--kuf_frame_rate=120"], [],
              "CONTINUE: world load, play at 120 fps"),
    "save_load": (["tests/campaign/shadows_depths_continue.toml", "tests/campaign/shadows_depths_reload.toml"], 2, [],
                  [], "load a save, status/equipment/inventory/quest menus, save; a new process loads that save"),
    "unload_reload": (["tests/render/k46_released_120.toml"], 1, [], [],
                      "play at 120 fps, the Quit save, the title, a new game's autosaves (K46 fault injection on)"),
    "framerate": (["tests/stability/framerate_switch.toml"], 2, [], [], "frame-rate limit switched every 5 s in play"),
    "resize": (["tests/stability/resize_play.toml"], 2, [], [], "window resized every 4 s in play at 120 fps"),
    # memory.high below what the game's cgroup is charged (about 1.1 GB in play: anonymous memory and guest shared
    # memory; its RSS reads 1.4 GB, some of it charged elsewhere), so the kernel reclaims and swaps the game's memory
    # (--memory-high; the first M8 run used 1300 MB, above the charge, and saw no pressure).
    "pressure": (["tests/perf/bench_explore.toml"], 1, ["--kuf_frame_rate=120"], ["MemoryHigh={memory_high}"],
                 "world load and play with memory.high below the game's charge (reclaim and swap on the game only)"),
    "long": (["tests/stability/long_play_120.toml"], 1, [], [], "30 minutes of play at 120 fps"),
}


def run_group(stage, rep, scenarios, game_args, scope_props, preset, tag, dry, memory_high="800M"):
    prefix = f"stab-{tag}-{stage}-r{rep}"
    results = []
    for sc in scenarios:
        cmd = ["systemd-run", "--user", "--scope", "--quiet", "-p", "MemoryMax=10G", "-p", "CPUQuota=1200%"]
        for p in scope_props:
            cmd += ["-p", p.format(memory_high=memory_high)]
        cmd += ["--", sys.executable, "-I", os.path.join(ROOT, "tools", "menu_test.py"), "--preset", preset,
                "--run-prefix", prefix, "--game-arg=--kuf_profile=true"]
        cmd += [f"--game-arg={a}" for a in game_args]
        cmd.append(sc)
        name = os.path.splitext(os.path.basename(sc))[0]
        run_dir = os.path.join(ROOT, "build", "runs", f"{prefix}-{name}")
        print(f"[stability] {stage} r{rep}: {name}", flush=True)
        if dry:
            print("  " + " ".join(cmd))
            continue
        t0 = time.monotonic()
        subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        r = judge(run_dir)
        r.update({"stage": stage, "rep": rep, "scenario": sc, "wall_s": round(time.monotonic() - t0, 1)})
        print(f"  {'PASS' if r['pass'] else 'FAIL'}: {r['why']}", flush=True)
        results.append(r)
    return results


def judge(run_dir):
    r = {"run": os.path.basename(run_dir)}
    st = os.path.join(run_dir, "exit_status")
    r["exit_status"] = int(open(st).read().strip()) if os.path.exists(st) else None
    r["crash_report"] = os.path.exists(os.path.join(run_dir, "user", "crash-report.txt")) or os.path.exists(
        os.path.join(run_dir, "crash-report.symbolized.txt"))
    report = os.path.join(run_dir, "menu-report.txt")
    checks = {}
    if os.path.exists(report):
        checks = {m.group(2): m.group(1) == "ok"
                  for m in re.finditer(r"\[(ok|FAIL)\] (\w[\w ]*?):", open(report).read())}
    r["checks"] = checks
    lines = []
    log = os.path.join(run_dir, "kuf_cod.log")
    if os.path.exists(log):
        with open(log, errors="replace") as f:
            lines = f.readlines()
    unexpected, known = known_errors.classify(lines)
    r["unexpected_errors"] = unexpected[:20]
    r["unexpected_error_count"] = len(unexpected)
    r["known_errors"] = known
    r["warnings"] = sum("[warning]" in l for l in lines)
    summ = os.path.join(run_dir, "user", "run-summary.json")
    rss = []
    if os.path.exists(summ):
        with open(summ) as f:
            rss = [x for x in json.load(f).get("rss_mb", []) if x]
    r["rss_mb_end"] = rss[-1] if rss else None
    r["rss_mb_peak"] = max(rss) if rss else None
    if len(rss) >= 8:
        q = len(rss) // 4
        r["rss_growth_mb_after_quarter"] = rss[-1] - rss[q]
        minutes = (len(rss) - 1 - q) * 10 / 60.0
        r["rss_mb_per_min_after_quarter"] = (rss[-1] - rss[q]) / minutes if minutes >= 10 else None
    why = []
    if r["exit_status"] != 0:
        why.append(f"exit {r['exit_status']}")
    if r["crash_report"]:
        why.append("crash report")
    for k in ("run", "script", "log"):
        if not checks.get(k, False):
            why.append(f"{k} check failed")
    if unexpected:
        why.append(f"{len(unexpected)} unexpected error line(s): {unexpected[0][:120]}")
    r["pass"] = not why
    r["why"] = "; ".join(why) if why else (f"exit 0, RSS end {r['rss_mb_end']} MB, peak {r['rss_mb_peak']} MB, "
                                         f"{r['warnings']} warnings, known errors {sum(known.values())}")
    return r


def write_report(tag, results):
    out_dir = os.path.join(ROOT, "build", "stability", tag)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "summary.json"), "w") as f:
        json.dump(results, f, indent=1)
    md = ["| Stage | Run | Result | Exit | RSS end / peak MB | RSS growth after 1/4 (MB) | MB per min | Wall s | "
          "Notes |", "|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        per_min = r.get("rss_mb_per_min_after_quarter")
        per_min = f"{per_min:.2f}" if per_min is not None else "n/a"
        md.append(f"| {r['stage']} | {r['run']} | {'PASS' if r['pass'] else 'FAIL'} | {r['exit_status']} | "
                  f"{r['rss_mb_end']} / {r['rss_mb_peak']} | {r.get('rss_growth_mb_after_quarter', 'n/a')} | "
                  f"{per_min} | {r['wall_s']} | {'' if r['pass'] else r['why']} |")
    passed = sum(r["pass"] for r in results)
    md.append(f"\n{passed} of {len(results)} runs passed.")
    text = "\n".join(md)
    with open(os.path.join(out_dir, "summary.md"), "w") as f:
        f.write(text + "\n")
    print(text)
    return passed == len(results)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tag", required=True)
    ap.add_argument("--stages", default=",".join(STAGES))
    ap.add_argument("--reps-scale", type=float, default=1.0, help="multiply every stage's repetitions (at least 1)")
    ap.add_argument("--preset", default="linux-amd64-relwithdebinfo")
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--memory-high", default="800M", help="memory.high of the pressure stage's scope")
    ap.add_argument("--report-only", action="store_true")
    a = ap.parse_args()
    out = os.path.join(ROOT, "build", "stability", a.tag, "summary.json")
    if a.report_only:
        with open(out) as f:
            return 0 if write_report(a.tag, json.load(f)) else 1
    results = []
    for stage in a.stages.split(","):
        scenarios, reps, game_args, props, _ = STAGES[stage]
        for rep in range(1, max(1, int(round(reps * a.reps_scale))) + 1):
            results += run_group(stage, rep, scenarios, game_args, props, a.preset, a.tag, a.dry, a.memory_high)
            if not a.dry:
                write_report(a.tag, results)
    if a.dry:
        return 0
    return 0 if write_report(a.tag, results) else 1


if __name__ == "__main__":
    sys.exit(main())
