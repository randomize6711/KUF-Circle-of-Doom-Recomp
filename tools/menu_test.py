#!/usr/bin/env python3
"""Deterministic menu-navigation tests (M4): run the game sandboxed with scripted input, then verify.

Usage: menu_test.py [--preset linux-amd64-relwithdebinfo] [--run-prefix menu] [--baseline]
                    [--references-root build/references] [--game-arg=--option=value ...] [--cache-from dir]
                    tests/menu/<scenario>.toml ...

Each scenario file has a [scenario] table (script or script_file, seconds, capture_frames, optional args,
log_expect, log_sequence) and capture_check.py expectations ([format], [[highlight]], [[range]], ...).
Save scenarios (M5B) chain runs:
  script_file     input script in a file next to the scenario (--kuf_input_script_file), for long scripts
  saves_from_run  start with a copy of the saves of an earlier scenario's run (<run-prefix>-<name>); capture
                  rules can then refer to that run's labelled captures as "prev:<label>" (compare_run: take
                  "prev:" captures from another scenario's run instead)
  saves_from_dir  start with a copy of a save directory under build/ (e.g. build/m6-saves/<name>, saves from a
                  manual play session kept outside Git); only read. compare_run then names the run whose
                  captures "prev:" refers to
  compare_dir     captures directory under build/ that "prev:" refers to instead (e.g. inspected captures stored
                  next to a saves_from_dir fixture)
  damage          [[scenario.damage]] entries change that copy before the run: {file = "<glob>", action =
                  "flip" | "truncate" | "delete" | "garbage"} (invalid and corrupted saves)
  expect_errors   regular expressions for [error] lines this scenario provokes on purpose: each must occur,
                  and only these are allowed besides the known list (config/v1/known_log_errors.toml)
  expect_exit     exit status of the run (default 0; 137 for a run killed on purpose with SIGKILL)
  expect_saves    globs (relative to the run's saves directory) that must exist after the run;
                  expect_no_saves globs that must not; expect_saves_unchanged = true: every game file the
                  run started with is still there, byte for byte
  references      name of the local reference set under build/references/ (default v1-menu-<scenario>); a new
                  name when the screens change on purpose, so earlier references are kept
  log_absent      lines that must not occur in the log
  log_value       [[scenario.log_value]] a number taken from the log in a window of lines, optionally relative to
                  another one, within a range (check_log_values; the camera tests, M6B)
  input_ticks     [[scenario.input_ticks]] {name, after, before, item, update} (K38): in the window, the input items
                  whose text matches `item` reach the game one tick each: between two such items applied at ticks N
                  and M the log has exactly M - N lines matching `update` (a per-tick trace, e.g. the camera's), and
                  one after the last (check_input_ticks)
  copy_in         ["<repository file>", ...]: copied into the run as /run-data/in/<name> (test helpers, M7)
  pc_settings_from_run / pc_settings_file   start with that run's (or that repository file's) pc_settings.toml
                  (M6B); pc_settings_expect {"table.key" = value, ...}: the run's pc_settings.toml holds these
                  values afterwards; pc_settings_absent = true: the run wrote no pc_settings.toml
  files           [[scenario.files]] {a = "<root>:<glob>", b = "<root>:<glob>", same = true | false}: two
                  save files compared byte for byte; root "saves" = this run's saves directory, "input" = the
                  saves it started with; each glob must match exactly one file (interrupted-save checks)
  timing          [scenario.timing] (M6D) the game's speed against real time: between the labelled captures
                  `from` and `to` (their frame numbers count game ticks, their times are wall-clock seconds)
                  ticks per second within [min_ticks_per_second, max_ticks_per_second]; optional expect_mode
                  (run-summary pacing.mode: "fixed_tick" / "original"), max_frames_per_tick (presented frames per
                  game tick over the run, e.g. 1.01 = each tick drawn once) and the game's own tick counter
                  advancing with the counted ticks (game_counter_matches = true); min_frames_per_tick (render-only
                  frames, D3b-1) and summary_max {key = limit}: run-summary pacing counters that may not exceed the
                  limit (e.g. rand_in_builds = 0)
Per scenario:
  run        sandboxed offscreen run (tools/run_sandboxed.sh --gpu) with --kuf_input_script, timed exit,
             exit status 0, no crash report, no [error]/[critical] log lines
  script     every scripted item was applied (one log line each)
  log        each log_expect line occurs; log_sequence lines occur in this order
  images     tools/capture_check.py check against the scenario file (highlighted menu rows etc.)
  compare    if local references exist (build/references/v1-menu-<scenario>/), perceptual comparison
--game-arg adds a game option to every run (e.g. --game-arg=--kuf_profile=true for a profiled run of a
scenario: the scenario's own checks then show the workload went as intended).
--gdb script.gdb runs every scenario under gdb in the sandbox (investigation only; the timing changes).
--baseline stores the captures as local references (only after the run passed and was inspected).
Captures and references are game content and stay under build/. Exit status 0 = all scenarios PASS.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import known_errors  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKER = os.path.join(ROOT, "tools", "capture_check.py")


def script_items(text):
    return [i.strip() for i in re.split(r"[;\n]", text) if i.strip()]


def file_script_text(text):
    """Same rules as the game's --kuf_input_script_file reader: '#' comment lines and blank lines skipped."""
    return "; ".join(l.strip() for l in text.splitlines() if l.strip() and not l.strip().startswith("#"))


def compare_files(entries, roots):
    """[[scenario.files]] checks; returns the problems found (empty = all as expected)."""
    problems = []

    def resolve(spec):
        root, _, pattern = spec.partition(":")
        if not roots.get(root):
            return None, f"{spec}: no {root!r} directory in this scenario"
        hits = glob.glob(os.path.join(roots[root], glob.escape(os.path.dirname(pattern)), os.path.basename(pattern)))
        if len(hits) != 1:
            return None, f"{spec}: {len(hits)} files match"
        return hits[0], None

    for e in entries:
        (a, err_a), (b, err_b) = resolve(e["a"]), resolve(e["b"])
        if err_a or err_b:
            problems += [x for x in (err_a, err_b) if x]
            continue
        same = open(a, "rb").read() == open(b, "rb").read()
        if same != e.get("same", True):
            problems.append(f"{e['a']} and {e['b']} are {'identical' if same else 'different'}")
    return problems


def prepare_damaged_saves(source, dest, damage):
    """Copies a saves directory to `dest` (a fresh directory under build/runs) and damages files in the copy;
    the source is never changed."""
    if os.path.exists(dest):
        os.rename(dest, f"{dest}.prev-{time.strftime('%Y%m%d-%H%M%S')}")
    shutil.copytree(source, dest, symlinks=False)
    for d in damage:
        hits = [p for p in glob.glob(os.path.join(dest, "**", d["file"]), recursive=True) if os.path.isfile(p)]
        if not hits:
            raise SystemExit(f"damage: no file matches {d['file']!r} in {dest}")
        for p in hits:
            data = open(p, "rb").read()
            if d["action"] == "flip":        # one byte in the middle changed
                i = len(data) // 2
                data = data[:i] + bytes([data[i] ^ 0x5A]) + data[i + 1:]
            elif d["action"] == "truncate":  # as if a write stopped half-way
                data = data[:len(data) // 2]
            elif d["action"] == "garbage":   # same size, unrelated content
                data = bytes((i * 131 + 7) & 0xFF for i in range(len(data)))
            elif d["action"] == "delete":
                os.remove(p)
                continue
            else:
                raise SystemExit(f"damage: unknown action {d['action']!r}")
            open(p, "wb").write(data)
    return dest


def saves_changed(before, after):
    """Game files (not runtime .rex-* bookkeeping) of `before` that are missing or different in `after`."""
    changed = []
    for base, dirs, names in os.walk(before):
        dirs[:] = [d for d in dirs if ".rex-" not in d]
        for n in names:
            if n.startswith(".rex-"):
                continue
            a = os.path.join(base, n)
            b = os.path.join(after, os.path.relpath(a, before))
            if not os.path.isfile(b) or open(a, "rb").read() != open(b, "rb").read():
                changed.append(os.path.relpath(a, before))
    return changed


def check_log_values(rules, lines):
    """[[scenario.log_value]] rules (M6B camera tests): a number from the log within a window of lines.

    Each rule: name; pattern (regular expression with one group, the number); after / before (substrings of log
    lines bounding the window; before optional); which = "first" | "last" | "min" | "max" (default "last");
    relative_to (the name of an earlier rule: this value minus that one; angle = true wraps the difference to
    -180..180); min / max (the allowed range)."""
    values, problems, shown = {}, [], []
    for rule in rules:
        name = rule["name"]
        pattern = re.compile(rule["pattern"])
        start = next((i for i, l in enumerate(lines) if rule["after"] in l), None)
        if start is None:
            problems.append(f"{name}: no line with {rule['after']!r}")
            continue
        end = len(lines)
        if rule.get("before"):
            end = next((i for i in range(start + 1, len(lines)) if rule["before"] in lines[i]), len(lines))
        found = [float(m.group(1)) for l in lines[start:end] for m in [pattern.search(l)] if m]
        if not found:
            problems.append(f"{name}: no value in the window")
            continue
        which = rule.get("which", "last")
        value = {"first": found[0], "last": found[-1], "min": min(found), "max": max(found)}[which]
        values[name] = value
        if rule.get("relative_to"):
            if rule["relative_to"] not in values:
                problems.append(f"{name}: {rule['relative_to']} has no value")
                continue
            value -= values[rule["relative_to"]]
            if rule.get("angle"):
                value = (value + 180.0) % 360.0 - 180.0
        if ("min" in rule and value < rule["min"]) or ("max" in rule and value > rule["max"]):
            problems.append(f"{name} = {value:.3f} (expected {rule.get('min', '-inf')}..{rule.get('max', 'inf')})")
        shown.append(f"{name}={value:.2f}")
    return problems, ", ".join(shown)


SCRIPT_ITEM = re.compile(r"\[kuf-input-script\] tick (\d+): (.*)$")


def check_input_ticks(rules, lines):
    """[[scenario.input_ticks]] rules (K38): scripted input reaches the game at its tick. An input item logged as
    "tick N" is applied when tick N + 1 starts, before that tick's own trace lines, so between items at N and M a
    per-tick trace has exactly M - N lines: fewer means a tick saw two items (or a trace line is missing), more means
    a tick between them saw none. Returns (problems, summary)."""
    problems, shown = [], []
    for rule in rules:
        name = rule["name"]
        item, update = re.compile(rule["item"]), re.compile(rule["update"])
        start = next((i for i, l in enumerate(lines) if rule["after"] in l), None)
        if start is None:
            problems.append(f"{name}: no line with {rule['after']!r}")
            continue
        end = len(lines)
        if rule.get("before"):
            end = next((i for i in range(start + 1, len(lines)) if rule["before"] in lines[i]), len(lines))
        items, bad, updates, last = 0, [], 0, None
        for line in lines[start:end]:
            m = SCRIPT_ITEM.search(line)
            if m and item.search(m.group(2)):
                tick = int(m.group(1))
                if last is not None and updates != tick - last:
                    bad.append(f"ticks {last}..{tick}: {updates} updates (expected {tick - last})")
                items, last, updates = items + 1, tick, 0
            elif last is not None and update.search(line):
                updates += 1
        if last is not None and updates < 1:
            bad.append(f"tick {last}: no update after the last item")
        if not items:
            problems.append(f"{name}: no input item in the window")
        elif bad:
            problems.append(f"{name}: {len(bad)} of {items} items not one tick each ({'; '.join(bad[:3])})")
        shown.append(f"{name}: {items} items, one tick each" if items and not bad else f"{name}: {len(bad)} bad")
    return problems, ", ".join(shown)


def check_timing(rule, run_dir, cap_dir):
    """[scenario.timing] (M6D): game ticks per real second between two labelled captures, and the run summary."""
    import json
    problems, notes = [], []
    caps = {}
    try:
        for line in open(os.path.join(cap_dir, "captures.jsonl")):
            c = json.loads(line)
            if c.get("label"):
                caps.setdefault(c["label"], c)
    except OSError as e:
        return [f"no captures: {e}"], ""
    a, b = caps.get(rule["from"]), caps.get(rule["to"])
    if not a or not b:
        return [f"missing capture {rule['from']!r} or {rule['to']!r}"], ""
    ticks, wall = b["requested_frame"] - a["requested_frame"], b["time_s"] - a["time_s"]
    rate = ticks / wall if wall > 0 else float("inf")
    lo, hi = rule.get("min_ticks_per_second", 0.0), rule.get("max_ticks_per_second", float("inf"))
    notes.append(f"{ticks} ticks in {wall:.2f} s = {rate:.2f}/s ({rate / 30:.2f}x real time)")
    if not lo <= rate <= hi:
        problems.append(f"{rate:.2f} ticks/s outside [{lo}, {hi}]")
    try:
        summary = json.load(open(os.path.join(run_dir, "user", "run-summary.json")))
    except (OSError, ValueError) as e:
        return problems + [f"no run-summary.json: {e}"], "; ".join(notes)
    pacing = summary.get("pacing", {})
    if "expect_mode" in rule and pacing.get("mode") != rule["expect_mode"]:
        problems.append(f"pacing mode {pacing.get('mode')!r} (expected {rule['expect_mode']!r})")
    if "max_frames_per_tick" in rule or "min_frames_per_tick" in rule:
        per_tick = summary["frames_presented"] / max(1, pacing.get("ticks", 0))
        notes.append(f"{per_tick:.3f} frames per tick")
        if per_tick > rule.get("max_frames_per_tick", float("inf")):
            problems.append(f"{per_tick:.3f} presented frames per tick > {rule['max_frames_per_tick']}")
        if per_tick < rule.get("min_frames_per_tick", 0.0):
            problems.append(f"{per_tick:.3f} presented frames per tick < {rule['min_frames_per_tick']}")
    for key, limit in rule.get("summary_max", {}).items():
        value = pacing.get(key)
        notes.append(f"{key} {value}")
        if value is None or value > limit:
            problems.append(f"pacing.{key} = {value} (at most {limit})")
    if rule.get("game_counter_matches"):
        ours, game = sum(summary.get("sim_tick_timeline", [])), sum(summary.get("game_counter_timeline", []))
        if abs(ours - game) > 2:
            problems.append(f"game tick counter advanced {game}, counted ticks {ours}")
    interval = pacing.get("tick_interval_ms", {})
    notes.append(f"tick interval p50 {interval.get('p50', 0):.2f} ms, p99 {interval.get('p99', 0):.2f} ms")
    return problems, "; ".join(notes)


def run_scenario(path, args):
    with open(path, "rb") as f:
        sc = tomllib.load(f)["scenario"]
    name = os.path.splitext(os.path.basename(path))[0]
    run_id = f"{args.run_prefix}-{name}"
    run_dir = os.path.join(ROOT, "build", "runs", run_id)
    cap_dir = os.path.join(run_dir, "user", "captures")
    frames = ",".join(str(f) for f in sc.get("capture_frames", []))
    cmd = [os.path.join(ROOT, "tools", "run_sandboxed.sh"), "-p", args.preset, "-t", str(sc["seconds"] + 60),
           "-n", run_id, "--gpu"]
    if "script_file" in sc:
        script_path = os.path.join(os.path.dirname(os.path.abspath(path)), sc["script_file"])
        items = script_items(file_script_text(open(script_path).read()))
        cmd += ["--input-script", script_path]
    else:
        items = script_items(sc["script"])
    prev_cap_dir = None
    source = None
    if "saves_from_run" in sc:
        prev_run = os.path.join(ROOT, "build", "runs", f"{args.run_prefix}-{sc['saves_from_run']}")
        compare_run = os.path.join(ROOT, "build", "runs", f"{args.run_prefix}-{sc.get('compare_run', sc['saves_from_run'])}")
        prev_cap_dir = os.path.join(compare_run, "user", "captures")
        source = os.path.join(prev_run, "saves")
        if not os.path.isdir(source):
            print(f"MENU TEST {name}: FAIL (no saves from run {prev_run}; run that scenario first)")
            return False
        if sc.get("damage"):
            source = prepare_damaged_saves(source, os.path.join(ROOT, "build", "runs", f"{run_id}-input-saves"),
                                           sc["damage"])
        cmd += ["--saves-from", source]
    elif "saves_from_dir" in sc:
        source = os.path.realpath(os.path.join(ROOT, sc["saves_from_dir"]))
        if not source.startswith(os.path.join(ROOT, "build") + os.sep) or not os.path.isdir(source):
            print(f"MENU TEST {name}: FAIL (saves_from_dir {sc['saves_from_dir']!r} is not a directory under build/)")
            return False
        if "compare_run" in sc:
            prev_cap_dir = os.path.join(ROOT, "build", "runs", f"{args.run_prefix}-{sc['compare_run']}", "user", "captures")
        cmd += ["--saves-from", source]
    if "compare_dir" in sc:
        prev_cap_dir = os.path.realpath(os.path.join(ROOT, sc["compare_dir"]))
        if not prev_cap_dir.startswith(os.path.join(ROOT, "build") + os.sep) or not os.path.isdir(prev_cap_dir):
            print(f"MENU TEST {name}: FAIL (compare_dir {sc['compare_dir']!r} is not a directory under build/)")
            return False
    if args.cache_from:
        cmd += ["--cache-from", args.cache_from]
    # PC settings (M6B): start from an earlier scenario's pc_settings.toml, or from a file in the repository.
    if "pc_settings_from_run" in sc:
        prev = os.path.join(ROOT, "build", "runs", f"{args.run_prefix}-{sc['pc_settings_from_run']}")
        if not os.path.isfile(os.path.join(prev, "user", "pc_settings.toml")):
            print(f"MENU TEST {name}: FAIL (no pc_settings.toml from run {prev}; run that scenario first)")
            return False
        cmd += ["--pc-settings-from", prev]
    elif "pc_settings_file" in sc:
        cmd += ["--pc-settings-from", os.path.join(ROOT, sc["pc_settings_file"])]
    for f in sc.get("copy_in", []):  # test helpers at /run-data/in/<name> (M7)
        cmd += ["--copy-in", os.path.join(ROOT, f)]
    if args.gdb:
        cmd += ["--gdb", args.gdb]
    cmd += ["--", f"--kuf_exit_after_seconds={sc['seconds']}"]
    if "script_file" not in sc:
        cmd.append("--kuf_input_script=" + "; ".join(items))
    cmd += sc.get("args", [])
    # The scripted routes answer the console's storage question themselves (Return or Escape at about frame 700); the
    # port answers it for players (M7 UX fix 5, --kuf_storage_prompt=auto): scenarios keep it unless they say so.
    if not any(a.startswith("--kuf_storage_prompt") for a in sc.get("args", []) + args.game_arg):
        cmd.append("--kuf_storage_prompt=ask")
    cmd += args.game_arg
    if frames:
        cmd.append(f"--kuf_capture_frames={frames}")
    subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL)

    results = []
    status = int(open(os.path.join(run_dir, "exit_status")).read().strip())
    crashed = os.path.exists(os.path.join(run_dir, "user", "crash-report.txt"))
    log = ""
    for fname in sorted(os.listdir(run_dir)):
        if fname.startswith("kuf_cod") and fname.endswith(".log"):
            log += open(os.path.join(run_dir, fname), errors="replace").read()
    errors, known = known_errors.classify(log.splitlines())
    expected = [re.compile(p) for p in sc.get("expect_errors", [])]
    absent_expected = [p.pattern for p in expected if not any(p.search(e) for e in errors)]
    errors = [e for e in errors if not any(p.search(e) for p in expected)]
    if absent_expected:
        errors.append(f"expected error lines missing: {absent_expected}")
    results.append(("run", status == sc.get("expect_exit", 0) and not crashed and not errors,
                    f"exit={status} crash_report={crashed} errors={errors[:3]}"
                    + (f" known={known}" if known else "")))

    applied = len(set(re.findall(r"\[kuf-input-script\] tick \d+: (.*?)(?: \(release\))?$", log, re.M)))
    if sc.get("expect_exit", 0) == 0:
        results.append(("script", applied == len(items), f"{applied}/{len(items)} scripted items applied"))
    else:  # killed on purpose: the rest of the script never runs
        results.append(("script", applied > 0, f"{applied}/{len(items)} scripted items applied before the exit"))

    missing = [e for e in sc.get("log_expect", []) if e not in log]
    missing += [f"(absent) {e}" for e in sc.get("log_absent", []) if e in log]
    pos, seq_ok = 0, True
    for line in sc.get("log_sequence", []):
        found = log.find(line, pos)
        if found < 0:
            seq_ok = False
            break
        pos = found + len(line)
    results.append(("log", not missing and seq_ok,
                    f"missing={missing}" + ("" if seq_ok else "; log_sequence out of order or incomplete")))

    saves_dir = os.path.join(run_dir, "saves")
    if "expect_saves" in sc or "expect_no_saves" in sc:
        missing_saves = [g for g in sc.get("expect_saves", []) if not glob.glob(os.path.join(saves_dir, g))]
        unwanted = [g for g in sc.get("expect_no_saves", []) if glob.glob(os.path.join(saves_dir, g))]
        results.append(("saves", not missing_saves and not unwanted,
                        f"missing={missing_saves} unexpected={unwanted}"))
    if sc.get("expect_saves_unchanged") and source:
        changed = saves_changed(source, saves_dir)
        results.append(("saves unchanged", not changed, f"changed={changed[:6]}"))

    if sc.get("log_value"):
        problems, summary = check_log_values(sc["log_value"], log.splitlines())
        results.append(("log values", not problems, "; ".join(problems) or summary))

    if sc.get("input_ticks"):
        problems, summary = check_input_ticks(sc["input_ticks"], log.splitlines())
        results.append(("input ticks", not problems, "; ".join(problems) or summary))

    # PC settings (M6B): what the run left in its pc_settings.toml.
    settings_path = os.path.join(run_dir, "user", "pc_settings.toml")
    if sc.get("pc_settings_absent"):
        results.append(("pc settings", not os.path.exists(settings_path),
                        "no pc_settings.toml written" if not os.path.exists(settings_path)
                        else "pc_settings.toml was written"))
    if "pc_settings_expect" in sc:
        problems = []
        try:
            with open(settings_path, "rb") as f:
                written = tomllib.load(f)
            for key, want in sc["pc_settings_expect"].items():
                table, _, field = key.partition(".")
                got = written.get(table, {}).get(field)
                if got != want:
                    problems.append(f"{key}={got!r} (expected {want!r})")
        except (OSError, tomllib.TOMLDecodeError) as e:
            problems.append(f"cannot read {settings_path}: {e}")
        results.append(("pc settings", not problems,
                        "; ".join(problems) or f"{len(sc['pc_settings_expect'])} values as expected"))

    if "timing" in sc:
        problems, summary = check_timing(sc["timing"], run_dir, cap_dir)
        results.append(("timing", not problems, "; ".join(problems + [summary]) if problems else summary))

    if sc.get("files"):
        problems = compare_files(sc["files"], {"saves": saves_dir, "input": source})
        results.append(("files", not problems, "; ".join(problems) or f"{len(sc['files'])} comparisons as expected"))

    check_cmd = [sys.executable, "-I", CHECKER, "check", cap_dir, path] + ([prev_cap_dir] if prev_cap_dir else [])
    out = subprocess.run(check_cmd, capture_output=True, text=True)
    fails = [l.strip() for l in out.stdout.splitlines() if "[FAIL]" in l]
    results.append(("images", out.returncode == 0, "; ".join(fails)[:700] or "capture checks passed"))

    ref_dir = os.path.join(args.references_root, sc.get("references", f"v1-menu-{name}"))
    passed_so_far = all(ok for _, ok, _ in results)
    if args.baseline:
        if passed_so_far:
            b = subprocess.run([sys.executable, "-I", CHECKER, "baseline", cap_dir, ref_dir], capture_output=True,
                               text=True)
            results.append(("baseline", b.returncode == 0, b.stdout.strip() or b.stderr.strip()))
        else:
            results.append(("baseline", False, "not stored: the scenario failed"))
    elif os.path.exists(os.path.join(ref_dir, "captures.jsonl")):
        c = subprocess.run([sys.executable, "-I", CHECKER, "compare", cap_dir, ref_dir, path], capture_output=True,
                           text=True)
        cfails = [l.strip() for l in c.stdout.splitlines() if "[FAIL]" in l]
        results.append(("compare", c.returncode == 0, "; ".join(cfails)[:500] or "matches local references"))
    else:
        results.append(("compare", True, f"skipped: no local references in {ref_dir}"))

    ok = all(r for _, r, _ in results)
    lines = [f"MENU TEST {name}: {'PASS' if ok else 'FAIL'} ({sc['description']}; run {run_id})"]
    lines += [f"  [{'ok' if r else 'FAIL'}] {n}: {d}" for n, r, d in results]
    text = "\n".join(lines) + "\n"
    open(os.path.join(run_dir, "menu-report.txt"), "w").write(text)
    print(text, end="", flush=True)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="linux-amd64-relwithdebinfo")
    ap.add_argument("--run-prefix", default="menu")
    ap.add_argument("--references-root", default=os.path.join(ROOT, "build", "references"))
    ap.add_argument("--baseline", action="store_true")
    ap.add_argument("--game-arg", action="append", default=[],
                    help="extra game option for every scenario (e.g. --game-arg=--kuf_profile=true)")
    ap.add_argument("--cache-from", help="start every scenario with a copy of this shader/pipeline cache (a cache "
                    "or run directory, only read): measures with a warm cache")
    ap.add_argument("--gdb", help="run the game under this gdb script (run_sandboxed.sh --gdb; output in gdb.txt)")
    ap.add_argument("scenarios", nargs="+")
    args = ap.parse_args()
    results = [run_scenario(p, args) for p in args.scenarios]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
