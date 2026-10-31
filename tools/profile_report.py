#!/usr/bin/env python3
"""Performance report of a run recorded with --kuf_profile=true (src/runtime/profiler.h), read only.

  python3 -I tools/profile_report.py build/runs/<run-id> [--from-s S] [--to-s S] [--top N] [--json out.json]
                                     [--bin-dir build/<preset>/bin]

From <run>/user/profile/:
  frames      frame time = time between the starts of two successive presents. Average fps, median, 95th and
              99th percentile, maximum; the worst sustained stretch (the slowest 1 s and 5 s windows by mean
              frame time). Present time: how long the game's present call took (it returns when the frame is
              queued, so it is not the GPU's time). Note: the game presents at most 30 fps (vsync interval 2),
              so at 30 fps frame time says nothing about how much faster the frame could have been made.
  threads     CPU seconds per thread, grouped by name (numbers stripped), and per 100 % = one core
  gpu         amdgpu busy percentage: mean, median, 95th percentile (the whole GPU, including the desktop)
  samples     where the CPU time goes (with --kuf_profile_samples=true): per thread group, per library or
              guest code, and per function; guest code is the recompiled sub_XXXXXXXX functions.
--from-s/--to-s limit everything to a time range (seconds since the first present); --from-context gameplay
starts 20 s after the game first entered gameplay (from the log).
"""
import argparse
import bisect
import csv
import glob
import json
import os
import re
import statistics
import struct
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAMPLE = struct.Struct("<QII")


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, int(round(p / 100.0 * (len(s) - 1))))]


def thread_group(name):
    """A thread name without the numbers and addresses that tell instances apart."""
    # Thread names are cut at 15 characters (/proc), so the "(F8000...)" handle may be partial or just "(".
    name = re.sub(r"\((F[0-9A-F]*\)?)?$", "", name).strip()
    name = re.sub(r"XThread[0-9A-F]{4,8}", "XThread (guest)", name)
    name = re.sub(r"\$\d+|\d+$", "", name).strip()
    return name or "?"


def read_frames(prof, t_from, t_to):
    with open(os.path.join(prof, "frames.csv")) as f:
        rows = list(csv.DictReader(f))
    begins = [int(r["begin_us"]) / 1e6 for r in rows]
    present = [int(r["present_us"]) / 1e3 for r in rows]
    times, pres, ends = [], [], []
    for i in range(1, len(begins)):
        if t_from <= begins[i] <= t_to:
            times.append((begins[i] - begins[i - 1]) * 1e3)
            pres.append(present[i])
            ends.append(begins[i])
    return times, pres, ends, begins


def worst_window(times, ends, seconds):
    """Highest mean frame time over any window of the given length (ms), and where it ends (s)."""
    best, at, j, total = 0.0, 0.0, 0, 0.0
    for i in range(len(times)):
        total += times[i]
        while ends[i] - ends[j] > seconds:
            total -= times[j]
            j += 1
        n = i - j + 1
        if ends[i] - ends[0] >= seconds and total / n > best:
            best, at = total / n, ends[i]
    return best, at


def frame_stats(times, pres, ends):
    if not times:
        return {}
    w1, at1 = worst_window(times, ends, 1.0)
    w5, at5 = worst_window(times, ends, 5.0)
    span = ends[-1] - ends[0] + times[0] / 1e3
    return {"frames": len(times), "avg_fps": len(times) / span if span else 0.0,
            "median_ms": statistics.median(times), "p95_ms": pct(times, 95), "p99_ms": pct(times, 99),
            "max_ms": max(times), "over_40ms": sum(t > 40 for t in times), "over_50ms": sum(t > 50 for t in times),
            "worst_1s_mean_ms": w1, "worst_1s_at_s": at1, "worst_5s_mean_ms": w5, "worst_5s_at_s": at5,
            "present_median_ms": statistics.median(pres), "present_p99_ms": pct(pres, 99), "present_max_ms": max(pres)}


def read_threads(prof, t0_wall, t_from, t_to):
    """CPU seconds per thread group within the range. threads.csv times count from profiler start, which is
    t0_wall seconds before the first present (unknown: taken as 0; the ranges are coarse, 1 s)."""
    groups = Counter()
    span = [None, None]
    with open(os.path.join(prof, "threads.csv")) as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        t = float(r["t_s"]) - t0_wall
        if t_from <= t <= t_to + 1:
            groups[thread_group(r["name"])] += float(r["cpu_s"])
            span[0] = t if span[0] is None else min(span[0], t)
            span[1] = t if span[1] is None else max(span[1], t)
    seconds = (span[1] - span[0] + 1) if span[0] is not None else 0
    return groups, seconds


def read_gpu(prof, frame_from, frame_to):
    path = os.path.join(prof, "gpu.csv")
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [int(r["busy_percent"]) for r in csv.DictReader(f) if frame_from <= int(r["frame"]) <= frame_to]


class Symbols:
    """Address -> (object, function) for the mappings of the run (maps.txt), through nm."""

    def __init__(self, prof, bin_dir):
        self.maps = []
        for line in open(os.path.join(prof, "maps.txt")):
            parts = line.split()
            if len(parts) < 6 or "x" not in parts[1]:
                continue
            start, end = (int(x, 16) for x in parts[0].split("-"))
            path = parts[5]
            host = os.path.join(bin_dir, path[len("/app/"):]) if path.startswith("/app/") else path
            self.maps.append((start, end, int(parts[2], 16), path, host))
        self.maps.sort()
        self.starts = [m[0] for m in self.maps]
        self.tables = {}

    def table(self, host):
        if host not in self.tables:
            addrs, names = [], []
            for flags in (["--defined-only"], ["-D", "--defined-only"]):
                try:
                    out = subprocess.run(["nm", "-n", "-C"] + flags + [host], capture_output=True, text=True,
                                         check=False).stdout
                except OSError:
                    out = ""
                for line in out.splitlines():
                    p = line.split(" ", 2)
                    if len(p) == 3 and p[1] in "TtWwiu":
                        addrs.append(int(p[0], 16))
                        names.append(p[2])
                if addrs:
                    break
            self.tables[host] = (addrs, names)
        return self.tables[host]

    def lookup(self, ip):
        i = bisect.bisect_right(self.starts, ip) - 1
        if i < 0 or ip >= self.maps[i][1]:
            return "?", "?"
        start, _, offset, path, host = self.maps[i]
        obj = os.path.basename(path) if path.startswith("/") else path
        addrs, names = self.table(host)
        vaddr = ip - start + offset
        j = bisect.bisect_right(addrs, vaddr) - 1
        return obj, (names[j] if j >= 0 else f"{obj}+{vaddr:#x}")


def category(obj, func, exe):
    if obj == exe:
        if re.match(r"sub_[0-9A-F]{8}$", func) or func.startswith("__imp__sub_"):
            return "guest code (recompiled)"
        if func.startswith("rex::graphics") or func.startswith("rex::ui"):
            return "runtime: graphics"
        if func.startswith("rex::"):
            return "runtime: other"
        return "port / libraries in kuf_cod"
    if obj.startswith("libvulkan_radeon") or obj.startswith("libLLVM") or obj.startswith("libdrm"):
        return "GPU driver (radv/LLVM/drm)"
    if obj.startswith("libc.") or obj.startswith("libm.") or obj.startswith("libstdc++"):
        return "C/C++ library"
    return obj


def read_samples(prof, frame_from, frame_to, symbols, names, exe, top):
    path = os.path.join(prof, "samples.bin")
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        data = f.read()
    by_cat, by_func, by_thread = Counter(), Counter(), Counter()
    by_thread_func = defaultdict(Counter)
    cache = {}
    n = 0
    for ip, tid, frame in SAMPLE.iter_unpack(data):
        if not frame_from <= frame <= frame_to:
            continue
        n += 1
        if ip not in cache:
            cache[ip] = symbols.lookup(ip)
        obj, func = cache[ip]
        group = thread_group(names.get(tid, "?"))
        cat = category(obj, func, exe)
        by_cat[cat] += 1
        by_func[(cat, func)] += 1
        by_thread[group] += 1
        by_thread_func[group][(cat, func)] += 1
    return {"samples": n, "by_category": by_cat.most_common(), "by_thread": by_thread.most_common(),
            "top_functions": [(c, f, k) for (c, f), k in by_func.most_common(top)],
            "per_thread": {g: [(c, f, k) for (c, f), k in by_thread_func[g].most_common(12)]
                           for g, _ in by_thread.most_common(8)}}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run_dir")
    ap.add_argument("--from-s", type=float, default=0.0)
    ap.add_argument("--to-s", type=float, default=1e9)
    ap.add_argument("--from-context", metavar="NAME",
                    help="start N seconds (--context-delay, default 20) after the first '[kuf-input] context: NAME' "
                         "line of the log, e.g. gameplay")
    ap.add_argument("--context-delay", type=float, default=20.0)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--bin-dir", default=os.path.join(ROOT, "build", "linux-amd64-relwithdebinfo", "bin"))
    ap.add_argument("--json")
    args = ap.parse_args()
    prof = os.path.join(args.run_dir, "user", "profile")
    if not os.path.isfile(os.path.join(prof, "frames.csv")):
        print(f"no profile in {prof} (run with --kuf_profile=true)", file=sys.stderr)
        return 2

    if args.from_context:
        frame = None
        for path in sorted(glob.glob(os.path.join(args.run_dir, "kuf_cod*.log")), key=os.path.getmtime):
            for line in open(path, errors="replace"):
                m = re.search(r"\[kuf-input\] context: (\S+) \(frame (\d+)\)", line)
                if m and m.group(1) == args.from_context:
                    frame = int(m.group(2))
                    break
            if frame:
                break
        if frame is None:
            print(f"no '{args.from_context}' context in the log", file=sys.stderr)
            return 2
        rows = list(csv.DictReader(open(os.path.join(prof, "frames.csv"))))
        args.from_s = int(rows[min(frame, len(rows)) - 1]["begin_us"]) / 1e6 + args.context_delay
    times, pres, ends, begins = read_frames(prof, args.from_s, args.to_s)
    report = {"run": os.path.basename(os.path.normpath(args.run_dir)), "from_s": args.from_s,
              "to_s": min(args.to_s, begins[-1] if begins else 0), "frames": frame_stats(times, pres, ends)}
    frame_from = next((i + 1 for i, b in enumerate(begins) if b >= args.from_s), 1)
    frame_to = max((i + 1 for i, b in enumerate(begins) if b <= args.to_s), default=0)
    groups, seconds = read_threads(prof, 0.0, args.from_s, args.to_s)
    report["threads_cpu_percent"] = [(g, 100.0 * s / seconds) for g, s in groups.most_common(15)] if seconds else []
    gpu = read_gpu(prof, frame_from, frame_to)
    if gpu:
        report["gpu_busy"] = {"mean": statistics.mean(gpu), "median": statistics.median(gpu),
                              "p95": pct(gpu, 95), "readings": len(gpu)}
    names = {}
    names_path = os.path.join(prof, "thread_names.csv")
    if os.path.exists(names_path):
        names = {int(r["tid"]): r["name"] for r in csv.DictReader(open(names_path))}
    for r in csv.DictReader(open(os.path.join(prof, "threads.csv"))):
        names.setdefault(int(r["tid"]), r["name"])
    if os.path.exists(os.path.join(prof, "samples.bin")):
        report["samples"] = read_samples(prof, frame_from, frame_to, Symbols(prof, args.bin_dir), names,
                                         "kuf_cod", args.top)

    f = report["frames"]
    print(f"profile {report['run']}: {report['from_s']:.0f}-{report['to_s']:.0f} s")
    if f:
        print(f"  frames {f['frames']}: {f['avg_fps']:.2f} fps average; frame time median {f['median_ms']:.2f} ms, "
              f"95th {f['p95_ms']:.2f}, 99th {f['p99_ms']:.2f}, max {f['max_ms']:.1f}; >40 ms: {f['over_40ms']}, "
              f">50 ms: {f['over_50ms']}")
        print(f"  worst 1 s: mean {f['worst_1s_mean_ms']:.1f} ms (at {f['worst_1s_at_s']:.0f} s); worst 5 s: "
              f"{f['worst_5s_mean_ms']:.1f} ms (at {f['worst_5s_at_s']:.0f} s); present call median "
              f"{f['present_median_ms']:.2f} ms, 99th {f['present_p99_ms']:.2f}, max {f['present_max_ms']:.1f}")
    print("  CPU per thread group (% of one core):")
    for g, p in report["threads_cpu_percent"]:
        print(f"    {p:6.1f}  {g}")
    if "gpu_busy" in report:
        g = report["gpu_busy"]
        print(f"  GPU busy: mean {g['mean']:.0f} %, median {g['median']:.0f} %, 95th {g['p95']:.0f} % "
              f"({g['readings']} readings)")
    s = report.get("samples")
    if s and s["samples"]:
        n = s["samples"]
        print(f"  CPU samples: {n}")
        print("   by kind:")
        for c, k in s["by_category"][:12]:
            print(f"    {100.0 * k / n:5.1f} %  {c}")
        print("   by thread group:")
        for g, k in s["by_thread"][:10]:
            print(f"    {100.0 * k / n:5.1f} %  {g}")
        print(f"   top {len(s['top_functions'])} functions (self time):")
        for c, func, k in s["top_functions"]:
            print(f"    {100.0 * k / n:5.1f} %  {func[:110]}  [{c}]")
        for g, funcs in s["per_thread"].items():
            print(f"   {g}:")
            for c, func, k in funcs:
                print(f"    {100.0 * k / n:5.1f} %  {func[:110]}  [{c}]")
    if args.json:
        with open(args.json, "w") as out:
            json.dump(report, out, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
