#!/usr/bin/env python3
"""Condense a gdb `thread apply all bt` dump into per-thread guest call stacks.

Usage: symbolize_gdb_dump.py <gdb stdout> <generated_dir>

Frames in generated code become `guest 0x... [instruction] in sub_...`; the innermost host frames (where a
thread is blocked: futex waits, the GPU thread, ...) are kept by name.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from guestmap import GuestMap  # noqa: E402

dump, gen_dir = sys.argv[1:3]
gm = GuestMap(gen_dir)
text = open(dump).read()
threads = re.split(r"\n(?=Thread \d+ )", text[text.find("Thread "):])
for block in threads:
    lines = block.strip().split("\n")
    header = lines[0]
    host, guest = [], []
    for line in lines[1:]:
        m = re.match(r"#(\d+)\s+(?:0x[0-9a-f]+ in )?(\S+).*?(?: at (\S+):(\d+))?$", line.strip())
        if not m:
            continue
        name, path, ln = m.group(2), m.group(3), m.group(4)
        info = gm.lookup(path, int(ln)) if path and "kuf_cod_recomp" in path else None
        if info:
            guest.append(f"guest 0x{info[0]:08X} [{info[1]}] in {info[2]}")
        elif not guest:
            host.append(name)
    print(header)
    print("  host: " + " <- ".join(host[:6]))
    for g in guest[:12]:
        print("  " + g)
