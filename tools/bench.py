#!/usr/bin/env python3
"""M8 benchmark framework: runs workloads in the sandbox over resolutions and frame rates, and reports them.

  python3 -I tools/bench.py run --tag base --scenario tests/perf/bench_explore.toml \\
        --res 3840x2160 --fps 30,60,120,144 --reps 2 [--cache-from <run or cache dir> | --warm] [--game-arg=...]
  python3 -I tools/bench.py analyze build/runs/<run-id> [--json out.json]
  python3 -I tools/bench.py report --tag base [--compare opt] [--markdown out.md] [--json out.json]

`run` starts each configuration through tools/menu_test.py (tools/run_sandboxed.sh: read-only game folder, no network,
no display or audio sockets, fresh saves and settings per run, hard timeout) inside a transient systemd user scope with
a memory and CPU limit, so a runaway run cannot take the desktop down. Run ids: m8-<tag>-<res>-<fps>-r<n>-<scenario>.
Resolutions are simulated outputs (--kuf_display_size) rendered at Render Native (display.render_scale=0), which for
1920x1080 and 2560x1440 is the 2560x1440 frame (2x2 of the game's 1280x720) and for 3840x2160 the 3840x2160 frame (3x3).
Frame rate 30 is the game's original pacing setting (one frame per tick, the default); others use --kuf_frame_rate.
While a run is going, VRAM (amdgpu mem_info_vram_used, whole GPU minus its reading before the run) and the game
process's resident memory are sampled from outside the sandbox every 0.5 s (bench-samples.csv in the run directory).

`analyze` reads one run: the measured windows (the workload's log marks: window_start/window_end, fight_start/
fight_end, menus_start/menus_end), per window the presented frames (frames.csv of
--kuf_profile) with average, median, 1 % and 0.1 % lows (fps over the slowest 1 % / 0.1 % of frame times), frame time
percentiles and spikes; CPU per thread group and in total (threads.csv; the test environment's SDL audio thread, which
spins on ALSA's null sink, is reported apart and left out of the total, docs/PERFORMANCE.md 1.3); GPU busy (amdgpu);
GPU time per frame when the run had --gpu_timestamp_stats; render-only frames dropped (late, render busy); pipelines
created and presentations skipped for placeholder pipelines (shader compilation stalls); boot and world-load times;
peak memory; exit status. Nothing is estimated: what a run did not record is reported as unavailable (None).
"""

import argparse
import csv
import glob
import importlib.util
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_spec = importlib.util.spec_from_file_location("profile_report", os.path.join(ROOT, "tools", "profile_report.py"))
profile_report = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(profile_report)

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
MARK = re.compile(r"\[kuf-frames\] (?:\S+ )?log (\S+): (\d+) frames presented")
WINDOWS = [("window", "window_start", "window_end"), ("fight", "fight_start", "fight_end"),
           ("menus", "menus_start", "menus_end")]
TEST_ONLY_THREADS = ("SDLAudioP",)  # spins on the sandbox's ALSA null sink; absent with a real device (K10)


def skipped_presentation(line):
    # SDK patch 0006 logs it with --gpu_cp_stats; the SDK's own warning otherwise.
    return "presentation skipped (async placeholder" in line or "Skipping Vulkan frame presentation" in line


def wall(line):
    m = TS.match(line)
    return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() if m else None


# ---------------------------------------------------------------------------------------------------------- sampling

def amdgpu_vram_path():
    for p in sorted(glob.glob("/sys/class/drm/card*/device/mem_info_vram_used")):
        return p
    return None


def read_int(path):
    try:
        with open(path) as f:
            return int(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def gpu_state():
    """amdgpu shader clock (MHz, the active DPM level), edge / junction / memory temperature (C) and power (W)."""
    dev = os.path.dirname(amdgpu_vram_path() or "")
    out = {"sclk_mhz": None, "edge_c": None, "junction_c": None, "mem_c": None, "power_w": None}
    if not dev:
        return out
    try:
        for line in open(os.path.join(dev, "pp_dpm_sclk")):
            if line.rstrip().endswith("*"):
                out["sclk_mhz"] = int(re.search(r"(\d+)Mhz", line).group(1))
    except (OSError, AttributeError):
        pass
    for hw in glob.glob(os.path.join(dev, "hwmon", "hwmon*")):
        for t in glob.glob(os.path.join(hw, "temp*_input")):
            label = open(t.replace("_input", "_label")).read().strip() if os.path.exists(t.replace("_input", "_label")) else ""
            key = {"edge": "edge_c", "junction": "junction_c", "mem": "mem_c"}.get(label)
            v = read_int(t)
            if key and v is not None:
                out[key] = v / 1000.0
        p = read_int(os.path.join(hw, "power1_average")) or read_int(os.path.join(hw, "power1_input"))
        if p is not None:
            out["power_w"] = p / 1e6
    return out


def game_pids():
    """Host pids of running kuf_cod processes (the sandbox's game; started only by the harness)."""
    pids = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open(f"/proc/{d}/comm") as f:
                if f.read().strip() == "kuf_cod":
                    pids.append(int(d))
        except OSError:
            pass
    return pids


def rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return None


class Sampler(threading.Thread):
    """VRAM and the game's RSS every 0.5 s, from outside the sandbox (only reads /sys and /proc)."""

    def __init__(self):
        super().__init__(daemon=True)
        self.stop = threading.Event()
        self.rows = []
        self.vram = amdgpu_vram_path()
        self.vram_idle = None
        if self.vram:
            readings = []
            for _ in range(5):
                readings.append(read_int(self.vram))
                time.sleep(0.1)
            readings = [r for r in readings if r is not None]
            self.vram_idle = statistics.median(readings) if readings else None

    def run(self):
        t0 = time.monotonic()
        self.pids = set()
        while not self.stop.wait(0.5):
            pids = game_pids()
            self.pids.update(pids)
            rss = sum(r for r in (rss_kb(p) for p in pids) if r) if pids else None
            g = gpu_state()
            self.rows.append((time.monotonic() - t0, read_int(self.vram) if self.vram else None, rss,
                              g["sclk_mhz"], g["edge_c"], g["junction_c"], g["mem_c"], g["power_w"]))

    def write(self, path):
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_s", "vram_used_bytes", "game_rss_kb", "vram_idle_bytes", "sclk_mhz", "edge_c", "junction_c",
                        "mem_c", "power_w"])
            for t, v, r, *g in self.rows:
                w.writerow([f"{t:.2f}", v if v is not None else "", r if r is not None else "",
                            int(self.vram_idle) if self.vram_idle is not None else ""] +
                           ["" if x is None else x for x in g])


# --------------------------------------------------------------------------------------------------------------- run

def config_args(res, fps, extra):
    args = ["--kuf_profile=true", "--gpu_cp_stats=true", "--gpu_timestamp_stats=true", f"--kuf_display_size={res}",
            "--kuf_pc_settings=display.render_scale=0"]
    if fps != 30:
        args.append(f"--kuf_frame_rate={fps}")
    return args + list(extra)


def run_one(scenario, run_prefix, res, fps, extra, cache_from, memory_max, cpu_quota, dry, preset):
    name = os.path.splitext(os.path.basename(scenario))[0]
    run_id = f"{run_prefix}-{name}"
    cmd = ["systemd-run", "--user", "--scope", "--quiet", "-p", f"MemoryMax={memory_max}", "-p",
           f"CPUQuota={cpu_quota}", "--", sys.executable, "-I", os.path.join(ROOT, "tools", "menu_test.py"),
           "--preset", preset, "--run-prefix", run_prefix]
    if cache_from:
        cmd += ["--cache-from", cache_from]
    cmd += [f"--game-arg={a}" for a in config_args(res, fps, extra)]
    cmd.append(scenario)
    print(f"[bench] {run_id}: {res} @ {fps} fps{' (warm cache)' if cache_from else ''}", flush=True)
    if dry:
        print("  " + " ".join(cmd))
        return run_id, 0
    sampler = Sampler()
    sampler.start()
    t0 = time.monotonic()
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    sampler.stop.set()
    sampler.join()
    run_dir = os.path.join(ROOT, "build", "runs", run_id)
    if os.path.isdir(run_dir):
        sampler.write(os.path.join(run_dir, "bench-samples.csv"))
        with open(os.path.join(run_dir, "bench-config.json"), "w") as f:
            json.dump({"scenario": scenario, "res": res, "fps": fps, "extra": list(extra), "cache_from": cache_from,
                       "preset": preset, "game_pids": sorted(getattr(sampler, "pids", [])),
                       "harness_exit": proc.returncode, "wall_s": time.monotonic() - t0,
                       "harness_output": proc.stdout[-4000:] + proc.stderr[-2000:]}, f, indent=1)
    # The harness also checks captures, which bench workloads do not take: a run counts by analyze()'s "valid"
    # (the harness's run, script and log checks) and its exit status.
    r = analyze(run_dir) if os.path.isdir(run_dir) else {"valid": False}
    ok = bool(r.get("valid")) and not r.get("crashed")
    w = next(iter(r.get("windows", {}).values()), {}).get("frames") or {}
    print(f"  {'valid' if ok else 'INVALID'}: exit {r.get('exit_status')}, "
          f"{w.get('avg_fps', 0):.1f} fps, p99 {w.get('p99_ms', 0):.1f} ms", flush=True)
    return run_id, 0 if ok else 1


def cmd_run(a):
    ress = a.res.split(",")
    fpss = [int(x) for x in a.fps.split(",")]
    scenarios = a.scenario
    failures = 0
    for rep in range(a.first_rep, a.first_rep + a.reps):
        for scenario in scenarios:
            for res in ress:
                for fps in fpss:
                    prefix = f"m8-{a.tag}-{res}-{fps}-r{rep}"
                    cache = a.cache_from
                    if a.warm:
                        # The warm cache: a copy of the cold-cache run of the same configuration (same pipelines).
                        # Pipelines do not depend on the output size or frame rate: any cold run of the scenario.
                        name = os.path.splitext(os.path.basename(scenario))[0]
                        cold = os.path.join(ROOT, "build", "runs", f"m8-{a.warm}-{res}-{fps}-r1-{name}")
                        if not os.path.isdir(os.path.join(cold, "cache")):
                            cold = next(iter(sorted(glob.glob(os.path.join(ROOT, "build", "runs",
                                                                           f"m8-{a.warm}-*-r1-{name}")))), cold)
                        cache = cold if os.path.isdir(os.path.join(cold, "cache")) else None
                        if cache is None:
                            print(f"[bench] no cold run {cold} for the warm cache; running cold", flush=True)
                    _, rc = run_one(scenario, prefix, res, fps, a.game_arg, cache, a.memory_max, a.cpu_quota, a.dry,
                                    a.preset)
                    failures += rc != 0
    print(f"[bench] done: {failures} invalid run(s)")
    return 1 if failures else 0


# ----------------------------------------------------------------------------------------------------------- analyze

def lows(times):
    """fps over the slowest 1 % and 0.1 % of frames (mean frame time of that tail)."""
    out = {}
    s = sorted(times, reverse=True)
    for key, frac in (("low_1pct_fps", 0.01), ("low_01pct_fps", 0.001)):
        n = max(1, int(len(s) * frac))
        tail = s[:n]
        out[key] = 1000.0 / statistics.mean(tail) if tail else None
    return out


def window_frames(prof, t_from, t_to):
    times, pres, ends, begins = profile_report.read_frames(prof, t_from, t_to)
    st = profile_report.frame_stats(times, pres, ends)
    if not st:
        return None
    st.update(lows(times))
    st["mean_ms"] = statistics.mean(times)
    st["p50_ms"] = st["median_ms"]
    st["over_2x_median"] = sum(t > 2 * st["median_ms"] for t in times)
    # Pacing: spread of the frame times, and jitter = mean change between consecutive frame times (frames in bursts,
    # e.g. 5 and 21 ms alternately, give a large jitter at the same average).
    st["stdev_ms"] = statistics.pstdev(times)
    st["jitter_ms"] = statistics.mean(abs(times[i] - times[i - 1]) for i in range(1, len(times))) if len(times) > 1 else 0.0
    return st


def read_log(run_dir):
    lines = []
    for path in sorted(glob.glob(os.path.join(run_dir, "kuf_cod*.log")), key=os.path.getmtime):
        with open(path, errors="replace") as f:
            lines.extend(f.readlines())
    return lines


def analyze(run_dir):
    run_dir = os.path.normpath(run_dir)
    out = {"run": os.path.basename(run_dir)}
    cfg_path = os.path.join(run_dir, "bench-config.json")
    if os.path.exists(cfg_path):
        with open(cfg_path) as f:
            cfg = json.load(f)
        out.update({k: cfg[k] for k in ("scenario", "res", "fps", "cache_from", "harness_exit")})
    st = os.path.join(run_dir, "exit_status")
    out["exit_status"] = int(open(st).read().strip()) if os.path.exists(st) else None
    out["crashed"] = out["exit_status"] not in (0, None) or os.path.exists(os.path.join(run_dir, "crash-report.txt"))
    # Valid: the harness's run, script and log checks passed (bench workloads have no image checks by design).
    report = os.path.join(run_dir, "menu-report.txt")
    if os.path.exists(report):
        checks = {m.group(2): m.group(1) == "ok" for m in re.finditer(r"\[(ok|FAIL)\] (\w[\w ]*?):", open(report).read())}
        out["valid"] = all(checks.get(k, False) for k in ("run", "script", "log"))
    else:
        out["valid"] = None
    lines = read_log(run_dir)
    prof = os.path.join(run_dir, "user", "profile")
    if not os.path.isfile(os.path.join(prof, "frames.csv")):
        out["error"] = "no profile (frames.csv)"
        return out
    with open(os.path.join(prof, "frames.csv")) as f:
        begins = [int(r["begin_us"]) / 1e6 for r in csv.DictReader(f)]
    out["frames_total"] = len(begins)

    # Log marks -> profile time (the frame count when the mark was applied -> that frame's present time).
    marks, mark_wall = {}, {}
    for line in lines:
        m = MARK.search(line)
        if m and m.group(1) not in marks:
            n = int(m.group(2))
            marks[m.group(1)] = begins[min(max(n, 1), len(begins)) - 1]
            mark_wall[m.group(1)] = wall(line)
    windows = {}
    for key, a, b in WINDOWS:
        if a in marks and b in marks:
            windows[key] = (marks[a], marks[b])
    out["windows"] = {}
    names = {}
    for r in csv.DictReader(open(os.path.join(prof, "threads.csv"))):
        names.setdefault(int(r["tid"]), r["name"])
    with open(os.path.join(prof, "frames.csv")) as f:
        nframes = len(begins)
    for key, (t0, t1) in windows.items():
        w = {"from_s": t0, "to_s": t1, "frames": window_frames(prof, t0, t1)}
        groups, seconds = profile_report.read_threads(prof, 0.0, t0, t1)
        if seconds:
            per = {g: 100.0 * s / seconds for g, s in groups.items()}
            w["cpu_percent_by_group"] = dict(sorted(per.items(), key=lambda kv: -kv[1])[:14])
            w["cpu_percent_by_group"].setdefault("GPU Commands", 0.0)
            w["cpu_percent_total"] = sum(v for g, v in per.items() if not g.startswith(TEST_ONLY_THREADS))
            w["cpu_percent_test_only"] = sum(v for g, v in per.items() if g.startswith(TEST_ONLY_THREADS))
        f0 = next((i + 1 for i, b in enumerate(begins) if b >= t0), 1)
        f1 = max((i + 1 for i, b in enumerate(begins) if b <= t1), default=nframes)
        gpu = profile_report.read_gpu(prof, f0, f1)
        w["gpu_busy_mean"] = statistics.mean(gpu) if gpu else None
        w["gpu_busy_p95"] = profile_report.pct(gpu, 95) if gpu else None
        w.update(window_log_stats(lines, t0, t1, begins, marks, mark_wall, key))
        out["windows"][key] = w

    # Loading and boot (wall clock of the log lines).
    first = next((wall(l) for l in lines if wall(l)), None)
    prompt = next((wall(l) for l in lines if "[kuf-input] context: start prompt" in l), None)
    out["boot_to_start_prompt_s"] = prompt - first if first and prompt else None
    if "load_request" in mark_wall:
        t_req = mark_wall["load_request"]
        t_play = next((wall(l) for l in lines if "[kuf-input] context: gameplay" in l and wall(l) and wall(l) > t_req),
                      None)
        out["world_load_s"] = t_play - t_req if t_play else None
    else:
        out["world_load_s"] = None
    out["pipelines_created_total"] = sum("Creating graphics pipeline state" in l or "Creating compute pipeline" in l
                                         for l in lines)
    out["presentations_skipped_total"] = sum(skipped_presentation(l) for l in lines)
    out["pipelines_from_cache"] = next((int(m.group(1)) for l in lines
                                        for m in [re.search(r"(\d+) pipelines? (?:were )?(?:created|loaded) from "
                                                            r"(?:the )?(?:storage|cache)", l)] if m), None)

    # Memory.
    summ = os.path.join(run_dir, "user", "run-summary.json")
    if not os.path.exists(summ):
        summ = next(iter(glob.glob(os.path.join(run_dir, "**", "run-summary.json"), recursive=True)), None)
    if summ and os.path.exists(summ):
        with open(summ) as f:
            rs = json.load(f)
        rss = [x for x in rs.get("rss_mb", []) if x]
        out["rss_peak_mb_telemetry"] = max(rss) if rss else None
        out["rss_end_mb_telemetry"] = rss[-1] if rss else None
    samples = os.path.join(run_dir, "bench-samples.csv")
    if os.path.exists(samples):
        rows = list(csv.DictReader(open(samples)))
        rss = [int(r["game_rss_kb"]) for r in rows if r["game_rss_kb"]]
        vram = [int(r["vram_used_bytes"]) for r in rows if r["vram_used_bytes"]]
        idle = next((int(r["vram_idle_bytes"]) for r in rows if r["vram_idle_bytes"]), None)
        out["rss_peak_mb"] = max(rss) / 1024 if rss else None
        out["vram_peak_mb_over_idle"] = (max(vram) - idle) / 2**20 if vram and idle is not None else None
        out["vram_idle_mb"] = idle / 2**20 if idle is not None else None
        # GPU state while the game ran with a high load (samples with power above half the run's maximum)
        if rows and "power_w" in rows[0]:
            pw = [float(r["power_w"]) for r in rows if r.get("power_w")]
            hi = max(pw) / 2 if pw else 0
            busy = [r for r in rows if r.get("power_w") and float(r["power_w"]) >= hi]
            for k in ("sclk_mhz", "junction_c", "mem_c", "power_w"):
                v = [float(r[k]) for r in busy if r.get(k)]
                out[f"gpu_{k}_median"] = statistics.median(v) if v else None
    return out


def window_log_stats(lines, t0, t1, begins, marks, mark_wall, key):
    """Counters from log lines inside a window: located by wall-clock time, mapped through the marks' two clocks."""
    a, b = {"window": ("window_start", "window_end"), "fight": ("fight_start", "fight_end"),
            "menus": ("menus_start", "menus_end")}[key]
    out = {"gpu_ms_per_frame": None, "dropped_late": None, "dropped_busy": None, "pipelines_created": None,
           "presentations_skipped": None, "build_ms_mean": None, "tick_ms_p95": None}
    if a not in mark_wall or b not in mark_wall:
        return out
    w0, w1 = mark_wall[a], mark_wall[b]
    inside = [l for l in lines if (lambda t: t is not None and w0 <= t <= w1)(wall(l))]
    out["pipelines_created"] = sum("Creating graphics pipeline state" in l for l in inside)
    out["presentations_skipped"] = sum(skipped_presentation(l) for l in inside)
    # Dropped render-only frames: cumulative counters on the 900-build cost lines; difference across the window.
    cost = [(wall(l), l) for l in lines if "[kuf-pacing] build costs over" in l]
    before = [l for t, l in cost if t <= w0]
    within = [l for t, l in cost if w0 < t <= w1]
    if within:
        def drops(l):
            m = re.search(r"frames dropped: late (\d+), render busy (\d+)", l)
            return (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        d1 = drops(within[-1])
        d0 = drops(before[-1]) if before else (0, 0)
        out["dropped_late"], out["dropped_busy"] = d1[0] - d0[0], d1[1] - d0[1]
        bm = [float(m.group(1)) for l in within for m in [re.search(r"scene build\) mean ([\d.]+)", l)] if m]
        tk = [float(m.group(1)) for l in within for m in [re.search(r"the whole tick mean [\d.]+ p50 [\d.]+ p95 ([\d.]+)", l)] if m]
        out["build_ms_mean"] = statistics.mean(bm) if bm else None
        out["tick_ms_p95"] = statistics.median(tk) if tk else None
    # GPU time per frame (SDK patch 0012, --gpu_timestamp_stats): 10 s summaries; weighted mean, median of p99s.
    gt = [m for l in inside for m in [re.search(r"\[gpu-time\] [\d.]+ s: (\d+) frames, GPU ms per frame mean ([\d.]+) "
                                                r"p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+) max ([\d.]+)", l)] if m]
    if gt:
        n = sum(int(m.group(1)) for m in gt)
        if n:
            out["gpu_ms_per_frame"] = sum(int(m.group(1)) * float(m.group(2)) for m in gt) / n
            out["gpu_ms_p50"] = statistics.median(float(m.group(3)) for m in gt)
            out["gpu_ms_p99"] = statistics.median(float(m.group(5)) for m in gt)
            out["gpu_ms_max"] = max(float(m.group(6)) for m in gt)
    # Sections (--gpu_timestamp_sections): mean over the window's 10 s summaries.
    sec = [m for l in inside for m in [re.search(r"sections per frame: draws ([\d.]+) ms, transfers ([\d.]+) ms "
                                                 r"\(([\d.]+) per frame\), resolves ([\d.]+) ms \(([\d.]+) per frame\)"
                                                 r"(?:, texture loads ([\d.]+) ms \([\d.]+ per frame\)"
                                                 r"(?:, texture copies into images ([\d.]+) ms)?)?", l)] if m]
    if sec:
        for key_name, g in (("gpu_draws_ms", 1), ("gpu_transfers_ms", 2), ("transfers_per_frame", 3),
                            ("gpu_resolves_ms", 4), ("resolves_per_frame", 5), ("gpu_texture_loads_ms", 6),
                            ("gpu_texture_copies_ms", 7)):
            vals = [float(m.group(g)) for m in sec if m.group(g) is not None]
            out[key_name] = statistics.mean(vals) if vals else None
    # GPU frame completion intervals (patch 0012): the offscreen proxy for when frames would reach the display.
    iv = [m for l in inside for m in [re.search(r"frame completion interval ms: mean ([\d.]+) stdev ([\d.]+) "
                                                r"p1 ([\d.]+) p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+) max ([\d.]+)",
                                                l)] if m]
    if iv:
        out["gpu_interval_stdev_ms"] = statistics.mean(float(m.group(2)) for m in iv)
        out["gpu_interval_p99_ms"] = statistics.median(float(m.group(6)) for m in iv)
    # cp-stats: the command processor's busy share.
    # Executing packets minus the WAIT_REG_MEM waits inside them (sleeps at 30 fps, yields above): the thread's work.
    cp = [m for l in inside for m in [re.search(r"\[cp-stats\] ([\d.]+) s: executing (\d+) ms in \d+ batches "
                                                r"\(waits inside: (\d+) ms", l)] if m]
    out["cp_busy_percent"] = (100.0 * sum(int(m.group(2)) - int(m.group(3)) for m in cp) /
                              (1000 * sum(float(m.group(1)) for m in cp)) if cp else None)
    return out


def cmd_analyze(a):
    res = [analyze(d) for d in a.run_dirs]
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)
    for r in res:
        print(json.dumps(r, indent=1))
    return 0


# ------------------------------------------------------------------------------------------------------------ report

def runs_for_tag(tag):
    # m8-<tag>-<width>x<height>-...: the size right after the tag, so "base" does not take "base-cold"'s runs
    pattern = re.compile(rf"^m8-{re.escape(tag)}-\d+x\d+-")
    return sorted(d for d in glob.glob(os.path.join(ROOT, "build", "runs", f"m8-{tag}-*"))
                  if os.path.isdir(d) and ".prev-" not in d and pattern.match(os.path.basename(d)))


def group_key(r):
    return (os.path.splitext(os.path.basename(r.get("scenario", "?")))[0], r.get("res"), r.get("fps"))


def agg(values):
    v = [x for x in values if x is not None]
    if not v:
        return None
    return {"mean": statistics.mean(v), "min": min(v), "max": max(v), "n": len(v),
            "stdev": statistics.stdev(v) if len(v) > 1 else 0.0}


METRICS = [  # (label, window metric path or run metric, higher is better)
    ("avg fps", ("frames", "avg_fps"), True),
    ("median frame ms", ("frames", "median_ms"), False),
    ("1% low fps", ("frames", "low_1pct_fps"), True),
    ("0.1% low fps", ("frames", "low_01pct_fps"), True),
    ("mean frame ms", ("frames", "mean_ms"), False),
    ("p95 frame ms", ("frames", "p95_ms"), False),
    ("p99 frame ms", ("frames", "p99_ms"), False),
    ("max frame ms", ("frames", "max_ms"), False),
    ("frames > 2x median", ("frames", "over_2x_median"), False),
    ("frame time stdev ms", ("frames", "stdev_ms"), False),
    ("frame time jitter ms", ("frames", "jitter_ms"), False),
    ("CPU % (one core = 100)", ("cpu_percent_total",), False),
    ("CPU % guest threads", ("cpu_percent_by_group", "XThread (guest)"), False),
    ("CPU % GPU command thread", ("cpu_percent_by_group", "GPU Commands"), False),
    ("GPU busy % (amdgpu)", ("gpu_busy_mean",), False),
    ("GPU ms per frame", ("gpu_ms_per_frame",), False),
    ("GPU ms p99", ("gpu_ms_p99",), False),
    ("GPU command thread working % (waits excluded)", ("cp_busy_percent",), False),
    ("GPU completion interval stdev ms", ("gpu_interval_stdev_ms",), False),
    ("GPU completion interval p99 ms", ("gpu_interval_p99_ms",), False),
    ("GPU draws ms", ("gpu_draws_ms",), False),
    ("GPU transfers ms", ("gpu_transfers_ms",), False),
    ("GPU resolves ms", ("gpu_resolves_ms",), False),
    ("GPU texture loads ms", ("gpu_texture_loads_ms",), False),
    ("dropped (late)", ("dropped_late",), False),
    ("dropped (render busy)", ("dropped_busy",), False),
    ("pipelines created", ("pipelines_created",), False),
    ("presentations skipped", ("presentations_skipped",), False),
]
RUN_METRICS = [("boot to start prompt s", "boot_to_start_prompt_s", False), ("world load s", "world_load_s", False),
               ("GPU clock MHz (median under load)", "gpu_sclk_mhz_median", True),
               ("GPU junction C (median)", "gpu_junction_c_median", False),
               ("GPU power W (median)", "gpu_power_w_median", False),
               ("peak RSS MB", "rss_peak_mb", False), ("peak VRAM MB over idle", "vram_peak_mb_over_idle", False),
               ("crashed", "crashed", False)]


def get(d, path):
    for p in path:
        if d is None:
            return None
        d = d.get(p)
    return d


def summarize(tag):
    groups = {}
    for d in runs_for_tag(tag):
        r = analyze(d)
        groups.setdefault(group_key(r), []).append(r)
    table = {}
    for key, runs in sorted(groups.items(), key=lambda kv: (kv[0][0], str(kv[0][1]), kv[0][2] or 0)):
        entry = {"runs": [r["run"] for r in runs], "windows": {}, "run": {}}
        wins = sorted({w for r in runs for w in r.get("windows", {})})
        for w in wins:
            entry["windows"][w] = {label: agg([get(r.get("windows", {}).get(w), path) for r in runs])
                                   for label, path, _ in METRICS}
        for label, k, _ in RUN_METRICS:
            entry["run"][label] = agg([float(r.get(k)) if r.get(k) is not None else None for r in runs])
        table["|".join(str(x) for x in key)] = entry
    return table


def fmt(a, digits=1):
    if a is None:
        return "unavailable"
    s = f"{a['mean']:.{digits}f}"
    if a["n"] > 1 and a["max"] != a["min"]:
        s += f" ({a['min']:.{digits}f}-{a['max']:.{digits}f})"
    return s


def change(b, o, higher_better):
    if b is None or o is None or b["mean"] == 0:
        return "n/a"
    d = 100.0 * (o["mean"] - b["mean"]) / abs(b["mean"])
    better = (d > 0) == higher_better
    return f"{d:+.1f} %" + ("" if abs(d) < 0.05 else (" better" if better else " worse"))


def cmd_report(a):
    base = summarize(a.tag)
    comp = summarize(a.compare) if a.compare else None
    md = []
    for key, entry in base.items():
        scen, res, fps = key.split("|")
        md.append(f"### {scen}, {res}, {fps} fps (runs: {len(entry['runs'])})\n")
        cols = "| Metric | " + a.tag + (" | " + a.compare + " | Change |" if comp else " |")
        md.append(cols)
        md.append("|---|---|" + ("---|---|" if comp else ""))
        for w, metrics in entry["windows"].items():
            for label, _, hb in METRICS:
                b = metrics[label]
                if comp:
                    o = comp.get(key, {}).get("windows", {}).get(w, {}).get(label)
                    md.append(f"| {w}: {label} | {fmt(b)} | {fmt(o)} | {change(b, o, hb)} |")
                else:
                    md.append(f"| {w}: {label} | {fmt(b)} |")
        for label, _, hb in RUN_METRICS:
            b = entry["run"][label]
            if comp:
                o = comp.get(key, {}).get("run", {}).get(label)
                md.append(f"| {label} | {fmt(b, 2)} | {fmt(o, 2)} | {change(b, o, hb)} |")
            else:
                md.append(f"| {label} | {fmt(b, 2)} |")
        md.append("")
    text = "\n".join(md)
    print(text)
    if a.markdown:
        with open(a.markdown, "w") as f:
            f.write(text + "\n")
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"base": base, "compare": comp}, f, indent=1)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--tag", required=True)
    r.add_argument("--scenario", action="append", required=True)
    r.add_argument("--res", default="3840x2160")
    r.add_argument("--fps", default="30")
    r.add_argument("--reps", type=int, default=1)
    r.add_argument("--first-rep", type=int, default=1, help="number of the first repetition (interleaved series)")
    r.add_argument("--cache-from")
    r.add_argument("--warm", metavar="COLD_TAG", help="use the cold run m8-<COLD_TAG>-<res>-<fps>-r1-<scenario>'s cache")
    r.add_argument("--game-arg", action="append", default=[])
    r.add_argument("--memory-max", default="10G")
    r.add_argument("--cpu-quota", default="1200%")
    r.add_argument("--dry", action="store_true")
    r.add_argument("--preset", default="linux-amd64-relwithdebinfo",
                   help="build/<preset>/bin to run (e.g. a frozen copy of a baseline build)")
    r.set_defaults(func=cmd_run)
    an = sub.add_parser("analyze")
    an.add_argument("run_dirs", nargs="+")
    an.add_argument("--json")
    an.set_defaults(func=cmd_analyze)
    rp = sub.add_parser("report")
    rp.add_argument("--tag", required=True)
    rp.add_argument("--compare")
    rp.add_argument("--markdown")
    rp.add_argument("--json")
    rp.set_defaults(func=cmd_report)
    a = ap.parse_args()
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
