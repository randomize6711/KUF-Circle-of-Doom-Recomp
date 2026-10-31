#!/usr/bin/env python3
"""Which instructions does the recompiled game use, and which have a semantic test?

Usage: insn_test_coverage.py <generated_dir> <asm_dir> [<asm_dir> ...] [--report out.txt]

Mnemonics come from the per-instruction comments in the generated code. Test coverage comes from the
instruction mnemonics that appear in the test assembly files (instr_*.s), so a test written for `lvlx` in
instr_lvl.s counts for `lvlx`. Extended mnemonics are folded to the base instruction they encode.
Plain loads/stores and branches are reported separately: every test exercises them implicitly.
"""
import collections, glob, os, re, sys

args = [a for a in sys.argv[1:]]
report = None
if "--report" in args:
    i = args.index("--report")
    report = args[i + 1]
    del args[i:i + 2]
gen_dir, asm_dirs = args[0], args[1:]

FOLD = {
    "li": "addi", "lis": "addis", "subi": "addi", "subis": "addis", "la": "addi", "mr": "or", "not": "nor",
    "nop": "ori", "cmpwi": "cmpi", "cmpdi": "cmpi", "cmplwi": "cmpli", "cmpldi": "cmpli", "cmpw": "cmp",
    "cmpd": "cmp", "cmplw": "cmpl", "cmpld": "cmpl", "subic": "addic", "sub": "subf", "subc": "subfc",
    "vmr": "vor", "vnot": "vnor", "vmr128": "vor128",
}
for m in ("clrlwi", "clrrwi", "slwi", "srwi", "rotlwi", "rotrwi", "extlwi", "extrwi", "clrlslwi"):
    FOLD[m] = "rlwinm"
for m in ("inslwi", "insrwi"):
    FOLD[m] = "rlwimi"
for m in ("rotlw",):
    FOLD[m] = "rlwnm"
for m in ("sldi",):
    FOLD[m] = "rldicr"
for m in ("srdi", "clrldi", "rotldi", "extrdi"):
    FOLD[m] = "rldicl"


def base(m):
    m = m.lower().rstrip(".")
    return FOLD.get(m, m)


def implicit(m):
    if re.fullmatch(r"(l|st)(b|h|w|d|ha|wa|f[sd]|mw)z?(u)?x?|lwbrx|stwbrx|lhbrx|sthbrx|ldbrx|stdbrx|lwarx|"
                    r"ldarx|stwcx|stdcx|lvx128|stvx128|lvx|stvx|lvxl|stvxl|lvxl128|stvxl128", m):
        return "load/store"
    if m.startswith("b") or m in ("mflr", "mtlr", "mfctr", "mtctr", "mfcr", "mtcrf", "mfmsr", "mtmsrd",
                                   "mftb", "sync", "lwsync", "eieio", "isync", "dcbt", "dcbtst", "dcbf",
                                   "dcbst", "dcbz", "dcbz128", "db16cyc", "cctpl", "cctpm", "mfspr", "mtspr"):
        return "control/system"
    return None


used = collections.Counter()
for path in glob.glob(os.path.join(gen_dir, "*_recomp.*.cpp")):
    for m in re.finditer(r"^\t// ([a-z0-9_.]+)", open(path).read(), re.M):
        used[base(m.group(1))] += 1

tested = set()
for d in asm_dirs:
    for path in glob.glob(os.path.join(d, "*.s")):
        # Some tests encode the instruction as `.long 0x...` (assembler lacks the mnemonic); the file
        # name (instr_<mnemonic>.s) names what they test.
        stem = os.path.basename(path)[:-2]
        if stem.startswith("instr_"):
            tested.add(base(stem[len("instr_"):]))
        for line in open(path):
            note = re.search(r"#\s*tests:\s*(\S+)", line)
            if note:
                tested.add(base(note.group(1)))  # raw `.long` encodings name the instruction this way
            line = line.split("#")[0].strip()
            if line and not line.endswith(":"):
                tested.add(base(line.split()[0]))

rows = []
groups = collections.Counter()
for m, c in used.most_common():
    k = "tested" if m in tested else (implicit(m) or "UNTESTED")
    groups[k] += c
    rows.append((m, c, k))
total = sum(used.values())
out = [f"distinct instructions used: {len(used)}; executed-site total: {total}"]
for k in ("tested", "load/store", "control/system", "UNTESTED"):
    n = sum(1 for _, _, kk in rows if kk == k)
    out.append(f"  {k:15s} {n:4d} distinct  {groups[k]:8d} sites ({100.0 * groups[k] / total:5.1f}%)")
out.append("")
out.append("untested instructions by use count:")
out += [f"  {m:14s} {c:7d}" for m, c, k in rows if k == "UNTESTED"]
text = "\n".join(out)
if report:
    open(report, "w").write(text + "\n")
print("\n".join(out[:6]))
print("top untested:", ", ".join([f"{m}({c})" for m, c, k in rows if k == "UNTESTED"][:30]))
