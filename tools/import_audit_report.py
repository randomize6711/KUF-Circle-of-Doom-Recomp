#!/usr/bin/env python3
"""Classify the game's imports against the ReXGlue runtime (static) and an audit run (dynamic).

Usage: import_audit_report.py <imports.txt> <sdk_dir> [run-summary.json ...] [--md out.md] [--reviewed reviewed.toml]

Static: finds each import's REX_EXPORT / REX_EXPORT_STUB in the SDK kernel sources and scans the
implementing function body for markers of incomplete behaviour (stub macros, TODO/STUB/unimplemented
comments, debug::Break, assert_always, X_STATUS_NOT_IMPLEMENTED, unconditional success returns with no
other code). Dynamic: call counts from run-summary.json files written by audit builds.

Classification (per the M2 brief):
  Unused                  not called in any audit run
  Intentionally stubbed   SDK REX_EXPORT_STUB*, or reviewed and accepted as a stub for this game
  Partially implemented   called; implementation carries incomplete-behaviour markers
  Fully implemented       called; implementation without markers (or reviewed)
  Not yet investigated    called, but the implementation could not be located
`--reviewed` (TOML: [name] status = "...", note = "...") overrides with manual review results.
"""
import glob
import json
import os
import re
import sys

args = sys.argv[1:]
md_out = None
reviewed_path = None
if "--md" in args:
    i = args.index("--md"); md_out = args[i + 1]; del args[i:i + 2]
if "--reviewed" in args:
    i = args.index("--reviewed"); reviewed_path = args[i + 1]; del args[i:i + 2]
imports_path, sdk_dir, *summaries = args

imports = []
for line in open(imports_path):
    line = line.strip()
    if line and not line.startswith("#"):
        name, thunk, lib = line.split()[:3]
        imports.append((name, thunk, lib))

sources = {}
for path in glob.glob(os.path.join(sdk_dir, "src", "kernel", "**", "*.cpp"), recursive=True):
    sources[path] = open(path, encoding="utf-8", errors="replace").read()

MARKERS = [
    (r"\bTODO\b", "TODO"), (r"\bSTUB\b|[Ss]tub(bed)?\b", "stub"), (r"[Uu]nimplemented|NOT_IMPLEMENTED", "unimplemented"),
    (r"debug::Break\(\)", "debug::Break"), (r"assert_always\(", "assert_always"),
    (r"REXKRNL_IMPORT_FAIL|FAILED", "fail-path"),
]


def find_impl(name):
    """Return (kind, impl function name, file, body) for an import."""
    pat = re.compile(r"(REX_EXPORT(?:_STUB(?:_RETURN)?)?)\(__imp__%s\b\s*(?:,\s*([\w:]+))?" % re.escape(name))
    for path, text in sources.items():
        m = pat.search(text)
        if not m:
            continue
        kind, impl = m.group(1), m.group(2)
        if kind != "REX_EXPORT" or not impl:
            return kind, impl, path, ""
        short = impl.split("::")[-1]
        fm = re.search(r"\n[^\n;]*\b%s\s*\(" % re.escape(short), text)
        if not fm:
            return kind, impl, path, ""
        start = text.index("{", fm.end())
        depth, i = 0, start
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        return kind, impl, path, text[start:i + 1]
    return None, None, None, ""


calls = {}
for s in summaries:
    d = json.load(open(s))
    for e in d.get("imports") or []:
        c = calls.setdefault(e["name"], {"calls": 0, "first_call_s": e["first_call_s"]})
        c["calls"] += e["calls"]
        c["first_call_s"] = min(c["first_call_s"], e["first_call_s"])

reviewed = {}
if reviewed_path:
    import tomllib
    reviewed = tomllib.load(open(reviewed_path, "rb"))

rows = []
for name, thunk, lib in imports:
    kind, impl, path, body = find_impl(name)
    markers = sorted({label for pat, label in MARKERS if re.search(pat, body)})
    called = calls.get(name)
    if kind is None:
        static = "not found"
    elif kind.startswith("REX_EXPORT_STUB"):
        static = "SDK stub"
    else:
        static = "implemented" + (" (" + ", ".join(markers) + ")" if markers else "")
    if not called:
        status = "Unused"
    elif kind is None:
        status = "Not yet investigated"
    elif kind.startswith("REX_EXPORT_STUB"):
        status = "Intentionally stubbed"
    elif markers:
        status = "Partially implemented"
    else:
        status = "Fully implemented"
    note = ""
    if name in reviewed:
        status = reviewed[name].get("status", status)
        note = reviewed[name].get("note", "")
    rows.append({"name": name, "lib": lib.split(".")[0], "static": static,
                 "file": os.path.relpath(path, sdk_dir) if path else "", "calls": called["calls"] if called else 0,
                 "first_call_s": called["first_call_s"] if called else None, "status": status, "note": note})

order = {"Not yet investigated": 0, "Partially implemented": 1, "Intentionally stubbed": 2, "Fully implemented": 3,
         "Unused": 4}
rows.sort(key=lambda r: (order[r["status"]], r["first_call_s"] if r["first_call_s"] is not None else 1e9, r["name"]))
counts = {}
for r in rows:
    counts[r["status"]] = counts.get(r["status"], 0) + 1
print("imports:", len(rows), counts)
for r in rows:
    if r["status"] not in ("Unused",):
        print(f"  {r['status']:22s} {r['name']:40s} calls={r['calls']:<9} {r['static']}")

if md_out:
    with open(md_out, "w") as f:
        f.write("| Import | Lib | Status | Calls (audit) | First call (s) | SDK implementation | Note |\n")
        f.write("|---|---|---|---|---|---|---|\n")
        for r in rows:
            fc = f"{r['first_call_s']:.2f}" if r["first_call_s"] is not None else ""
            f.write(f"| {r['name']} | {r['lib']} | {r['status']} | {r['calls']} | {fc} | {r['static']} "
                    f"`{r['file']}` | {r['note']} |\n")
