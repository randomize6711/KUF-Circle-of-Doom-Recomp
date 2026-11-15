"""Per-function hashes of the code in COFF objects (llvm-objdump -d -r --no-show-raw-insn), the layout removed:
addresses, RIP displacements, the disassembler's comments, and the symbolic label of an instruction whose target is
given by its relocation (the relocation, with the real target, is kept). usage: funchash.py OBJDUMP OUT.tsv OBJ..."""
import hashlib, re, subprocess, sys
objdump, out, objs = sys.argv[1], sys.argv[2], sys.argv[3:]
hashes = {}

def norm(t):
    t = re.sub(r"\s*#.*$", "", t)
    t = re.sub(r"-?0x[0-9a-f]+\(%rip\)", "REL(%rip)", t)
    t = re.sub(r"\b0x[0-9a-f]+\b(?=\s*<)", "", t)
    t = re.sub(r"^\s*[0-9a-f]+:\s*", "", t)
    return t

for o in objs:
    lines = subprocess.run([objdump, "-d", "-r", "--no-show-raw-insn", "--no-leading-addr", o],
                           capture_output=True, text=True).stdout.splitlines()
    name, body = None, []
    def flush():
        if name:
            hashes[name] = hashlib.sha256("\n".join(body).encode()).hexdigest()
    for i, line in enumerate(lines):
        s = line.strip()
        m = re.match(r"^(?:[0-9a-f]+ )?<([^>]+)>:$", s)
        if m:
            flush()
            name, body = m.group(1), []
            continue
        if name is None or not s or s.startswith("Disassembly of section"):
            continue
        t = norm(s)
        nxt = lines[i + 1].strip() if i + 1 < len(lines) else ""
        if nxt.startswith("IMAGE_REL_"):
            t = re.sub(r"\s*<[^>]*>", "", t)
        body.append(t)
    flush()
with open(out, "w") as f:
    for k in sorted(hashes):
        f.write(k + "\t" + hashes[k] + "\n")
print(out, len(hashes), "functions")
