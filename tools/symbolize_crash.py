#!/usr/bin/env python3
"""Turn a crash report's host frames into original Xbox 360 instruction addresses.

Usage: symbolize_crash.py <crash-report.txt> <bin_dir> <generated_dir> [-o out.txt]

For every `(<module>+0x<offset>)` frame in the report, llvm-symbolizer resolves the module's line tables
(RelWithDebInfo builds emit them for generated code) to `<generated file>:<line>`. In the generated C++,
each guest instruction is preceded by a `// <mnemonic> <operands>` comment, so the active guest instruction
at a line is the last such comment above it; its address is rebuilt from the function start
(PPCFuncMappings) and the `loc_XXXXXXXX:` labels, the same way tools/audit_coverage.py does.
"""
import glob
import os
import re
import subprocess
import sys

report, bin_dir, gen_dir = sys.argv[1:4]
out_path = sys.argv[sys.argv.index("-o") + 1] if "-o" in sys.argv else None

func_addr = {}
init = glob.glob(os.path.join(gen_dir, "*_init.cpp"))
if init:
    for m in re.finditer(r"\{ 0x([0-9A-Fa-f]{8}), (\w+) \}", open(init[0]).read()):
        func_addr[m.group(2)] = int(m.group(1), 16)

_line_maps = {}


def line_map(path):
    """line number -> (guest address, mnemonic+operands, function name) of the active instruction."""
    if path in _line_maps:
        return _line_maps[path]
    result = {}
    addr = None
    func = None
    current = None
    for n, line in enumerate(open(path), start=1):
        m = re.match(r"^DEFINE_REX_FUNC\((\w+)\)", line)
        if m:
            func = m.group(1)
            addr = func_addr.get(func)
            current = None
        else:
            lm = re.match(r"^loc_([0-9A-F]{8}):", line)
            if lm:
                addr = int(lm.group(1), 16)
            else:
                cm = re.match(r"^\t// (\S.*)$", line)
                if cm and addr is not None:
                    current = (addr, cm.group(1).strip(), func)
                    addr += 4
        if current:
            result[n] = current
    _line_maps[path] = result
    return result


def symbolize(module, offset):
    obj = os.path.join(bin_dir, module)
    if not os.path.exists(obj):
        return []
    out = subprocess.run(["llvm-symbolizer", "--inlining", "--obj", obj, hex(offset)],
                         capture_output=True, text=True).stdout.strip().split("\n")
    frames = []
    for name, loc in zip(out[0::2], out[1::2]):
        m = re.match(r"(.*):(\d+):(\d+)$", loc)
        frames.append((name, m.group(1) if m else loc, int(m.group(2)) if m else 0))
    return frames


lines_out = []
first_guest = None
for line in open(report):
    lines_out.append(line.rstrip("\n"))
    m = re.search(r"\(([^()+]+)\+0x([0-9a-f]+)\)", line)
    if not m or not line.lstrip().startswith(("host_pc", "#")):
        continue
    for name, path, lineno in symbolize(m.group(1), int(m.group(2), 16)):
        entry = f"        -> {name} at {os.path.basename(path)}:{lineno}"
        if path.startswith(os.path.abspath(gen_dir)) or "/generated/" in path:
            if os.path.exists(path):
                info = line_map(path).get(lineno)
                if info:
                    entry += f"  guest 0x{info[0]:08X} [{info[1]}] in {info[2]}"
                    if first_guest is None:
                        first_guest = info
        lines_out.append(entry)

summary = ("faulting_guest_instruction: " +
           (f"0x{first_guest[0]:08X} [{first_guest[1]}] in {first_guest[2]}" if first_guest else "unknown"))
text = summary + "\n" + "\n".join(lines_out) + "\n"
if out_path:
    open(out_path, "w").write(text)
print(summary)
