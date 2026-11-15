#!/usr/bin/env python3
"""M18C: a damaged copy of a shader/pipeline cache for stability.damaged_shader_cache.

  damage_shader_cache.py SRC_CACHE OUT_DIR

SRC_CACHE is a cache directory (shaders/shareable/<title>.xsh and .vk.xpso; only read). OUT_DIR is made anew with the
shader storage cut to half its size and the pipeline storage cut in the middle of a record, as a crash while writing
or a partly deleted cache leaves them: the game must load what is intact and create the rest again on first use.
"""
import os
import shutil
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    shareable = os.path.join(src, "shaders", "shareable")
    files = sorted(os.listdir(shareable))
    if not any(f.endswith(".xsh") for f in files) or not any(f.endswith(".xpso") for f in files):
        sys.exit(f"{shareable}: no .xsh and .xpso to damage")
    shutil.rmtree(out, ignore_errors=True)
    dest = os.path.join(out, "shaders", "shareable")
    os.makedirs(dest)
    for f in files:
        path = os.path.join(dest, f)
        shutil.copyfile(os.path.join(shareable, f), path)
        size = os.path.getsize(path)
        cut = size // 2 + (7 if f.endswith(".xsh") else 13)  # not on a record boundary
        with open(path, "r+b") as fh:
            fh.truncate(cut)
        print(f"{f}: {size} -> {cut} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
