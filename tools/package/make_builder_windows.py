#!/usr/bin/env python3
"""Writes a Windows package's builder/ (M16, docs/WINDOWS.md; the Linux one: make_builder.py): what kuf-localbuild.exe
needs to make kuf_cod.exe on the player's Windows PC, and nothing of the game. Taken from the llvm-mingw build tree
(preset mingw-amd64-relwithdebinfo), by its own records:

  recipe.txt     the compile and link commands of the verified build, this tree's paths replaced; "compiler" names
                 llvm-mingw's clang-23.exe, which finds ld.lld.exe beside it
  toolchain/     llvm-mingw's Windows build, only: the compiler and linker with their DLLs, the headers the build read
                 (MinGW-w64, libc++, clang's own), the link inputs it read (CRT objects, import libraries, builtins)
  tree/          the SDK's and the project's headers the generated code includes
  lib/           the project's own code (libkuf_own.a), the SDK's static and import libraries, prebuilt
  config/, kuf_cod_manifest.toml   the code generator's inputs

The llvm-mingw used here (Linux host) and the one shipped (Windows host) are the same release; every target file the
build reads is checked byte for byte between them. On a Windows host both are the same folder; the build's commands
there (cmd.exe wrappers, backslashes, quoted paths with spaces, the link's objects in a response file: relink with
`ninja -d keeprsp` first) are read as forward-slash paths.

Usage: make_builder_windows.py --build BUILD_DIR --out PACKAGE_DIR --llvm-mingw-host DIR --llvm-mingw-windows DIR
"""
import argparse
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

WINDOWS = os.name == "nt"


def fwd(path):
    """A path with forward slashes (Windows host; unchanged elsewhere)."""
    return path.replace("\\", "/") if WINDOWS else path


ROOT = fwd(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
# What the llvm-mingw wrapper (x86_64-w64-mingw32-clang++ and its .cfg files) adds, said explicitly: the recipe calls
# clang-23.exe itself, with no configuration files.
DRIVER = ["--no-default-config", "--driver-mode=g++", "-target", "x86_64-w64-mingw32", "-rtlib=compiler-rt",
          "-unwindlib=libunwind", "-stdlib=libc++", "-fuse-ld=lld"]
COMPILER_FILES = ["bin/clang-23.exe", "bin/ld.lld.exe", "bin/libLLVM-23.dll", "bin/libclang-cpp.dll",
                  "bin/libc++.dll", "bin/libunwind.dll"]


def die(message):
    sys.exit("make_builder_windows: " + message)


def run(*argv, cwd=None):
    return subprocess.run(argv, cwd=cwd, check=True, capture_output=True, text=True).stdout


class Paths:
    def __init__(self, build, host_tc):
        self.build, self.host_tc = build, host_tc
        self.map = sorted([
            (ROOT + "/generated/default", "@W@/generated/default"),
            (host_tc, "@B@/toolchain"),
            (build, "@B@/tree/build"),
            (ROOT, "@B@/tree"),
        ], key=lambda kv: -len(kv[0]))

    def rewrite(self, text):
        for prefix, to in self.map:
            text = text.replace(prefix, to)
        if ROOT in text or "/home/" in text:
            die("a path of this machine is left in the recipe: " + text)
        return text


def posix_line(line):
    """A Windows command line with forward slashes (its escaped quotes kept)."""
    if not WINDOWS:
        return line
    return line.replace('\\"', "\0").replace("\\", "/").replace("\0", '\\"')


def split_command(line, build):
    parts = posix_line(line).split(" && ")
    line = next(p for p in parts if "clang++" in p)
    argv = shlex.split(line[2:] if line.startswith(": ") else line)
    out = []
    for a in argv:  # a response file (the Windows link): its arguments in its place
        if a.startswith("@") and os.path.isfile(os.path.join(build, a[1:])):
            out += shlex.split(posix_line(open(os.path.join(build, a[1:])).read()))
        elif a.startswith("@"):
            die("the response file %s is gone: relink with ninja -d keeprsp" % a)
        else:
            out.append(a)
    return out


def all_args(line):
    """Every argument of a command line, through its && parts (for the -o and -c values)."""
    out = []
    for part in posix_line(line).split(" && "):
        try:
            out += shlex.split(part)
        except ValueError:  # a part cut inside a cmd.exe /C "..." quote
            out += part.split()
    return out


def value_of(argv, flag):
    return next((argv[i + 1] for i in range(len(argv) - 1) if argv[i] == flag), "")


def compile_flags(command, paths):
    flags, i = [], 1
    while i < len(command):
        a = command[i]
        if a in ("-MT", "-MF", "-o", "-c"):
            i += 2
            continue
        if a == "-Xclang" and command[i + 1] in ("-include-pch", "-include"):
            i += 4
            continue
        if a in ("-MD", "-g", "-gline-tables-only", "-Winvalid-pch") or a.startswith("-fmacro-prefix-map="):
            i += 1
            continue
        flags.append(paths.rewrite(a))
        i += 1
    return DRIVER + flags + ["-g0", "-w", "-fmacro-prefix-map=@W@/=", "-fmacro-prefix-map=@B@/tree/=",
                             "-fmacro-prefix-map=@B@/="]


def windows_rel(rel):
    """A file's place in the Windows llvm-mingw: the Linux host keeps the target headers in generic-w64-mingw32/include
    (x86_64-w64-mingw32/include links there), the Windows one in include/."""
    for prefix in ("x86_64-w64-mingw32/include/", "generic-w64-mingw32/include/"):
        if rel.startswith(prefix):
            return "include/" + rel[len(prefix):]
    return rel


def copy_file(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(os.path.realpath(src), dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--llvm-mingw-host", required=True, help="the llvm-mingw the build used (Linux host)")
    ap.add_argument("--llvm-mingw-windows", required=True, help="the same release built for Windows (shipped)")
    a = ap.parse_args()
    build = fwd(os.path.realpath(a.build))
    host_tc, win_tc = fwd(os.path.realpath(a.llvm_mingw_host)), fwd(os.path.realpath(a.llvm_mingw_windows))
    out = os.path.join(a.out, "builder")
    if os.path.exists(out):
        die(out + " exists")
    paths = Paths(build, host_tc)
    objcopy, nm, ar = (os.path.join(host_tc, "bin", t) for t in ("llvm-objcopy", "llvm-nm", "llvm-ar"))

    commands = run("ninja", "-C", build, "-t", "commands", "kuf_cod").splitlines()
    args = [all_args(c) for c in commands]
    tu = next((c for c, v in zip(commands, args) if value_of(v, "-c").endswith("generated/default/kuf_cod_recomp.0.cpp")),
              None)
    link = next((c for c, v in zip(commands, args) if value_of(v, "-o") == "bin/kuf_cod.exe"), None)
    if not tu or not link:
        die("the build has no commands for the generated code or the link")
    flags = compile_flags(split_command(tu, build), paths)
    link_command = split_command(link, build)

    def place(h):
        """Where a build input goes in builder/ (None: regenerated on the player's PC)."""
        for prefix, to in paths.map:
            if h.startswith(prefix + "/"):
                rel = fwd(os.path.relpath(h, prefix))
                if to.startswith("@W@") or rel.startswith("CMakeFiles/") or rel.startswith("build/CMakeFiles/"):
                    return None
                return to[len("@B@/"):], rel, prefix
        die("a build input is outside the known folders: " + h)

    # Headers: what ninja recorded for every generated object and the precompiled header.
    outputs = [value_of(v, "-o") for v in args]
    objects = [o for o in outputs if re.fullmatch(r"CMakeFiles/kuf_cod_recomp\.dir/\S+\.obj", o)]
    if len(objects) < 100:
        die("expected the generated objects; found %d" % len(objects))
    pch = [o for o in outputs if re.fullmatch(r"CMakeFiles/kuf_cod_recomp\.dir/\S+\.pch", o)]
    headers = set()
    for o in objects + pch:
        for line in run("ninja", "-C", build, "-t", "deps", o).splitlines():
            line = line.strip()
            if line and not line.endswith(":") and "#" not in line:
                headers.add(fwd(os.path.normpath(os.path.join(build, line))))
    copied = regenerated = 0
    for h in sorted(headers):
        if h.endswith((".pch", ".obj", ".cpp")):
            continue
        where = place(h)
        if where is None:
            regenerated += 1
            continue
        dest, rel, prefix = where
        if prefix == host_tc:
            wrel = windows_rel(rel)
            shipped = os.path.join(win_tc, wrel)
            if not os.path.isfile(shipped) or open(shipped, "rb").read() != open(h, "rb").read():
                die("the Windows llvm-mingw differs from the host's: " + rel)
            copy_file(shipped, os.path.join(out, dest, wrel))
        else:
            copy_file(h, os.path.join(out, dest, rel))
        copied += 1

    # Link inputs: the verified link run once, verbose; every file it names from llvm-mingw.
    with tempfile.TemporaryDirectory(dir=build) as tmp:
        argv, i = [], 1
        while i < len(link_command):
            x = link_command[i]
            if x == "-o":
                argv += ["-o", os.path.join(tmp, "kuf_cod.exe")]
                i += 2
                continue
            if x.startswith("-Wl,--out-implib"):
                i += 1
                continue
            argv.append(x)
            i += 1
        verbose = subprocess.run([link_command[0]] + argv + ["-Wl,--verbose"], cwd=build, check=True,
                                 capture_output=True, text=True).stderr
    inputs = set(re.findall(re.escape(host_tc) + r"/[^\s]+\.(?:a|o)\b", fwd(verbose)))
    if len(inputs) < 10:
        die("the link named only %d llvm-mingw inputs" % len(inputs))
    for p in sorted(inputs):
        rel = fwd(os.path.relpath(p, host_tc))
        shipped = os.path.join(win_tc, rel)
        if not os.path.isfile(shipped) or open(shipped, "rb").read() != open(p, "rb").read():
            die("the Windows llvm-mingw differs from the host's: " + rel)
        copy_file(shipped, os.path.join(out, "toolchain", rel))

    # The compiler and linker (Windows programs) with the DLLs they need.
    for rel in COMPILER_FILES:
        copy_file(os.path.join(win_tc, rel), os.path.join(out, "toolchain", rel))
    lic = os.path.join(win_tc, "LICENSE.TXT")
    if os.path.isfile(lic):
        copy_file(lic, os.path.join(out, "toolchain", "LICENSE.TXT"))

    # The project's own prebuilt code in one archive (checked: no translated function), the SDK's libraries.
    hooks = set()
    for base, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if f.endswith((".cpp", ".h")):
                text = open(os.path.join(base, f), errors="replace").read()
                hooks.update("sub_" + x.upper() for x in re.findall(r"\b(?:0x|sub_)?(8[0-9A-Fa-f]{7})\b", text))
    os.makedirs(os.path.join(out, "lib"), exist_ok=True)
    own = [x for x in link_command if re.fullmatch(r"CMakeFiles/kuf_cod\.dir/\S+\.obj", x)]
    if len(own) < 20:
        die("expected the project's own objects; found %d" % len(own))
    with tempfile.TemporaryDirectory(dir=build) as tmp:
        stripped = []
        for n, o in enumerate(own):
            s = os.path.join(tmp, "%03d-%s" % (n, os.path.basename(o)))
            subprocess.check_call([objcopy, "--strip-debug", os.path.join(build, o), s])
            for line in run(nm, "--defined-only", s).splitlines():
                m = re.search(r" [TtDdRrBbWw] (sub_8[0-9A-Fa-f]{7})$", line)
                if m and "sub_" + m.group(1)[4:].upper() not in hooks:
                    die("translated code in the project's own object %s: %s" % (o, line))
            stripped.append(s)
        subprocess.check_call([ar, "rcsD", os.path.join(out, "lib", "libkuf_own.a")] + stripped)
    libraries = ["-Wl,--whole-archive", "@B@/lib/libkuf_own.a", "-Wl,--no-whole-archive"]
    # The compiled resources (cmake/windows/utf8.manifest: the UTF-8 code page) linked as they are.
    for res in [x for x in link_command if re.fullmatch(r"CMakeFiles/kuf_cod\.dir/\S+\.res", x)]:
        shutil.copyfile(os.path.join(build, res), os.path.join(out, "lib", os.path.basename(res)))
        libraries.append("@B@/lib/" + os.path.basename(res))
    for item in link_command[1:]:
        if re.fullmatch(r"(?:sdk-lib/)?lib[\w+-]+\.(?:dll\.)?a", item):
            dst = os.path.join(out, "lib", os.path.basename(item))
            if not os.path.exists(dst):
                if item.endswith(".dll.a"):  # an import library: names only, nothing to strip
                    shutil.copyfile(os.path.join(build, item), dst)
                else:
                    subprocess.check_call([objcopy, "--strip-debug", os.path.join(build, item), dst])
            libraries.append("@B@/lib/" + os.path.basename(item))
        elif item.startswith("-l"):
            libraries.append(item)
    links = list(DRIVER)
    i = 1
    while i < len(link_command):
        x = link_command[i]
        if x == "-o":
            i += 2
            continue
        if x.endswith((".obj", ".a", ".res")) or x.startswith(("-l", "-Wl,--out-implib")) or x in ("-g",):
            i += 1
            continue
        links.append(paths.rewrite(x))
        i += 1

    # The code generator's inputs.
    manifest = open(os.path.join(ROOT, "kuf_cod_manifest.toml")).read()
    shutil.copyfile(os.path.join(ROOT, "kuf_cod_manifest.toml"), os.path.join(out, "kuf_cod_manifest.toml"))
    for inc in re.findall(r'"(config/[^"]+\.toml)"', manifest):
        copy_file(os.path.join(ROOT, inc), os.path.join(out, inc))

    # The id of everything the generated objects depend on (not the project's own code, which is only linked).
    h = hashlib.sha256()
    h.update("\n".join(flags).encode())
    for sub in ("toolchain", "tree", "config"):
        for base, dirs, files in sorted(os.walk(os.path.join(out, sub))):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(base, f)
                h.update(os.path.relpath(p, out).replace(os.sep, "/").encode())
                h.update(open(p, "rb").read())
    for sub in ("src/rexglue", "src/codegen"):
        d = os.path.join(ROOT, "thirdparty/rexglue-sdk", sub)
        for base, dirs, files in sorted(os.walk(d)):
            dirs.sort()
            for f in sorted(files):
                h.update(open(os.path.join(base, f), "rb").read())
    sources = sum(1 for v in args if re.search(r"generated/default/[^/]+\.cpp$", value_of(v, "-c")))
    with open(os.path.join(out, "recipe.txt"), "w", newline="\n") as f:
        f.write("# The verified build's commands (tools/package/make_builder_windows.py).\n")
        f.write("generation_id " + h.hexdigest() + "\n")
        f.write("platform windows-x86_64\n")
        f.write("sources %d\n" % sources)
        f.write("compiler @B@/toolchain/bin/clang-23.exe\n")
        f.write("compile " + " ".join(flags) + "\n")
        f.write("link " + " ".join(links) + "\n")
        f.write("link_libraries " + " ".join(libraries) + "\n")
    print("builder (windows): %d headers copied, %d generated headers left out, %d link inputs, %d sources, id %s" %
          (copied, regenerated, len(inputs), sources, h.hexdigest()[:16]))


if __name__ == "__main__":
    main()
