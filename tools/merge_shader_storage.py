#!/usr/bin/env python3
"""Merge the Vulkan shader and pipeline storage of several runs into one cache directory (benchmark warm caches).

Usage: python3 -I tools/merge_shader_storage.py OUT_CACHE_DIR RUN_OR_CACHE_DIR...

Every run starts from a copy of one cache (tools/run_sandboxed.sh --cache-from) and appends the pipelines it meets
for the first time; a warm cache copied from a single run therefore misses the pipelines other runs happened to meet,
and runs that meet them pay a first-use hitch (a synchronous placeholder pipeline and a skipped presentation) that
varies from run to run. The union of many runs' storage removes most of that noise.

Formats (SDK src/graphics/vulkan/pipeline_cache.cpp, include/rex/graphics/vulkan/pipeline_cache.h):
  <title>.xsh: 'XESH', version (byte-swapped); records: u64 XXH3 of the ucode, u32 dword count (31 bits) + shader
  type (1 bit), ucode dwords.
  <title>.fbo.vk.xpso / .fsi.vk.xpso: 'XEPS', API, version (byte-swapped); fixed-size records: u64 XXH3 of the
  description, description. The record size is not stored: it is found from the inputs (the size that divides every
  file's records), and records are kept byte-for-byte (the game validates the hashes on load).
The output is game-derived data: keep it under build/, never commit it.
"""
import os
import struct
import sys
from collections import OrderedDict


def shareable(path):
    for sub in ("cache/shaders/shareable", "shaders/shareable", "."):
        p = os.path.join(path, sub)
        if os.path.isdir(p) and any(n.endswith((".xsh", ".xpso")) for n in os.listdir(p)):
            return p
    sys.exit(f"no shader storage in {path}")


def read_xsh(path, shaders):
    data = open(path, "rb").read()
    if len(data) < 8 or data[:4] != b"XESH":
        print(f"  skipped {path}: not shader storage", file=sys.stderr)
        return data[:8], 0
    pos, count = 8, 0
    while pos + 12 <= len(data):
        (word,) = struct.unpack_from("<I", data, pos + 8)
        end = pos + 12 + (word & 0x7FFFFFFF) * 4
        if end > len(data):
            break
        shaders.setdefault(data[pos:pos + 8], data[pos:end])
        pos, count = end, count + 1
    return data[:8], count


def merge(out_dir, inputs):
    groups = {}  # file name -> list of paths
    for src in inputs:
        root = shareable(src)
        for name in sorted(os.listdir(root)):
            if name.endswith((".xsh", ".xpso")):
                groups.setdefault(name, []).append(os.path.join(root, name))
    out_root = os.path.join(out_dir, "shaders", "shareable")
    os.makedirs(out_root, exist_ok=True)
    for name, paths in sorted(groups.items()):
        if name.endswith(".xsh"):
            shaders, header = OrderedDict(), None
            for p in paths:
                h, _ = read_xsh(p, shaders)
                if h[:4] == b"XESH" and header and h != header:
                    sys.exit(f"{name}: storage versions differ: not merged")
                header = header or (h if h[:4] == b"XESH" else None)
            if not header:
                sys.exit(f"{name}: no valid shader storage")
            with open(os.path.join(out_root, name), "wb") as f:
                f.write(header + b"".join(shaders.values()))
            print(f"{name}: {len(shaders)} shaders from {len(paths)} files")
            continue
        blobs = [open(p, "rb").read() for p in paths]
        header = blobs[0][:12]
        if header[:4] != b"XEPS" or any(b[:12] != header for b in blobs):
            sys.exit(f"{name}: headers differ (another API or storage version): not merged")
        sizes = [len(b) - 12 for b in blobs if len(b) > 12]
        candidates = [r for r in range(16, 1025) if sizes and all(s % r == 0 for s in sizes)]
        if not candidates:
            sys.exit(f"{name}: no record size divides every file")
        record = _pick_record(blobs, candidates)
        records = OrderedDict()
        for b in blobs:
            for pos in range(12, len(b) - record + 1, record):
                records.setdefault(b[pos:pos + record], None)
        with open(os.path.join(out_root, name), "wb") as f:
            f.write(header + b"".join(records))
        print(f"{name}: {len(records)} pipelines from {len(paths)} files (record {record} bytes)")


def _pick_record(blobs, candidates):
    # Several sizes can divide every file (divisors and multiples of the real one). At the real size every record of a
    # file starts with a distinct 64-bit hash (a run appends only descriptions it did not load); at a divisor the
    # "hash" fields fall inside descriptions, which repeat. Take the smallest size whose leading fields are distinct.
    for r in candidates:
        if all(len({b[pos:pos + 8] for pos in range(12, len(b) - r + 1, r)}) == (len(b) - 12) // r for b in blobs):
            return r
    sys.exit("no record size gives distinct record hashes")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    merge(sys.argv[1], sys.argv[2:])
