#!/usr/bin/env python3
"""Sandboxed smoke test: the game must initialise, reach and keep progressing through its main loop, read
its archives and exit cleanly, with bounded diagnostics. A timeout alone never passes.

Usage: smoke_test.py [--preset linux-amd64-audit] [--seconds 300] [--run-id smoke-<time>] [-- extra game args]

Runs tools/run_sandboxed.sh --gpu with --kuf_exit_after_seconds, then checks the run directory:
  exit            process exit status 0 via the timed-exit path (summary exit_reason), no crash report
  frames          every progress interval after warm-up presents >= 90 % of 30 fps (the original cap),
                  i.e. the main loop is running and not stalled
  archives        the boot archives, shader bundle, BGM banks and title movie were opened without failure
  timebase        guest timebase ticks at 50 MHz +/- 0.5 % against wall-clock time
  log             no [error]/[critical] runtime lines; run directory below 64 MB (not counting captures)
  gpu             (report only) GPU-subsystem warnings, grouped
  memory          resident memory after warm-up grows by at most max(MEMORY_GROWTH_MB, 10 %) to the end
  images          periodic captures (--capture-every, default every 300 frames) pass tools/capture_check.py
                  check against tests/capture/title_v1.toml: not blank, plausible, and the title/attract
                  movies keep changing for the whole run (no freeze)
  imports         (audit builds) every exercised import that is not "Fully implemented" in
                  config/v1/import_review.toml is listed; unreviewed partial/stub imports fail the test
Writes smoke-report.txt into the run directory; exit status 0 = PASS.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import known_errors  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REQUIRED_FILES = [
    r"d:\dependency.bin", r"d:\initres.sox", r"d:\initres.pak", r"d:\preloadres.sox", r"d:\preloadres.pak",
    r"d:\mainres.sox", r"d:\mainres.pak", r"d:\robbyres.sox", r"d:\robbyres.pak",
    r"d:\data\Material\CodShaders.ccz", r"d:\Data\Sound\SoundRes\71_BGM.xwb", r"d:\Data\Sound\SoundRes\71_BGM.xsb",
    r"d:\data\title\COD_TITLE_LOOP.wmv",
]
EXPECTED_FPS = 30
MEMORY_GROWTH_MB = 128
EXPECTATIONS = os.path.join(ROOT, "tests", "capture", "title_v1.toml")
GUEST_TICK_HZ = 50_000_000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="linux-amd64-audit")
    ap.add_argument("--seconds", type=int, default=300)
    ap.add_argument("--run-id", default="smoke-" + time.strftime("%Y%m%d-%H%M%S"))
    ap.add_argument("--capture-every", type=int, default=300, help="capture every N frames (0 = no image check)")
    ap.add_argument("extra", nargs="*", help="extra kuf_cod arguments (after --), e.g. for negative controls")
    args = ap.parse_args()

    run_dir = os.path.join(ROOT, "build", "runs", args.run_id)
    cmd = [os.path.join(ROOT, "tools", "run_sandboxed.sh"), "-p", args.preset, "-t", str(args.seconds + 60),
           "-n", args.run_id, "--gpu", "--", f"--kuf_exit_after_seconds={args.seconds}", *args.extra]
    if args.capture_every > 0:
        cmd.append(f"--kuf_capture_every={args.capture_every}")
    subprocess.run(cmd, check=False)

    results = []

    def check(name, ok, detail):
        results.append((name, bool(ok), detail))

    status = int(open(os.path.join(run_dir, "exit_status")).read().strip())
    summary_path = os.path.join(run_dir, "user", "run-summary.json")
    summary = json.load(open(summary_path)) if os.path.exists(summary_path) else {}
    crashed = os.path.exists(os.path.join(run_dir, "user", "crash-report.txt"))
    check("exit", status == 0 and not crashed and summary.get("exit_reason") == "kuf_exit_after_seconds",
          f"status={status} crash_report={crashed} exit_reason={summary.get('exit_reason')}")

    timeline = summary.get("frame_timeline", [])
    interval = 10
    steady = timeline[2:]  # first ~20 s: boot, shader compilation
    minimum = 0.9 * EXPECTED_FPS * interval
    expected_intervals = args.seconds // interval
    check("frames", len(timeline) >= expected_intervals - 1 and steady and min(steady) >= minimum,
          f"intervals={len(timeline)} min_after_warmup={min(steady) if steady else None} (need >= {minimum:.0f}) "
          f"total={summary.get('frames_presented')}")

    files = {f["path"]: f for f in summary.get("files", [])}
    missing = [p for p in REQUIRED_FILES if p not in files or files[p]["failures"]]
    failed = [p for p, f in files.items() if f["failures"]]
    check("archives", not missing, f"opened={len(files)} missing_or_failed={missing} all_failures={failed}")

    rates = summary.get("guest_tick_rate", [])[1:]
    bad = [r for r in rates if abs(r - GUEST_TICK_HZ) > 0.005 * GUEST_TICK_HZ]
    check("timebase", rates and not bad, f"samples={len(rates)} out_of_tolerance={bad}")

    cap_dir = os.path.join(run_dir, "user", "captures")
    size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(run_dir) for f in fs
               if not dp.startswith(cap_dir))
    gpu_warnings = {}
    log_lines = []
    errors = []
    for name in sorted(os.listdir(run_dir)):
        if name.startswith("kuf_cod") and name.endswith(".log"):
            for line in open(os.path.join(run_dir, name), errors="replace"):
                log_lines.append(line)
                m = re.search(r"\[warning\] \[gpu\] \[t\d+\] (.*)", line)
                if m:
                    key = re.sub(r"[0-9A-Fa-f]{6,}|\d+", "N", m.group(1))[:120]
                    gpu_warnings[key] = gpu_warnings.get(key, 0) + 1
    errors, known = known_errors.classify(log_lines)
    check("log", not errors and size < 64 * 1024 * 1024,
          f"run_dir={size / 1e6:.1f} MB (without captures) errors={errors[:5]}"
          + (f" known={known}" if known else ""))
    results.append(("gpu", True, f"{sum(gpu_warnings.values())} GPU warnings: "
                                 + ("; ".join(f"{n}x {k}" for k, n in gpu_warnings.items()) or "none")))

    rss = summary.get("rss_mb", [])
    if not rss or not any(rss):
        check("memory", False, "no rss_mb samples in run-summary.json")
    else:
        after_warmup = rss[min(5, len(rss) - 1):]  # from 60 s: boot loading and shader compilation done
        growth = after_warmup[-1] - min(after_warmup)
        allowed = max(MEMORY_GROWTH_MB, 0.10 * min(after_warmup))
        check("memory", growth <= allowed,
              f"rss {rss[0]} MB at 10 s, {min(after_warmup)}-{max(after_warmup)} MB after 60 s, end {rss[-1]} MB; "
              f"growth {growth} MB (allowed {allowed:.0f})")

    if args.capture_every > 0:
        out = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "capture_check.py"), "check", cap_dir,
                              EXPECTATIONS], capture_output=True, text=True).stdout
        fails = [l.strip() for l in out.splitlines() if "[FAIL]" in l]
        frames_line = next((l.strip() for l in out.splitlines() if "] index:" in l), "")
        check("images", out.startswith("CAPTURE CHECK") and not fails,
              (f"{frames_line[5:]}; " if frames_line else "") + ("; ".join(fails)[:600] if fails else
                                                                 "all capture checks passed (capture-report.txt)"))

    imports = summary.get("imports")
    if not summary:
        check("imports", False, "no run-summary.json")
    elif imports is None:
        check("imports", True, "not an audit build: import calls not recorded")
    else:
        review = tomllib.load(open(os.path.join(ROOT, "config", "v1", "import_review.toml"), "rb"))
        exercised_not_full = []
        for e in sorted(imports, key=lambda e: e["first_call_s"]):
            r = review.get(e["name"], {})
            if r.get("status") in ("Partially implemented", "Intentionally stubbed"):
                exercised_not_full.append(f"{e['name']} ({r['status']}, {e['calls']} calls)")
        # An import only counts as unreviewed if the static scan flags it; see tools/import_audit_report.py.
        out = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "import_audit_report.py"),
                              os.path.join(ROOT, "config", "v1", "imports.txt"),
                              os.path.join(ROOT, "thirdparty", "rexglue-sdk"), summary_path, "--reviewed",
                              os.path.join(ROOT, "config", "v1", "import_review.toml")],
                             capture_output=True, text=True).stdout
        unreviewed = [l.split()[2] for l in out.splitlines()
                      if l.strip().startswith(("Partially", "Not yet")) and l.split()[2] not in review]
        check("imports", not unreviewed,
              f"called={len(imports)} exercised partial/stubbed={exercised_not_full} unreviewed={unreviewed}")

    passed = all(ok for _, ok, _ in results)
    lines = [f"KUF-COD SMOKE TEST: {'PASS' if passed else 'FAIL'} ({args.seconds}s, preset {args.preset})"]
    lines += [f"  [{'ok' if ok else 'FAIL'}] {name}: {detail}" for name, ok, detail in results]
    report = "\n".join(lines) + "\n"
    open(os.path.join(run_dir, "smoke-report.txt"), "w").write(report)
    print(report, end="")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
