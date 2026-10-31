#!/usr/bin/env python3
"""Fail if Git tracks (or would commit) anything that looks like game-derived content.

Checks every path in the index plus the given revision (default HEAD): no game archive or media formats, no
images, audio, saves or disassembly, nothing under build/, generated/default/ or other ignored content
directories, and no large binary blobs. Usage: python3 -I tools/check_tracked.py [rev]
Exit 0 = clean; 1 = something suspicious (listed).
"""
import subprocess
import sys

FORBIDDEN_SUFFIXES = (
    ".xex", ".xexp", ".pak", ".sox", ".ccz", ".xwb", ".xsb", ".xgs", ".wmv", ".36t", ".bm", ".bvl",
    ".chunk", ".dds", ".bin", ".elf", ".dis", ".png", ".jpg", ".jpeg", ".bmp", ".ppm", ".tga", ".gif",
    ".wav", ".xma", ".ogg", ".raw", ".pcm", ".sav", ".dat", ".stfs", ".xcp",
)
FORBIDDEN_PREFIXES = ("build/", "out/", "generated/default/", "assets/", "extracted/", "game/")
MAX_BINARY_BYTES = 64 * 1024


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True).stdout


def main():
    rev = sys.argv[1] if len(sys.argv) > 1 else "HEAD"
    paths = set(git("ls-files", "-z").decode().split("\0")) - {""}
    sizes = {}
    for line in git("ls-tree", "-r", "-l", "-z", rev).decode().split("\0"):
        if not line:
            continue
        meta, path = line.split("\t", 1)
        mode, kind, obj, size = meta.split()
        paths.add(path)
        if kind == "blob":
            sizes[path] = (obj, int(size))
    bad = []
    for path in sorted(paths):
        low = path.lower()
        if low.endswith(FORBIDDEN_SUFFIXES):
            bad.append(f"{path}: game content or media format")
        elif low.startswith(FORBIDDEN_PREFIXES):
            bad.append(f"{path}: ignored content directory")
        elif path in sizes and sizes[path][1] > MAX_BINARY_BYTES:
            blob = git("cat-file", "blob", sizes[path][0])
            if b"\0" in blob[:8192]:
                bad.append(f"{path}: binary blob of {sizes[path][1]} bytes")
    for b in bad:
        print("SUSPICIOUS", b)
    print(f"checked {len(paths)} tracked paths at {rev}: {'clean' if not bad else f'{len(bad)} suspicious'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
