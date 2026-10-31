#!/usr/bin/env python3
"""Summary of one sandboxed game session (a manual play session or a long stability run), read only.

  python3 -I tools/session_report.py build/runs/<run-id> [--json out.json]

From the run directory: user/run-summary.json (the game's telemetry: frames presented, process CPU and
resident memory per 10 s interval, src/runtime/telemetry.cpp), the log files (kuf_cod*.log), exit_status
and crash reports. Prints:
  duration and exit      run time, exit status (124 = sandbox timeout, 70 = crash report), exit reason
  frame rate             fps per interval: median, 5th percentile, minimum; intervals below 27 fps and the
                         longest run of them (the game targets 30)
  CPU                    process CPU % per interval (100 = one core): median and maximum
  memory                 resident memory at 60 s, maximum and at the end; growth after warm-up, as a whole
                         and as the slope of a least-squares line (MB per hour)
  log                    [error]/[critical] lines, unexpected ones grouped by message and known ones by
                         issue (config/v1/known_log_errors.toml); [warning] lines grouped by message
  saves                  the content commits the game made (time and what changed)
  contexts               how often the game switched between title, menu and gameplay
Nothing in the run directory is changed.
"""
import argparse
import glob
import json
import os
import re
import statistics
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import known_errors  # noqa: E402

INTERVAL_S = 10.0
WARMUP_INTERVALS = 6  # the first 60 s: boot loading and shader compilation
LOW_FPS = 27.0
TIMESTAMP = re.compile(r"^\[\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d)\.\d+\] ")
VARIABLE = re.compile(r"0x[0-9A-Fa-f]+|\b[0-9A-Fa-f]{8,}\b|\b\d+(\.\d+)?\b")


def message_key(line):
    """A log line without its timestamp and thread, with numbers and addresses replaced, for grouping."""
    line = TIMESTAMP.sub("", line.strip())
    line = re.sub(r"^\[\w+\] \[\w+\] \[t\d+\] ", "", line)
    return VARIABLE.sub("#", line)[:160]


def log_lines(run_dir):
    paths = sorted(glob.glob(os.path.join(run_dir, "kuf_cod*.log")), key=os.path.getmtime)
    for path in paths:
        with open(path, errors="replace") as f:
            yield from f


def slope_mb_per_hour(values):
    n = len(values)
    if n < 2:
        return 0.0
    xs = [i * INTERVAL_S / 3600.0 for i in range(n)]
    mx, my = sum(xs) / n, sum(values) / n
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, values)) / den if den else 0.0


def longest_run(flags):
    best = cur = 0
    for f in flags:
        cur = cur + 1 if f else 0
        best = max(best, cur)
    return best


def report(run_dir):
    out = {"run": os.path.basename(os.path.normpath(run_dir))}
    status_path = os.path.join(run_dir, "exit_status")
    out["exit_status"] = open(status_path).read().strip() if os.path.exists(status_path) else None
    out["crash_reports"] = sorted(os.path.basename(p) for p in glob.glob(os.path.join(run_dir, "crash-report*")))
    summary_path = os.path.join(run_dir, "user", "run-summary.json")
    summary = json.load(open(summary_path)) if os.path.exists(summary_path) else {}
    out["duration_s"] = summary.get("duration_s")
    out["exit_reason"] = summary.get("exit_reason")

    # The last interval is usually partial (the session ended inside it): left out of the statistics.
    frames = summary.get("frame_timeline", [])[:-1]
    fps = [f / INTERVAL_S for f in frames]
    played = fps[WARMUP_INTERVALS:] or fps
    if played:
        low = [v < LOW_FPS for v in played]
        out["fps"] = {"median": statistics.median(played),
                      "p5": sorted(played)[int(0.05 * (len(played) - 1))],
                      "min": min(played),
                      "intervals": len(played),
                      "below_27": sum(low),
                      "longest_low_run_s": longest_run(low) * INTERVAL_S}
    cpu = summary.get("cpu_percent", [])[:-1]
    if cpu:
        out["cpu_percent"] = {"median": statistics.median(cpu), "max": max(cpu)}
    rss = [v for v in summary.get("rss_mb", [])[:-1]]
    if rss:
        after = rss[WARMUP_INTERVALS:] or rss
        out["rss_mb"] = {"at_60s": after[0], "max": max(rss), "end": rss[-1],
                         "growth_after_warmup": rss[-1] - after[0],
                         "slope_mb_per_hour": round(slope_mb_per_hour(after), 1)}

    lines = list(log_lines(run_dir))
    unexpected, known = known_errors.classify(lines)
    out["errors_unexpected"] = Counter(message_key(l) for l in unexpected).most_common()
    out["errors_known"] = known
    out["warnings"] = Counter(message_key(l) for l in lines if "[warning]" in l).most_common(25)
    out["saves"] = [TIMESTAMP.match(l).group(1) + " " + l.split("[content] ", 1)[1].strip()[:150]
                    for l in lines if "[content] " in l and "): committed (" in l and TIMESTAMP.match(l)]
    out["contexts"] = dict(Counter(l.split("context: ", 1)[1].split(" (")[0]
                                   for l in lines if "[kuf-input] context: " in l))
    return out


def print_report(r):
    print(f"session {r['run']}: exit status {r['exit_status']} ({r['exit_reason']}), "
          f"{(r['duration_s'] or 0) / 60:.1f} min, crash reports: {r['crash_reports'] or 'none'}")
    if "fps" in r:
        f = r["fps"]
        print(f"  frame rate (after 60 s, {f['intervals']} x 10 s): median {f['median']:.1f}, 5th percentile "
              f"{f['p5']:.1f}, min {f['min']:.1f}; {f['below_27']} intervals below 27 fps, longest "
              f"{f['longest_low_run_s']:.0f} s")
    if "cpu_percent" in r:
        print(f"  CPU: median {r['cpu_percent']['median']:.0f} %, max {r['cpu_percent']['max']:.0f} % (100 = one core)")
    if "rss_mb" in r:
        m = r["rss_mb"]
        print(f"  memory: {m['at_60s']} MB at 60 s, max {m['max']} MB, end {m['end']} MB; growth after warm-up "
              f"{m['growth_after_warmup']:+d} MB, trend {m['slope_mb_per_hour']:+.1f} MB/h")
    print(f"  unexpected errors: {sum(n for _, n in r['errors_unexpected'])}")
    for msg, n in r["errors_unexpected"][:20]:
        print(f"    {n:6d}  {msg}")
    print(f"  known errors: {r['errors_known'] or 'none'}")
    print(f"  warnings (top {len(r['warnings'])} kinds):")
    for msg, n in r["warnings"]:
        print(f"    {n:6d}  {msg}")
    print(f"  saves committed: {len(r['saves'])}")
    for s in r["saves"]:
        print(f"    {s}")
    print(f"  contexts entered: {r['contexts']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run_dir")
    ap.add_argument("--json", help="also write the report as JSON to this path")
    args = ap.parse_args()
    if not os.path.isdir(args.run_dir):
        print(f"not a directory: {args.run_dir}", file=sys.stderr)
        return 2
    r = report(args.run_dir)
    print_report(r)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(r, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
