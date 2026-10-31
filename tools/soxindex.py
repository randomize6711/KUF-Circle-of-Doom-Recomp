#!/usr/bin/env python3
"""Read-only index of KUF:COD .sox/.pak archives. Usage: soxindex.py <game_dir> <out_tsv>"""
import sys, struct, os, zlib, glob, collections, random
G, out = sys.argv[1], sys.argv[2]
rows = []
for sox in sorted(glob.glob(os.path.join(G, "*.sox"))):
    d = open(sox, "rb").read()
    ver, ncol, nrow = struct.unpack_from(">III", d, 0)
    cols = [(d[12+i*68:12+i*68+64].split(b"\0")[0].decode(), struct.unpack_from(">I", d, 12+i*68+64)[0]) for i in range(ncol)]
    o = 12 + ncol*68
    while d[o] == 0: o += 1
    o -= 3  # back to start of 1-based row id (u32)
    for r in range(nrow):
        rid = struct.unpack_from(">I", d, o)[0]; o += 4; vals = []
        for name, t in cols:
            if t == 1:
                n = struct.unpack_from(">H", d, o)[0]; vals.append(d[o+2:o+2+n].decode("latin1")); o += 2+n
            else:
                vals.append(struct.unpack_from(">I", d, o)[0]); o += 4
        rows.append((os.path.basename(sox), rid, *vals))
    assert o == len(d), (sox, o, len(d))
with open(out, "w") as f:
    for r in rows: f.write("\t".join(map(str, r)) + "\n")
print("entries", len(rows))
ext = collections.Counter(os.path.splitext(r[2])[1].lower() for r in rows)
size = collections.Counter()
for r in rows: size[os.path.splitext(r[2])[1].lower()] += r[6]
for e, c in ext.most_common(40): print(f"  {e:8s} {c:6d} files {size[e]/1e6:9.1f} MB")
random.seed(1)
ok = 0
for r in random.sample(rows, 40):
    with open(os.path.join(G, os.path.basename(r[3].replace("\\", "/"))), "rb") as f:
        f.seek(r[4]); blob = zlib.decompress(f.read(r[5]))
    ok += len(blob) == r[6]
print("inflate check", ok, "/ 40")
