#!/usr/bin/env python3
"""Checks a Windows acceptance result (tools/windows/acceptance/Run-Acceptance.ps1, docs/WINDOWS_TEST.md) on Linux.

  tools/windows/verify_acceptance.py RESULTS.zip

Unpacks the archive into a new folder under build/windows-acceptance/ (names checked: no absolute paths or ..),
prints the checks made on Windows (results.json), then checks every game run with the Linux rules:
tools/menu_test.py --verify-only (exit and crash report, every scripted item, the log lines in order, the
captures, saves and settings expectations). Exit 0 when nothing failed.
"""
import json
import os
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXTRA_RUNS = {"win-moved-": "quit_popup", "win-setup-": "quit_popup"}  # runs of a plan scenario under another prefix


def unpack(archive):
    name = os.path.splitext(os.path.basename(archive))[0]
    dest = os.path.join(ROOT, "build", "windows-acceptance", name)
    n = 0
    while os.path.exists(dest + (f".{n}" if n else "")):
        n += 1
    dest += f".{n}" if n else ""
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            parts = info.filename.replace("\\", "/").split("/")
            if info.filename.startswith(("/", "\\")) or ".." in parts or ":" in parts[0]:
                sys.exit(f"refused: {info.filename!r} in {archive}")
        z.extractall(dest)
    return dest


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    res = unpack(sys.argv[1])
    print(f"unpacked to {res}")
    checks = json.load(open(os.path.join(res, "results.json"), encoding="utf-8-sig"))
    if isinstance(checks, dict):
        checks = [checks]
    pc = os.path.join(res, "pc.json")
    if os.path.exists(pc):
        for k, v in json.load(open(pc, encoding="utf-8-sig")).items():
            print(f"  {k}: {v}")
    print("\n== checks on Windows")
    for c in checks:
        print(f"  [{c['status']}] {c['id']}: {c['detail']}")
    plan = json.load(open(os.path.join(res, "plan.json"), encoding="utf-8-sig"))
    runs = os.path.join(res, "runs")
    by_name = {s["name"]: s for s in plan["scenarios"]}
    jobs = []
    for d in sorted(os.listdir(runs)) if os.path.isdir(runs) else []:
        for prefix, name in list(EXTRA_RUNS.items()) + [(plan["prefix"] + "-", None)]:
            if d.startswith(prefix):
                scenario = name or d[len(prefix):]
                if scenario in by_name and (name is None or d == prefix + name):
                    jobs.append((prefix[:-1], os.path.join(ROOT, by_name[scenario]["scenario"]), d))
                break
    print(f"\n== game runs, checked with the Linux rules ({len(jobs)})")
    failed = []
    for prefix, path, d in jobs:
        out = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "menu_test.py"), "--run-prefix", prefix,
                              "--verify-only", runs, path], capture_output=True, text=True)
        print(out.stdout.rstrip())
        if out.returncode != 0:
            failed.append(d)
    windows_failed = [c["id"] for c in checks if c["status"] == "fail"]
    manual = [c["id"] for c in checks if c["status"] in ("manual", "skip")]
    print("\n== summary")
    print(f"  Windows checks failed: {windows_failed or 'none'}")
    print(f"  game runs failed (Linux rules): {failed or 'none'}")
    print(f"  manual or not run: {manual or 'none'}")
    return 1 if windows_failed or failed else 0


if __name__ == "__main__":
    sys.exit(main())
