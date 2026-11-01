#!/usr/bin/env python3
"""Derive a "load into a storage image" variant of a ReXGlue texture load compute shader (KUF port M9).

  python3 -I tools/spirv_load_to_image.py <texture_load_*_cs.h> <out.spvasm> [--bits 64|32]

The SDK's texture load shaders read the guest (here: resolution-scaled resolve) data, untile and endian-swap it and
store 16-byte vectors into a linear staging buffer (set 0, binding 0), which the texture cache then copies into the
image with vkCmdCopyBufferToImage. The variant keeps every instruction that reads and converts the data and replaces
each store into the staging buffer with image writes of the same texels into a storage image (set 0, binding 0),
so the copy and the staging traffic are not needed. Supported: shaders storing two 16-byte vectors per invocation
for consecutive texels of one row (the 64 bpb, 32 bpb and depth float loads): --bits 64 (two texels per vector,
written to an Rgba16ui image: the same bits as the staging buffer's) or --bits 32 (four texels per vector, R32ui).
Both image formats are core storage formats (no StorageImageExtendedFormats). Only for one 2D slice of mip level 0
(the texture cache uses it only then). A texel past the level's width is not written (the buffer row's padding).

Reads the disassembly the SDK keeps in the header's "#if 0" block, so the result follows the shipped shader.
"""
import re
import sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    bits = int(sys.argv[sys.argv.index("--bits") + 1]) if "--bits" in sys.argv else 64
    per_vector = 128 // bits
    image_format = {64: "Rgba16ui", 32: "R32ui"}[bits]
    text = open(src).read()
    asm = text.split("#if 0\n", 1)[1].split("#endif", 1)[0]
    lines = asm.splitlines()
    # The staging buffer variable: decorated DescriptorSet 0, NonReadable.
    dest = None
    for l in lines:
        m = re.match(r"\s*OpDecorate (%\w+) DescriptorSet 0$", l)
        if m:
            dest = m.group(1)
    if not dest:
        raise SystemExit("no set-0 destination found")
    stores = [i for i, l in enumerate(lines) if re.match(rf"\s*%\w+ = OpAccessChain %_ptr_Uniform_v4uint {dest} ", l)]
    if len(stores) != 2:
        raise SystemExit(f"expected 2 stores to {dest}, found {len(stores)}")
    # The invocation's block coordinates: gid << (2, 0, 0) (4 blocks per invocation along x).
    shifted = next(re.match(r"\s*(%\w+) = OpShiftLeftLogical %v3uint (%\w+) %\w+", l).group(1) for l in lines
                   if re.match(r"\s*%\w+ = OpShiftLeftLogical %v3uint %\w+ %\w+", l))
    size = next(re.search(r"(%\w+) = OpLoad %v3uint (%\w+)$", l).group(1) for l in lines
                if re.search(r"= OpLoad %v3uint %\w+$", l) and "GlobalInvocationID" not in l)

    out_lines = []
    n = [0]

    def new(name):
        n[0] += 1
        return f"%kuf_{name}_{n[0]}"

    def texel_writes(vec, first):
        """Image writes of the two texels of `vec` at x0 + first, x0 + first + 1."""
        code = []
        x0 = new("x0")
        y = new("y")
        w = new("w")
        code += [f"{x0} = OpCompositeExtract %uint {shifted} 0", f"{y} = OpCompositeExtract %uint {shifted} 1",
                 f"{w} = OpCompositeExtract %uint {size} 0"]
        for k in range(per_vector):
            x = new("x")
            code.append(f"{x} = OpIAdd %uint {x0} %kuf_uint_{first + k}")
            texel = new("texel")
            if bits == 64:
                a, b = new("a"), new("b")
                code += [f"{a} = OpCompositeExtract %uint {vec} {2 * k}",
                         f"{b} = OpCompositeExtract %uint {vec} {2 * k + 1}"]
                comps = []
                for word in (a, b):
                    lo16, hi16 = new("lo"), new("hi")
                    code += [f"{lo16} = OpBitwiseAnd %uint {word} %kuf_uint_65535",
                             f"{hi16} = OpShiftRightLogical %uint {word} %kuf_uint_16"]
                    comps += [lo16, hi16]
                code.append(f"{texel} = OpCompositeConstruct %v4uint {' '.join(comps)}")
            else:
                a = new("a")
                code += [f"{a} = OpCompositeExtract %uint {vec} {k}",
                         f"{texel} = OpCompositeConstruct %v4uint {a} %kuf_uint_0 %kuf_uint_0 %kuf_uint_0"]
            xi, yi, c, img = new("xi"), new("yi"), new("c"), new("img")
            code += [f"{xi} = OpBitcast %int {x}", f"{yi} = OpBitcast %int {y}",
                     f"{c} = OpCompositeConstruct %v2int {xi} {yi}"]
            inside, then, merge = new("inside"), new("then"), new("merge")
            code += [f"{inside} = OpULessThan %bool {x} {w}", f"OpSelectionMerge {merge} None",
                     f"OpBranchConditional {inside} {then} {merge}", f"{then} = OpLabel",
                     f"{img} = OpLoad %kuf_image {dest_image}", f"OpImageWrite {img} {c} {texel}",
                     f"OpBranch {merge}", f"{merge} = OpLabel"]
        return code

    dest_image = "%kuf_dest_image"
    skip = set()
    for i in stores:
        skip.add(i)
        skip.add(i + 1)  # the OpStore after the access chain
    for i, l in enumerate(lines):
        s = l.strip()
        if re.match(rf"OpDecorate {dest} ", s):
            out_lines.append(l.replace(dest, dest_image))
            continue
        if re.match(rf"{dest} = OpVariable", s):
            out_lines += [f"%kuf_image = OpTypeImage %uint 2D 0 0 0 2 {image_format}",
                          "%kuf_ptr_image = OpTypePointer UniformConstant %kuf_image",
                          f"{dest_image} = OpVariable %kuf_ptr_image UniformConstant",
                          "%kuf_uint_65535 = OpConstant %uint 65535", "%kuf_uint_16 = OpConstant %uint 16"]
            for v in range(2 * per_vector):
                out_lines.append(f"%kuf_uint_{v} = OpConstant %uint {v}")
            continue
        if i in skip:
            if i in stores:
                store = lines[i + 1].strip()
                vec = store.split()[-1]
                out_lines += texel_writes(vec, 0 if i == stores[0] else per_vector)
            continue
        out_lines.append(l)
    open(out, "w").write("\n".join(out_lines) + "\n")


if __name__ == "__main__":
    main()
