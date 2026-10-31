#!/usr/bin/env python3
"""Read-only inspection of Circle of Doom saves (M5B) for diagnostics. Never writes anything.

Usage: kuf_save.py <saves dir | run dir>        e.g. build/runs/<run-id> or a --save-dir

A saves directory (--content_root) holds Xbox 360 content packages as directories:
  <XUID>/424507D1/00000001/Circle of Doom - Character Save/   the game's one save package
      GlobalSetting                                           settings shared by all characters
      <XX>_<n>                                                one file per character (KD Kendal, LH Leinhart,
                                                              CL Celine, CU Curian, DU Duane, RG Regnier)
      .rex-manifest                                           size + SHA-256 of each file at the last commit
  <XUID>/424507D1/Headers/00000001/<package>.header           XCONTENT_AGGREGATE_DATA for enumeration
Runtime leftovers beside a package (docs/SAVES.md): <package>.rex-staging (the open session's working copy),
<package>.rex-interrupted (uncommitted changes of a session that ended while writing; never loaded),
<package>.rex-damaged (a damaged package the game was told to delete, kept for manual recovery).

For each package: files and sizes, the manifest check (ok / no manifest / damaged + why), leftovers.
Exit status: 0 = every package verified or has no manifest; 1 = a damaged package or unreadable input.
"""
import hashlib
import os
import sys

TITLE = "424507D1"
SAVE_TYPE = "00000001"
CHARACTERS = {"KD": "Kendal", "LH": "Leinhart", "CL": "Celine", "CU": "Curian", "DU": "Duane", "RG": "Regnier"}


def files_of(pkg):
    out = {}
    for base, _, names in os.walk(pkg):
        for n in names:
            p = os.path.join(base, n)
            rel = os.path.relpath(p, pkg).replace(os.sep, "/")
            if rel != ".rex-manifest":
                out[rel] = p
    return out


def verify(pkg):
    """('ok' | 'no manifest' | 'damaged', [details]) -- same rules as content_txn::Verify."""
    manifest = os.path.join(pkg, ".rex-manifest")
    if not os.path.exists(manifest):
        return "no manifest", []
    lines = open(manifest, encoding="utf-8", errors="replace").read().splitlines()
    if not lines or lines[0] != "rex-content-manifest 1":
        return "damaged", ["unreadable manifest"]
    expected = {}
    for line in lines[1:]:
        parts = line.split(" ", 2)
        if len(parts) != 3 or len(parts[0]) != 64:
            return "damaged", ["unreadable manifest"]
        expected[parts[2]] = (int(parts[1]), parts[0])
    actual = {}
    for rel, p in files_of(pkg).items():
        data = open(p, "rb").read()
        actual[rel] = (len(data), hashlib.sha256(data).hexdigest())
    problems = []
    for rel, (size, digest) in sorted(expected.items()):
        if rel not in actual:
            problems.append(f"{rel}: missing")
        elif actual[rel][0] != size:
            problems.append(f"{rel}: size {actual[rel][0]}, expected {size}")
        elif actual[rel][1] != digest:
            problems.append(f"{rel}: content differs")
    problems += [f"{rel}: unexpected file" for rel in sorted(set(actual) - set(expected))]
    return ("damaged" if problems else "ok"), problems


def describe(name):
    prefix = name.split("_", 1)[0]
    if name == "GlobalSetting":
        return "settings shared by all characters"
    if prefix in CHARACTERS and "_" in name:
        return f"character file ({CHARACTERS[prefix]})"
    return ""


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__.split("\n\n")[1])
    root = argv[1]
    if os.path.isdir(os.path.join(root, "saves")):
        root = os.path.join(root, "saves")
    if not os.path.isdir(root):
        print(f"not a directory: {root}")
        return 1
    damaged = False
    found = False
    for xuid in sorted(os.listdir(root)):
        type_dir = os.path.join(root, xuid, TITLE, SAVE_TYPE)
        if not os.path.isdir(type_dir):
            continue
        for entry in sorted(os.listdir(type_dir)):
            path = os.path.join(type_dir, entry)
            if not os.path.isdir(path):
                continue
            if entry.endswith((".rex-staging", ".rex-interrupted", ".rex-previous", ".rex-snapshot", ".rex-damaged")):
                kind = entry.rsplit(".rex-", 1)[1]
                note = {"staging": "working copy of an open (or ended) session",
                        "interrupted": "uncommitted changes of an interrupted session: never loaded",
                        "previous": "previous version from an interrupted commit (restored on next start)",
                        "snapshot": "unfinished flush copy (removed on next start)",
                        "damaged": "a damaged package the game deleted, kept for manual recovery: never loaded"}[kind]
                print(f"  leftover {entry}: {note}")
                continue
            found = True
            header = os.path.join(root, xuid, TITLE, "Headers", SAVE_TYPE, entry + ".header")
            state, problems = verify(path)
            damaged |= state == "damaged"
            print(f"package {xuid}/{TITLE}/{SAVE_TYPE}/{entry}")
            print(f"  header: {'present' if os.path.exists(header) else 'MISSING'}")
            for rel, p in sorted(files_of(path).items()):
                print(f"  {os.path.getsize(p):8d}  {rel:16s} {describe(rel)}")
            print(f"  manifest: {state}" + (f" -- {'; '.join(problems)}" if problems else ""))
    if not found:
        print(f"no save packages in {root}")
    return 1 if damaged else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
