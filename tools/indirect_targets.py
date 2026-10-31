#!/usr/bin/env python3
"""Find functions that are reached only through code pointers stored in .rdata/.data (small virtual
methods, dispatch-table entries): no .pdata record, never a `bl` target, and no RTTI for ReXGlue's vtable
scanner. Without an entry in the function table, an indirect call to them aborts at runtime
("Call to invalid or unregistered function").

Usage: indirect_targets.py <generated_dir> <image.bin> <coverage.txt> <flat.dis> <out.toml>

coverage.txt is the report written by audit_coverage.py for the same generated_dir.

A candidate's extent is found by walking its local control flow (conditional branches, `b` to non-function
targets; a `b` to any function entry is a tail call). It is accepted when everything it reaches is either code nobody emitted or a gap-fill fragment
(a generated function that is not a .pdata start, not a `bl` target and not referenced from data); such
fragments are absorbed by registering the candidate with an explicit `end`. It is rejected when it runs
into the body of a real function: those blocks are continuations or SEH/C++ EH funclets of that function.
"""
import bisect, glob, os, re, struct, sys

gen_dir, img_path, cov_path, dis_path, out_path = sys.argv[1:6]
B = 0x82000000
CODE = [(0x82150000, 0x8CE084), (0x82A1E200, 0x16990), (0x82A34C00, 0x17F54), (0x82A4CC00, 0x6ECC),
        (0x82A53C00, 0x1ED8), (0x82A55C00, 0x16E60), (0x82A6CC00, 0x1300), (0x82A6E000, 0x2D4),
        (0x82A6E400, 0x2D50)]
DATA = [(0x82000600, 0x11C1B4), (0x82A80000, 0x702BC)]  # .rdata, .data
PDATA = (0x8211C800, 0x2C3D0)

img = open(img_path, "rb").read()
init = open(glob.glob(os.path.join(gen_dir, "*_init.cpp"))[0]).read()
funcs = sorted({int(m.group(1), 16) for m in re.finditer(r"\{ 0x([0-9A-Fa-f]{8}), \w+ \}", init)})
func_set = set(funcs)

uncovered = set()
for line in open(cov_path):
    m = re.match(r"\s+0x([0-9a-f]+) 0x([0-9a-f]+)\s+\d+", line)
    if m:
        uncovered.update(range(int(m.group(1), 16), int(m.group(2), 16), 4))

dis = {}
bl_targets = set()
for line in open(dis_path):
    m = re.match(r"([0-9a-f]{8}):\s+(\S+)\s*(.*)", line)
    if m:
        a = int(m.group(1), 16)
        dis[a] = (m.group(2), m.group(3))
        if m.group(2) == "bl":
            t = re.search(r"0x([0-9a-f]{8})", m.group(3))
            if t:
                bl_targets.add(int(t.group(1), 16))

pdata_starts = {a for a, _ in struct.iter_unpack(">II", img[PDATA[0] - B:PDATA[0] - B + PDATA[1]]) if a}

data_refs = {}
for start, size in DATA:
    for off in range(start - B, start - B + size - 3, 4):
        w = struct.unpack_from(">I", img, off)[0]
        if w % 4 == 0 and any(s <= w < s + n for s, n in CODE):
            data_refs.setdefault(w, off + B)


# Code addresses materialised in code with `lis rX,hi` + `addi/ori rY,rX,lo` (callbacks, thread entry points,
# member-function thunks passed as pointers). Tracked per register until the next block terminator.
code_refs = {}
lis_val = {}
for a in sorted(dis):
    mn, ops = dis[a]
    if mn == "lis" and ops.count(",") == 1:
        rd, imm = [x.strip() for x in ops.split(",")]
        lis_val[int(rd)] = (int(imm) & 0xFFFF) << 16
    elif mn in ("addi", "ori") and ops.count(",") == 2:
        rd, rs, imm = [x.strip() for x in ops.split(",")]
        rs = int(rs)
        if rs in lis_val:
            imm = int(imm)
            addr = (lis_val[rs] + imm) & 0xFFFFFFFF if mn == "addi" else lis_val[rs] | (imm & 0xFFFF)
            if addr % 4 == 0 and any(s0 <= addr < s0 + n for s0, n in CODE):
                code_refs.setdefault(addr, a)
    if mn in ("blr", "b", "bctr", "bl", "bctrl"):
        lis_val.clear()


def is_real_function(f):
    return f in pdata_starts or f in bl_targets or f in data_refs or f in code_refs


def owner(a):
    i = bisect.bisect_right(funcs, a) - 1
    return funcs[i] if i >= 0 else None


def extent(start, split=None):
    """Return the exclusive end address of the candidate, or None if it runs into a real function.
    `split` is the gap-fill function the candidate is carved out of, whose body it may walk."""
    todo, seen = [start], set()
    while todo:
        a = todo.pop()
        while a not in seen:
            if a != start and a in func_set and is_real_function(a):
                return None  # fell or branched onto another real function's entry
            if a not in uncovered:
                f = owner(a)
                if f != split and (f is None or f < start or is_real_function(f)):
                    return None  # inside the body of a real function
            seen.add(a)
            mn, ops = dis.get(a, ("?", ""))
            word = struct.unpack_from(">I", img, a - B)[0]
            # LLVM cannot decode most Xbox VMX128 encodings (primary opcodes 4/5/6); those are valid code.
            if mn == "?" or (mn == "<unknown>" and (word >> 26) not in (4, 5, 6)) or len(seen) > 4096:
                return None
            tgt = re.search(r"0x([0-9a-f]{8})", ops)
            if mn.startswith("b") and not mn.startswith("bl") and tgt:
                t = int(tgt.group(1), 16)
                if mn == "b":
                    if t in func_set:
                        break  # unconditional jump to a function entry: tail call
                    a = t
                    continue
                todo.append(t)  # conditional branch: follow both edges
            if mn in ("blr", "bctr", "b"):
                break
            a += 4
    return max(seen) + 4


# Keep entries from previous runs: once registered they are function starts and no longer reported.
targets = {}
if os.path.exists(out_path):
    for m in re.finditer(r"^0x([0-9A-F]{8}) = \{ name = \"\w+\"(?:, end = 0x([0-9A-F]{8}))? \}\s+# ref 0x([0-9A-F]{8})",
                         open(out_path).read(), re.M):
        targets[int(m.group(1), 16)] = (int(m.group(2), 16) if m.group(2) else None, int(m.group(3), 16))
rejected = {}
for w, ref in data_refs.items():
    if w in func_set or w not in uncovered or w in targets:
        continue
    end = extent(w)
    if end is None:
        rejected[w] = ref
    else:
        targets[w] = (end, ref)

TERMINATORS = ("blr", "b", "bctr")
for w, ref in code_refs.items():
    if w in func_set or w in targets or w in rejected:
        continue
    prev = dis.get(w - 4, ("?", ""))[0]
    prev_word = struct.unpack_from(">I", img, w - 4 - B)[0]
    if prev not in TERMINATORS and prev_word != 0:
        continue  # not a clean entry boundary (e.g. a label inside a function or a table address)
    if w in uncovered:
        end = extent(w)
    else:
        host = owner(w)
        if host is None or is_real_function(host):
            continue  # never split a .pdata / call-target / data-referenced function
        end = extent(w, split=host)
    if end is None:
        rejected[w] = ref
    else:
        targets[w] = (end, ref)

with open(out_path, "w") as f:
    f.write("# Generated by tools/indirect_targets.py -- do not edit by hand.\n")
    f.write("# Functions reached only through pointers: stored in .rdata/.data (vtables, dispatch tables) or\n")
    f.write("# built in code with lis+addi/ori (callbacks, member thunks). `end` absorbs gap-fill fragments;\n")
    f.write("# `ref` is the data address or the instruction that references the function.\n\n")
    f.write("[functions]\n")
    for t in sorted(targets):
        end, ref = targets[t]
        end_s = f", end = 0x{end:08X}" if end else ""
        f.write(f"0x{t:08X} = {{ name = \"sub_{t:08X}\"{end_s} }}  # ref 0x{ref:08X}\n")
print(f"{len(targets)} indirect-only functions in {out_path}; rejected {len(rejected)} (continuations/funclets):")
for t in sorted(rejected):
    print(f"  0x{t:08X} (ref 0x{rejected[t]:08X})")
