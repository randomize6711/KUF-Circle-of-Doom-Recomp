#!/usr/bin/env python3
"""Audits an unpacked portable edition or Windows package (M16; the release audit's checks: audit_release.py): no game
program (kuf_cod, kuf_cod.exe), no generated code, no translated function in any binary or archive (ELF, PE/COFF;
llvm-nm), no console key except the retail XEX key in the launcher (Play KUF), no game data or Xbox executable, no private keys or tokens, no personal data (home path, user
name, git e-mail). Prints AUDIT: clean or the problems; exit 1 on a problem.

usage: audit_portable.py DIR [--nm LLVM_NM]
"""
import argparse
import importlib.util
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("audit_release", os.path.join(HERE, "audit_release.py"))
ar = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ar)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("dir")
    p.add_argument("--nm", default="llvm-nm")
    a = p.parse_args()
    hooks = ar.own_hook_names()
    personal = ar.personal_patterns()
    problems, files, binaries, own = [], 0, 0, 0
    for d, _, names in os.walk(a.dir):
        for n in names:
            path = os.path.join(d, n)
            rel = os.path.relpath(path, a.dir)
            files += 1
            if os.path.islink(path):
                problems.append(f"{rel} is a link")
                continue
            data = open(path, "rb").read()
            base = n.lower()
            if base in ("kuf_cod", "kuf_cod.exe"):
                problems.append(f"{rel}: the game's program must not be distributed")
            if re.search(r"kuf_cod_(recomp|funcs|init|register)\b", base) or "/generated/" in "/" + rel.replace(os.sep, "/"):
                problems.append(f"{rel} looks like generated code")
            if any(__import__("fnmatch").fnmatch(base, g) for g in ar.GAME_DATA):
                problems.append(f"{rel} looks like game data")
            if data[:4] == b"XEX2":
                problems.append(f"{rel} is an Xbox 360 executable")
            why = ar.key_problem(rel.replace(os.sep, "/"), data)  # the launcher carries the retail key, nothing else
            if why:
                problems.append(why)
            # llvm-mingw's own build machine (GitHub's runner) is named in its prebuilt libraries: not personal data.
            scan = data.replace(b"/home/runner/work/llvm-mingw/", b"<llvm-mingw-ci>/")
            for rx, why in ar.SECRETS + personal:
                if rx.search(scan):
                    problems.append(f"{rel} contains {why}")
            if data[:2] == b"MZ" or data[:4] == b"\x7fELF" or data[:8] == b"!<arch>\n":
                binaries += 1
                out = subprocess.run([a.nm, "--defined-only", path], capture_output=True, text=True).stdout
                for name in re.findall(r" [TtDdRrBbWw] (?:__imp__)?(sub_8[0-9A-Fa-f]{7})$", out, re.M):
                    if "sub_" + name[4:].upper() in hooks:
                        own += 1
                    else:
                        problems.append(f"{rel} defines {name}: translated code")
                        break
    print(f"{files} files, {binaries} binaries checked; {own} own hook definitions")
    for x in problems:
        print("PROBLEM: " + x)
    print("AUDIT: clean" if not problems else f"AUDIT: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
