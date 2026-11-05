#!/usr/bin/env python3
"""Writes a package's builder/ (docs/LOCAL_BUILD.md): what kuf-localbuild needs to make the game's program on the player's
machine, and nothing of the game. Taken from the portable build tree, by its own records:

  recipe.txt     the compile and link commands of the verified build, with this tree's paths replaced
  toolchain/     the compiler (tools/toolchain/build_local_compiler.sh): one program, plus its built-in headers
  sysroot/       the C library's headers and link inputs, exactly the files the build read (Steam Runtime 3)
  libstdcxx/     the C++ library's headers and link inputs, likewise
  tree/          the SDK's and the project's headers the generated code includes, likewise
  lib/           the project's own code and static libraries, prebuilt
  config/, kuf_cod_manifest.toml   the code generator's inputs

Generated files are never copied: a build input under generated/ is regenerated on the player's machine.

Usage: make_builder.py --build BUILD_DIR --out PACKAGE_DIR --compiler LLVM_DIR [--plain-compiler-script FILE]
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

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def die(message):
    sys.exit("make_builder: " + message)


def run(*argv, cwd=None):
    return subprocess.run(argv, cwd=cwd, check=True, capture_output=True, text=True).stdout


class Paths:
    """This tree's locations and where each goes in builder/ (longest prefix first)."""

    def __init__(self, build, sysroot, libstdcxx, resource):
        self.build, self.sysroot, self.libstdcxx, self.resource = build, sysroot, libstdcxx, resource
        self.map = sorted([
            (os.path.join(ROOT, "generated", "default"), "@W@/generated/default"),
            (sysroot, "@B@/sysroot"),
            (libstdcxx, "@B@/libstdcxx"),
            (resource, "@B@/toolchain/lib/clang/" + os.path.basename(resource)),
            (build, "@B@/tree/build"),
            (ROOT, "@B@/tree"),
        ], key=lambda kv: -len(kv[0]))

    def rewrite(self, text):
        for prefix, to in self.map:
            text = text.replace(prefix, to)
        if ROOT in text or "/home/" in text:
            die("a path of this machine is left in the recipe: " + text)
        return text


def split_command(line):
    line = line.split(" && ")
    line = next(part for part in line if "clang++" in part)
    return shlex.split(line.replace(": ", "", 1) if line.startswith(": ") else line)


def compile_flags(command, paths):
    flags, i = [], 1
    skip_value = {"-MT", "-MF", "-o", "-c"}
    while i < len(command):
        a = command[i]
        if a in skip_value:
            i += 2
            continue
        if a == "-Xclang" and command[i + 1] in ("-include-pch", "-include"):
            i += 4  # the option and its value, each after -Xclang
            continue
        if a in ("-MD", "-g", "-gline-tables-only", "-Winvalid-pch") or a.startswith("-fmacro-prefix-map="):
            i += 1
            continue
        flags.append(paths.rewrite(a))
        i += 1
    return flags + ["-g0", "-w", "-fmacro-prefix-map=@W@/=", "-fmacro-prefix-map=@B@/tree/=",
                    "-fmacro-prefix-map=@B@/="]


def link_flags(command, paths):
    keep, i = [], 1
    while i < len(command):
        a = command[i]
        if a == "-Xlinker" or a == "-o":
            i += 2
            continue
        if a.endswith(".o") or a.endswith(".a") or a.endswith(".so") or a in ("-g", "-O2", "-DNDEBUG", "-fuse-ld=lld") \
                or a.startswith("-l"):
            i += 1
            continue
        if a == "-isystem":
            i += 2
            continue
        if a == "-nostdinc++":
            i += 1
            continue
        keep.append(paths.rewrite(a).replace("\\$ORIGIN", "$ORIGIN"))
        i += 1
    return keep


def copy_file(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(os.path.realpath(src), dst)
    shutil.copymode(os.path.realpath(src), dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--compiler", required=True, help="the LLVM build directory (bin/llvm, lib/clang/N/include)")
    ap.add_argument("--plain-compiler-script", help="tests of this tool only: a script that stands in for the compiler")
    a = ap.parse_args()
    build = os.path.realpath(a.build)
    out = os.path.join(a.out, "builder")
    if os.path.exists(out):
        die(out + " exists")
    cache = open(os.path.join(build, "CMakeCache.txt")).read()
    sysroot = os.path.realpath(re.search(r"^KUFCOD_SNIPER_SYSROOT:PATH=(.*)$", cache, re.M).group(1))
    libstdcxx = os.path.realpath(re.search(r"^KUFCOD_LIBSTDCXX:PATH=(.*)$", cache, re.M).group(1))
    version_dir = os.path.join(a.compiler, "lib", "clang")
    resource_dirs = os.listdir(version_dir)
    if len(resource_dirs) != 1:
        die("no single clang resource folder in " + version_dir)
    new_resource = os.path.join(version_dir, resource_dirs[0])
    host_resource = run("clang", "-print-resource-dir").strip()
    paths = Paths(build, sysroot, libstdcxx, os.path.realpath(host_resource))

    commands = run("ninja", "-C", build, "-t", "commands", "kuf_cod").splitlines()
    tu = next((c for c in commands if re.search(r"-c \S*generated/default/kuf_cod_recomp\.0\.cpp", c)), None)
    link = next((c for c in commands if re.search(r"-o bin/kuf_cod( |$)", c)), None)
    if not tu or not link:
        die("the build has no commands for the generated code or the link")
    flags = compile_flags(split_command(tu), paths)
    link_command = split_command(link)

    # Headers: what ninja recorded for every generated object and the precompiled header.
    objects = [m for m in re.findall(r"-o (CMakeFiles/kuf_cod_recomp\.dir/\S+\.o)", "\n".join(commands))]
    if len(objects) < 100:
        die("expected the generated objects; found %d" % len(objects))
    headers = set()
    for o in objects + ["CMakeFiles/kuf_cod_recomp.dir/cmake_pch.hxx.pch"]:
        for line in run("ninja", "-C", build, "-t", "deps", o).splitlines():
            line = line.strip()
            if line.startswith("/") or line.startswith("../") or (line and "#" not in line and ":" not in line):
                headers.add(os.path.normpath(os.path.join(build, line)))
    if not headers:
        die("ninja recorded no header dependencies")

    copied = regenerated = 0
    for h in sorted(headers):
        real_prefix = None
        for prefix, to in paths.map:
            if h.startswith(prefix + "/"):
                real_prefix = (prefix, to)
                break
        if real_prefix is None:
            die("a build input is outside the known folders: " + h)
        prefix, to = real_prefix
        rel = os.path.relpath(h, prefix)
        if to.startswith("@W@") or rel.startswith("build/CMakeFiles/") or rel.startswith("CMakeFiles/"):
            regenerated += 1
            continue
        if not os.path.isfile(h):
            die("missing " + h)
        if h.endswith((".pch", ".o", ".cpp")) or "/generated/" in h:
            if not h.endswith((".pch", ".o")):
                die("unexpected input in the header set: " + h)
            continue
        if prefix == paths.resource:
            # The host compiler also has compiler-rt's sanitizer/ headers, which code only probes for (__has_include)
            # and this compiler does not have; the build on the player's machine works without them (tested).
            if rel.startswith("include/sanitizer/") and not os.path.exists(os.path.join(new_resource, rel)):
                continue
            copy_file(os.path.join(new_resource, rel), os.path.join(out, to[len("@B@/"):], rel))
        else:
            copy_file(h, os.path.join(out, to[len("@B@/"):], rel))
        copied += 1

    # Link inputs: the verified link run once, tracing what it reads.
    os.makedirs(os.path.join(out, "lib"))
    with tempfile.TemporaryDirectory(dir=build) as tmp:
        trace = []
        i = 1
        while i < len(link_command):
            if link_command[i] == "-Xlinker":
                i += 2
                continue
            if link_command[i] == "-o":
                trace += ["-o", os.path.join(tmp, "kuf_cod")]
                i += 2
                continue
            trace.append(link_command[i])
            i += 1
        out_trace = subprocess.run([link_command[0]] + trace + ["-Wl,--trace"], cwd=build, check=True,
                                   capture_output=True, text=True).stdout
    for line in out_trace.splitlines():
        p = os.path.normpath(os.path.join(build, line.strip().split("(")[0]))
        if p.startswith(sysroot + "/"):
            copy_file(p, os.path.join(out, "sysroot", os.path.relpath(p, sysroot)))
        elif p.startswith(libstdcxx + "/"):
            copy_file(p, os.path.join(out, "libstdcxx", os.path.relpath(p, libstdcxx)))
    gcc_dir = next(x.split("=", 1)[1] for x in link_command if x.startswith("--gcc-install-dir="))
    for name in ("crtbegin.o", "crtend.o", "crtbeginS.o", "crtendS.o", "libgcc.a", "libgcc_s.so"):
        p = os.path.join(gcc_dir, name)
        if os.path.exists(p):
            copy_file(p, os.path.join(out, "sysroot", os.path.relpath(p, sysroot)))
    # The names the linker asks for (-lm, -lc, ...) are linker scripts or links, which it does not list.
    for name in ("libm.so", "libc.so", "libdl.so", "libpthread.so", "librt.so", "libm-2.31.a", "libmvec.so"):
        p = os.path.join(sysroot, "usr/lib/x86_64-linux-gnu", name)
        if os.path.exists(p):
            copy_file(p, os.path.join(out, "sysroot", os.path.relpath(p, sysroot)))
    # A linker script names files by absolute path inside the sysroot: those too.
    for base, _, files in os.walk(os.path.join(out, "sysroot")):
        for f in files:
            p = os.path.join(base, f)
            if os.path.getsize(p) < 4096 and open(p, "rb").read(4) != b"\x7fELF":
                try:
                    for name in re.findall(r"(/[\w./+-]+\.(?:so(?:\.\d+)*|a))", open(p, errors="strict").read()):
                        src = sysroot + name
                        dst = os.path.join(out, "sysroot", name.lstrip("/"))
                        if os.path.exists(src) and not os.path.exists(dst):
                            copy_file(src, dst)
                except UnicodeDecodeError:
                    pass

    # The project's own prebuilt code, in one archive; the SDK's static libraries.
    hooks = set()  # the game's functions the project replaces, by hand, in its own sources (REX_HOOK*)
    for base, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if f.endswith((".cpp", ".h")):
                hooks.update("sub_" + a.upper() for a in re.findall(r"\b(?:0x|sub_)?(8[0-9A-Fa-f]{7})\b", open(os.path.join(base, f), errors="replace").read()))
    with tempfile.TemporaryDirectory(dir=build) as tmp:
        own = [o for o in re.findall(r"(CMakeFiles/kuf_cod\.dir/\S+\.o)", link)]
        if len(own) < 20:
            die("expected the project's own objects; found %d" % len(own))
        stripped = []
        for n, o in enumerate(own):
            s = os.path.join(tmp, "%03d-%s" % (n, os.path.basename(o)))
            subprocess.check_call(["objcopy", "--strip-debug", os.path.join(build, o), s])
            for line in run("nm", "--defined-only", s).splitlines():
                m = re.search(r" [TtDdRrBbWw] (sub_8[0-9A-Fa-f]{7})$", line)
                if m and "sub_" + m.group(1)[4:].upper() not in hooks:
                    die("translated code in the project's own object %s: %s" % (o, line))
            stripped.append(s)
        subprocess.check_call(["ar", "rcsD", os.path.join(out, "lib", "libkuf_own.a")] + stripped)
    statics = []
    for lib in re.findall(r"(?:^|\s)((?:sdk-lib/)?lib[\w-]+\.a)(?=\s)", link):
        dst = os.path.join(out, "lib", os.path.basename(lib))
        subprocess.check_call(["objcopy", "--strip-debug", os.path.join(build, lib), dst])
        statics.append("@B@/lib/" + os.path.basename(lib))

    # The code generator's inputs.
    manifest = open(os.path.join(ROOT, "kuf_cod_manifest.toml")).read()
    shutil.copyfile(os.path.join(ROOT, "kuf_cod_manifest.toml"), os.path.join(out, "kuf_cod_manifest.toml"))
    for inc in re.findall(r'"(config/[^"]+\.toml)"', manifest):
        copy_file(os.path.join(ROOT, inc), os.path.join(out, inc))

    # The compiler.
    os.makedirs(os.path.join(out, "toolchain"), exist_ok=True)
    tool = os.path.join(out, "toolchain", "llvm")
    if a.plain_compiler_script:
        shutil.copyfile(a.plain_compiler_script, tool)
        os.chmod(tool, 0o755)
    else:
        subprocess.check_call(["strip", "-o", tool, os.path.join(a.compiler, "bin", "llvm")])
        needed = re.findall(r"NEEDED.*\[(.*)\]", run("readelf", "-d", tool))
        for lib in needed:
            if not re.match(r"lib(c|m|dl|pthread|rt|gcc_s)\.so|ld-linux", lib):
                die("the compiler needs " + lib + " from the system")
        newest = sorted(set(re.findall(r"GLIBC_(2\.\d+)", run("objdump", "-T", tool))),
                        key=lambda v: [int(x) for x in v.split(".")])[-1]
        if [int(x) for x in newest.split(".")] > [2, 31]:
            die("the compiler needs glibc " + newest)

    resource_flag = "-resource-dir=@B@/toolchain/lib/clang/" + os.path.basename(paths.resource)
    target_flags = [resource_flag] + flags
    links = [resource_flag] + link_flags(link_command, paths)
    libraries = ["-Wl,--whole-archive", "@B@/lib/libkuf_own.a", "-Wl,--no-whole-archive"]
    order = re.findall(r"(?:^|\s)((?:sdk-lib/)?lib[\w-]+\.a|bin/lib\w+\.so|-l[\w+]+)(?=\s|$)", link.split(" && ")[1]
                       if link.startswith(": && ") else link)
    for item in order:
        if item.endswith(".a"):
            libraries.append("@B@/lib/" + os.path.basename(item))
        elif item.endswith(".so"):
            libraries.append("-l" + re.match(r"lib(.*)\.so", os.path.basename(item)).group(1))
        else:
            libraries.append(item)

    # The id of everything the generated objects depend on: not the project's own code, which is only linked.
    h = hashlib.sha256()
    h.update("\n".join(target_flags).encode())
    h.update(open(tool, "rb").read())
    for sub in ("toolchain/lib", "sysroot/usr/include", "libstdcxx/include", "tree", "config"):
        for base, dirs, files in sorted(os.walk(os.path.join(out, sub))):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(base, f)
                h.update(os.path.relpath(p, out).encode())
                h.update(open(p, "rb").read())
    for sub in ("src/rexglue", "src/codegen"):
        d = os.path.join(ROOT, "thirdparty/rexglue-sdk", sub)
        for base, dirs, files in sorted(os.walk(d)):
            dirs.sort()
            for f in sorted(files):
                h.update(open(os.path.join(base, f), "rb").read())
    sources = sum(1 for c in commands if re.search(r"-c \S*generated/default/\S+\.cpp", c))
    with open(os.path.join(out, "recipe.txt"), "w") as f:
        f.write("# The verified build's commands (tools/package/make_builder.py).\n")
        f.write("generation_id " + h.hexdigest() + "\n")
        f.write("sources %d\n" % sources)
        f.write("compile " + " ".join(target_flags) + "\n")
        f.write("link " + " ".join(links) + "\n")
        f.write("link_libraries " + " ".join(libraries) + "\n")
    print("builder: %d headers copied, %d generated headers left out, %d sources expected, id %s" %
          (copied, regenerated, sources, h.hexdigest()[:16]))


if __name__ == "__main__":
    main()
