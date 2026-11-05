#!/usr/bin/env python3
"""Build the in-memory PE image from a basic-compressed or uncompressed, unencrypted XEX2 and resolve import ordinals.
Usage: xeximage.py <default.xex> <out_dir>"""
import struct, sys, os
u32 = lambda b, o: struct.unpack_from(">I", b, o)[0]
d = open(sys.argv[1], "rb").read(); out = sys.argv[2]
_, pe_off, _, sec, n = struct.unpack_from(">5I", d, 4)
h = dict(struct.unpack_from(">II", d, 24 + i*8) for i in range(n))
ff = h[0x3FF]; ffsz = u32(d, ff)
enc, comp = struct.unpack_from(">HH", d, ff + 4)
if enc != 0: sys.exit("this XEX is encrypted; only an unencrypted XEX is supported (docs/LOCAL_BUILD.md)")
if comp not in (0, 1): sys.exit("unsupported XEX compression %d" % comp)
raw = d[pe_off:]
img = bytearray(); src = 0
if comp == 0: img += raw
else:
  for o in range(ff + 8, ff + ffsz, 8):
    ds, zs = struct.unpack_from(">II", d, o)
    img += raw[src:src+ds] + b"\0"*zs; src += ds
size = u32(d, sec + 4); img = img[:size] + b"\0"*max(0, size-len(img))
open(os.path.join(out, "image.bin"), "wb").write(img)
print("image", hex(len(img)))
# PE sections (little-endian PE header)
pe = struct.unpack_from("<I", img, 0x3C)[0]
nsec = struct.unpack_from("<H", img, pe+6)[0]; opt = struct.unpack_from("<H", img, pe+20)[0]
for i in range(nsec):
    o = pe + 24 + opt + i*40
    nm = img[o:o+8].rstrip(b"\0").decode(); vs, va, rs, ro = struct.unpack_from("<4I", img, o+8)
    ch = struct.unpack_from("<I", img, o+36)[0]
    print(f"  PE section {nm:8s} va={0x82000000+va:#010x} vsize={vs:#9x} flags={ch:#x}")
# imports
ib = h[0x103FF]; b = d[ib:ib+u32(d, ib)]
strsz = u32(b, 4); names = [x.decode() for x in b[12:12+strsz].split(b"\0") if x]
o = 12 + strsz
with open(os.path.join(out, "imports.txt"), "w") as f:
    while o < len(b):
        lsz = u32(b, o); ni, cnt = struct.unpack_from(">HH", b, o+36)
        for j in range(cnt):
            a = u32(b, o+40+j*4); v = u32(img, a - 0x82000000)
            typ, ordn = v >> 24, v & 0xFFFF
            if typ == 0: f.write(f"{names[ni]} {ordn} var {a:#x}\n")
            elif typ == 1: f.write(f"{names[ni]} {ordn} func {a:#x}\n")
        o += lsz
