#!/usr/bin/env python3
"""Independent coverage audit of ReXGlue output against the original image.

Usage: audit_coverage.py <generated_dir> <flat.dis> <image.bin> [--report out.txt]

ReXGlue emits one `// <mnemonic> <operands>` comment per guest instruction, in address order, plus
`loc_XXXXXXXX:` labels at branch targets. We rebuild the address of every emitted instruction from the
function start and the labels, check the mnemonic against LLVM's disassembly (flat.dis, produced by
llvm-objdump over the image), and list every code word in the executable sections that no generated
function covers. Exit status is non-zero on any mnemonic mismatch.
"""
import re, sys, struct, glob, os, collections

gen_dir, dis_path, img_path = sys.argv[1:4]
report = sys.argv[sys.argv.index("--report") + 1] if "--report" in sys.argv else None
BASE = 0x82000000
CODE = [(0x82150000, 0x8CE084), (0x82A1E200, 0x16990), (0x82A34C00, 0x17F54), (0x82A4CC00, 0x6ECC),
        (0x82A53C00, 0x1ED8), (0x82A55C00, 0x16E60), (0x82A6CC00, 0x1300), (0x82A6E000, 0x2D4),
        (0x82A6E400, 0x2D50)]

# LLVM reference: address -> mnemonic
ref = {}
for line in open(dis_path):
    m = re.match(r"([0-9a-f]{8}):\s+(\S+)", line)
    if m:
        ref[int(m.group(1), 16)] = m.group(2)

# ReXGlue and LLVM spell many instructions differently (extended mnemonics such as `beq`/`bt`,
# `rlwinm`/`slwi`, `subf`/`sub`, `li`/`addi`). LLVM also decodes the Xbox-specific VMX128 encodings as
# newer Power ISA instructions or `<unknown>`. Both sides are normalised to a coarse family so that only
# real address drift is reported.
ROT = {"rlwinm", "rlwimi", "rlwnm", "rotlwi", "rotlw", "slwi", "srwi", "clrlwi", "clrrwi", "extrwi",
       "extlwi", "inslwi", "insrwi", "rldicr", "rldicl", "rldic", "rldimi", "sldi", "srdi", "clrldi",
       "clrrdi", "rotldi", "rotld", "rldcl", "rldcr", "clrlsldi", "extrdi", "extldi", "insrdi"}
ALIASES = {"li": "addi", "lis": "addis", "mr": "or", "not": "nor", "nop": "ori", "subf": "sub",
           "subfc": "subc", "vmr": "vor", "vnot": "vnor", "mtctr": "mtspr", "mfctr": "mfspr",
           "mtlr": "mtspr", "mflr": "mfspr", "mtxer": "mtspr", "mfxer": "mfspr", "mftb": "mfspr",
           "cmpwi": "cmpw", "cmplwi": "cmplw", "cmpdi": "cmpd", "cmpldi": "cmpld", "subi": "addi",
           "subis": "addis", "subic": "addic", "la": "addi", "crclr": "crxor", "crset": "creqv",
           "crmove": "cror", "crnot": "crnor", "mtcr": "mtcrf",
           "db16cyc": "or", "cctpl": "or", "cctpm": "or", "cctph": "or"}
def family(m):
    m = m.lower().rstrip(".+-")
    if m.startswith("b"):
        return "branch"
    if m.startswith("tw") or m.startswith("td"):
        return "trap"
    if m in ROT:
        return "rot"
    return ALIASES.get(m, m)
def vmx128ish(m):
    return m.endswith("128") or m in ("lvlx", "lvrx", "stvlx", "stvrx", "lvlxl", "lvrxl", "stvlxl",
                                      "stvrxl", "lvewx", "stvewx", "lvsl", "lvsr")
def same(gen, llvm):
    if llvm == "<unknown>" or vmx128ish(gen):
        return True
    return family(gen) == family(llvm)

# Function addresses come from ReXGlue's own mapping table, so named helpers (e.g. __restvmx_64) and
# config-named functions are placed correctly too.
func_addr = {}
for m in re.finditer(r"\{ 0x([0-9A-Fa-f]{8}), (\w+) \}", open(glob.glob(os.path.join(gen_dir, "*_init.cpp"))[0]).read()):
    func_addr[m.group(2)] = int(m.group(1), 16)
func_re = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
covered = {}
mismatch = []
funcs = 0
discontinuities = 0
for path in sorted(glob.glob(os.path.join(gen_dir, "*_recomp.*.cpp"))):
    addr = None
    name = None
    for line in open(path):
        m = func_re.match(line)
        if m:
            funcs += 1
            name = line.strip()
            addr = func_addr.get(m.group(1))
            continue
        if addr is None and name is None:
            continue
        lm = re.match(r"^loc_([0-9A-F]{8}):", line)
        if lm:
            a = int(lm.group(1), 16)
            if addr is not None and a != addr:
                discontinuities += 1
            addr = a
            continue
        cm = re.match(r"^\t// (\S+)", line)
        if cm and addr is not None:
            mn = cm.group(1)
            r = ref.get(addr)
            if r is not None and not same(mn, r):
                mismatch.append((addr, mn, r, name))
            covered[addr] = name
            addr += 4

img = open(img_path, "rb").read()
uncovered = []
for start, size in CODE:
    for a in range(start, start + size, 4):
        w = struct.unpack_from(">I", img, a - BASE)[0]
        if w == 0 or a in covered:
            continue
        uncovered.append((a, w, ref.get(a, "?")))

# group uncovered words into runs
runs = []
for a, w, mn in uncovered:
    if runs and runs[-1][1] == a:
        runs[-1][1] = a + 4
        runs[-1][2].append(mn)
    else:
        runs.append([a, a + 4, [mn]])

total = sum(s // 4 for _, s in CODE)
nonzero = sum(1 for s0, s in CODE for a in range(s0, s0 + s, 4) if struct.unpack_from(">I", img, a - BASE)[0])
out = []
out.append(f"generated functions:        {funcs}")
out.append(f"instructions emitted:       {len(covered)}")
out.append(f"code words total/nonzero:   {total}/{nonzero}")
out.append(f"non-zero words uncovered:   {len(uncovered)} in {len(runs)} runs")
out.append(f"label discontinuities:      {discontinuities}")
out.append(f"mnemonic mismatches:        {len(mismatch)}")
kinds = collections.Counter()
for a, b, mns in runs:
    valid = [m for m in mns if m not in ("<unknown>", "?")]
    kinds["all-invalid (data)" if not valid else "contains valid insns"] += 1
out.append(f"uncovered run kinds:        {dict(kinds)}")
bctr_unc = [a for a, w, mn in uncovered if mn in ("bctr", "bctrl")]
out.append(f"uncovered bctr/bctrl:       {len(bctr_unc)}")
out.append("")
out.append("first mismatches:")
out += [f"  {a:#x} gen={g} llvm={r} in {n}" for a, g, r, n in mismatch[:20]]
out.append("")
out.append("uncovered runs with valid instructions (addr end count first-mnemonics):")
for a, b, mns in runs:
    valid = [m for m in mns if m not in ("<unknown>", "?")]
    if valid:
        out.append(f"  {a:#x} {b:#x} {len(mns):5d} {' '.join(mns[:8])}")
text = "\n".join(out)
if report:
    open(report, "w").write(text + "\n")
print("\n".join(out[:12]))
sys.exit(1 if mismatch else 0)
