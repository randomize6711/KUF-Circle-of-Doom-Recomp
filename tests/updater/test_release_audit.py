#!/usr/bin/env python3
"""Release consistency rules (tools/package/audit_release.py; docs/RELEASING.md "Audit"): synthetic releases signed
with throw-away keys, each broken in one way, must be refused; the good one passes. No network, nothing outside a
temporary folder. Each case prints one line; exit 1 on a failure."""
import hashlib
import io
import struct
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AUDIT = os.path.join(ROOT, "tools", "package", "audit_release.py")
NS, MNS = "kuf-cod-recomp-release", "kuf-cod-recomp-manifest"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
results = []


def fake_elf(needs=None, defines=None):
    """A minimal ELF64 file with only what the audit reads: .gnu.version_r (needs: {library: [versions]}) and
    .gnu.version_d (defines: [versions], the first is the file's own name). Not loadable; enough for the audit."""
    strtab = bytearray(b"\0")

    def name(t):
        off = len(strtab)
        strtab.extend(t.encode() + b"\0")
        return off
    verneed = bytearray()
    libs = list((needs or {}).items())
    for i, (lib, versions) in enumerate(libs):
        aux = b"".join(struct.pack("<IHHII", 0, 0, 2 + j, name(v), 16 if j + 1 < len(versions) else 0)
                       for j, v in enumerate(versions))
        size = 16 + len(aux)
        verneed += struct.pack("<HHIII", 1, len(versions), name(lib), 16, size if i + 1 < len(libs) else 0) + aux
    verdef = bytearray()
    for i, v in enumerate(defines or []):
        verdef += struct.pack("<HHHHIII", 1, 0, i + 1, 1, 0, 20, 28 if i + 1 < len(defines) else 0)
        verdef += struct.pack("<II", name(v), 0)
    shstr = b"\0.dynstr\0.gnu.version_r\0.gnu.version_d\0.shstrtab\0"
    body = bytes(strtab) + bytes(verneed) + bytes(verdef) + shstr
    o_str, o_vn, o_vd, o_sh = 64, 64 + len(strtab), 64 + len(strtab) + len(verneed), 64 + len(strtab) + len(verneed) + len(verdef)
    shoff = 64 + len(body)
    secs = [(0, 0, 0, 0, 0, 0), (1, 3, o_str, len(strtab), 0, 0), (9, 0x6FFFFFFE, o_vn, len(verneed), 1, len(libs)),
            (24, 0x6FFFFFFD, o_vd, len(verdef), 1, len(defines or [])), (39, 3, o_sh, len(shstr), 0, 0)]
    head = b"\x7fELF\x02\x01\x01" + b"\0" * 9 + struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, 0, 0, shoff, 0, 64, 0, 0,
                                                             64, len(secs), 4)
    sh = b"".join(struct.pack("<IIQQQQIIQQ", n, t, 0, 0, o, sz, l, inf, 1, 0) for n, t, o, sz, l, inf in secs)
    return head + body + sh


GOOD_NEEDS = {"libc.so.6": ["GLIBC_2.2.5", "GLIBC_2.30"], "libstdc++.so.6": ["GLIBCXX_3.4.32"]}
COMPAT = fake_elf({"libc.so.6": ["GLIBC_2.26"]}, ["libstdc++.so.6", "GLIBCXX_3.4.32", "GLIBCXX_3.4.33"])
GOOD_REQUIRES = b"glibc 2.30\nglibcxx 3.4.32\nglibcxx-bundled 3.4.33\n"


def make_key(d):
    key = os.path.join(d, "key")
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "release@kuf-cod-recomp", "-f", key],
                   check=True)
    pub = open(key + ".pub").read().split()
    signers = os.path.join(d, "allowed_signers")
    open(signers, "w").write(f'release@kuf-cod-recomp namespaces="{NS},{MNS}" {pub[0]} {pub[1]}\n')
    return key, signers


def release(d, key, signers, version="0.7.1", channel=None, program=None, extra=None, drop=(), source=COMMIT,
            api='GITHUB_API = "https://api.github.com"', repo="randomize6711/KUF-Circle-of-Doom-Recomp", needs=None,
            requires=GOOD_REQUIRES, manifest_requires="auto", source_files=None, sources=True):
    """A release folder like make_release.sh writes (no AppImage); returns it."""
    os.makedirs(d)
    top = f"kuf-cod-recomp-{version}-linux-x86_64"
    if program is None:
        program = fake_elf(needs or GOOD_NEEDS) + b"\0" + version.encode() + b"\0...\0" + COMMIT[:10].encode() + b"\0"
    # No game program in a package: the project's prebuilt code carries the version and the commit (docs/LOCAL_BUILD.md).
    files = {"builder/lib/libkuf_own.a": program, "bin/rexgluerd": fake_elf(needs or GOOD_NEEDS),
             "builder/kuf-localbuild": fake_elf({"libc.so.6": ["GLIBC_2.2.5"]}), "builder/recipe.txt": b"generation_id x\n",
             "builder/toolchain/llvm": fake_elf({"libc.so.6": ["GLIBC_2.2.5"]}),
             "licenses/PROJECT-LICENSE.txt": b"BSD\n", "licenses/PROJECT-NOTICE.txt": b"scope\n",
             "bin/compat/libstdc++.so.6": COMPAT, "REQUIRES": requires,
             "kuf-cod.sh": b"#!/bin/sh\n", "kuf_update.py": (api + "\n").encode(),
             "install.sh": f'DEFAULT_REPO="{repo}"\n'.encode(), "VERSION": (version + "\n").encode(),
             "allowed_signers": open(signers, "rb").read(), "README-PACKAGE.txt": b"readme\n"}
    files.update(extra or {})
    for k in drop:
        files.pop(k, None)
    files["MANIFEST.sha256"] = "".join(f"{hashlib.sha256(v).hexdigest()}  {k}\n" for k, v in sorted(files.items())
                                       ).encode()
    pkg = os.path.join(d, top + ".tar.gz")
    with tarfile.open(pkg, "w:gz") as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(f"{top}/{name}")
            info.size, info.mode = len(data), 0o755
            tar.addfile(info, io.BytesIO(data))
    channel = channel or ("prerelease" if "-" in version else "stable")
    if manifest_requires == "auto":
        manifest_requires = {"glibc": "2.30", "glibcxx": "3.4.32", "glibcxx_bundled": "3.4.33"}
    m = {"schema": 1, "product": "kuf-cod-recomp", "version": version, "platform": "linux-x86_64", "channel": channel,
         "published": "2026-11-01", "notes": "x", "summary": "x",
         "package": {"name": os.path.basename(pkg), "size": os.path.getsize(pkg),
                     "sha256": hashlib.sha256(open(pkg, "rb").read()).hexdigest()}}
    if manifest_requires is not None:
        m["requires"] = manifest_requires
    man = os.path.join(d, top + ".update.json")
    json.dump(m, open(man, "w"))
    for f, ns in ((pkg, NS), (man, MNS)):
        subprocess.run(["ssh-keygen", "-q", "-Y", "sign", "-f", key, "-n", ns, f], check=True, capture_output=True)
    open(os.path.join(d, "notes.txt"), "w").write("notes\n")
    if source:
        open(os.path.join(d, "SOURCE"), "w").write(f"commit={source}\nversion={version}\n")
    assets = [os.path.basename(x) for x in (pkg, pkg + ".sig", man, man + ".sig")]
    if sources:
        base = f"kuf-cod-recomp-{version}"
        srcdir = os.path.join(d + ".src", base + "-source")
        os.makedirs(srcdir)
        for name, data in (source_files or {"README.md": b"source\n"}).items():
            os.makedirs(os.path.dirname(os.path.join(srcdir, name)) or srcdir, exist_ok=True)
            open(os.path.join(srcdir, name), "wb").write(data)
        subprocess.run(["tar", "-cJf", os.path.join(d, base + "-source.tar.xz"), "-C", d + ".src", base + "-source"], check=True)
        open(os.path.join(d, base + "-toolchain-sources.tar"), "wb").write(b"\0" * 1024)
        assets += [base + "-source.tar.xz", base + "-toolchain-sources.tar"]
    open(os.path.join(d, "SHA256SUMS"), "w").write(
        "".join(f"{hashlib.sha256(open(os.path.join(d, a), 'rb').read()).hexdigest()}  {a}\n" for a in assets))
    return d


def audit(d, signers, test_key=True, image=None):
    args = [sys.executable, "-I", AUDIT, d, "--signers", signers] + (["--test-key"] if test_key else [])
    if image:
        args += ["--image", image]
    r = subprocess.run(args, capture_output=True, text=True)
    return r.returncode, r.stdout


def case(name, fn):
    try:
        fn()
        results.append((True, name, ""))
    except AssertionError as e:
        results.append((False, name, str(e)))


def main():
    with tempfile.TemporaryDirectory(prefix="kuf-audit-test-") as t:
        key, signers = make_key(t)
        n = [0]

        def new(**kw):
            n[0] += 1
            return release(os.path.join(t, f"r{n[0]}"), key, signers, **kw)

        def refused(label, want, **kw):
            code, out = audit(new(**kw), signers)
            assert code == 1 and want in out, f"{label}: {code} {out[-400:]}"

        def good():
            code, out = audit(new(), signers)
            assert code == 0 and "built from commit" in out, out[-600:]
        case("a consistent release passes (signatures, versions, commit, updater, repository)", good)

        def test_key():
            code, out = audit(new(), signers, test_key=False)
            assert code == 1 and "not the production key" in out, out[-400:]
        case("a key that is not the pinned production key is refused (no test keys in releases)", test_key)
        case("a program built for another version is refused",
             lambda: refused("version", "does not carry the version", program=b"\x7fELF\x000.7.0\x00" +
                             COMMIT[:10].encode() + b"\x00"))
        case("a development version string in a binary is refused",
             lambda: refused("dev", "development version", program=b"\x7fELF\x000.7.1\x00" + COMMIT[:10].encode() +
                             b"\x000.7.1-dev.218+g10894b6593\x00"))
        case("a program built from another commit than the release's is refused",
             lambda: refused("commit", "not built from the release's commit", source="f" * 40))
        case("a pre-release version on the stable channel is refused",
             lambda: refused("channel", "pre-release version", version="0.7.1-rc.1", channel="stable"))
        case("a release version on the pre-release channel is refused",
             lambda: refused("channel2", "release version", channel="prerelease"))
        case("a package without the updater is refused", lambda: refused("updater", "no updater", drop=("kuf_update.py",)))
        case("an updater pointing elsewhere than GitHub's API is refused",
             lambda: refused("api", "not https://api.github.com", api='GITHUB_API = "http://127.0.0.1:8000"'))
        case("an installer defaulting to another repository is refused",
             lambda: refused("repo", "does not default to", repo="someone/fork"))
        case("an update.conf in the package is refused",
             lambda: refused("conf", "update.conf", extra={"update.conf": b"api = http://127.0.0.1:1\n"}))

        def sums():
            d = new()
            with open(os.path.join(d, "SHA256SUMS"), "a") as f:
                f.write(hashlib.sha256(b"notes\n").hexdigest() + "  notes.txt\n")
            code, out = audit(d, signers)
            assert code == 1 and "beyond the assets" in out, out[-400:]
        case("SHA256SUMS listing anything but the published assets is refused", sums)

        case("portable (M11): a binary needing a newer glibc than the 2.31 baseline is refused",
             lambda: refused("glibc", "needs glibc 2.38", needs={"libc.so.6": ["GLIBC_2.38"],
                                                                 "libstdc++.so.6": ["GLIBCXX_3.4.32"]}))
        case("portable (M11): a package without its bundled libstdc++ is refused",
             lambda: refused("compat", "no bundled libstdc++", drop=("bin/compat/libstdc++.so.6",)))
        case("portable (M11): a game needing a newer GLIBCXX than the bundled libstdc++ is refused",
             lambda: refused("cxx", "bundled libstdc++ has 3.4.33",
                             needs={"libc.so.6": ["GLIBC_2.30"], "libstdc++.so.6": ["GLIBCXX_3.4.34"]},
                             requires=b"glibc 2.30\nglibcxx 3.4.34\nglibcxx-bundled 3.4.33\n",
                             manifest_requires={"glibc": "2.30", "glibcxx": "3.4.34", "glibcxx_bundled": "3.4.33"}))
        case("portable (M11): REQUIRES that does not state what the binaries need is refused",
             lambda: refused("requires", "REQUIRES says", requires=b"glibc 2.17\nglibcxx 3.4.32\nglibcxx-bundled 3.4.33\n"))
        case("portable (M11): a manifest without the package's requirements is refused",
             lambda: refused("manifest", "the manifest's requires", manifest_requires=None))

        def private():
            code, out = audit(new(extra={"licenses/x": b"-----BEGIN OPENSSH PRIVATE KEY-----\nx\n"}), signers)
            assert code == 1 and "private key" in out, out[-400:]
        case("a private key in the package is refused", private)

        def game_data():
            code, out = audit(new(extra={"licenses/default.xex": b"XEX2...."}), signers)
            assert code == 1 and "game data" in out, out[-400:]
        case("retail game data in the package is refused", game_data)

        # The distribution model (docs/LOCAL_BUILD.md).
        case("the game's program in the package is refused",
             lambda: refused("program", "must not be distributed", extra={"bin/kuf_cod": b"x"}))
        case("generated code in the package is refused",
             lambda: refused("generated", "generated code", extra={"builder/generated/default/kuf_cod_recomp.0.cpp": b"x"}))
        case("a package without its builder is refused",
             lambda: refused("builder", "cannot make the game's program", drop=("builder/kuf-localbuild",)))
        case("a package without the project's licence is refused",
             lambda: refused("licence", "PROJECT-LICENSE", drop=("licenses/PROJECT-LICENSE.txt",)))
        case("a stand-in compiler is refused", lambda: refused("standin", "test stand-in", extra={"builder/toolchain/llvm": b"#!/bin/sh\n"}))
        case("a build machine's path in the recipe is refused",
             lambda: refused("recipe", "path of the build machine", extra={"builder/recipe.txt": b"compile -I/home/someone/x\n"}))

        def translated():
            src = os.path.join(t, "t.c")
            open(src, "w").write("int sub_82ABCDEF(void) { return 1; }\n")
            obj = os.path.join(t, "t.o")
            subprocess.run(["clang", "-c", src, "-o", obj], check=True)
            code, out = audit(new(extra={"builder/lib/t.o": open(obj, "rb").read()}), signers)
            assert code == 1 and "translated code" in out, out[-400:]
        case("a translated function in a prebuilt object is refused", translated)

        def own_hook_allowed():
            # A name the project's own sources give to a hand-written hook is not translated code.
            src = os.path.join(t, "h.c")
            open(src, "w").write("int sub_828008C0(void) { return 1; }\n")  # src/runtime/eh_trace.cpp
            obj = os.path.join(t, "h.o")
            subprocess.run(["clang", "-c", src, "-o", obj], check=True)
            code, out = audit(new(extra={"builder/lib/h.o": open(obj, "rb").read()}), signers)
            assert code == 0, out[-400:]
        case("the project's own hook is accepted", own_hook_allowed)

        def image():
            data = os.urandom(1 << 16)
            path = os.path.join(t, "image.bin")
            open(path, "wb").write(data)
            code, out = audit(new(), signers, image=path)
            assert code == 0, out[-400:]
            code, out = audit(new(extra={"licenses/x": data[100:60000]}), signers, image=path)
            assert code == 1 and "game image data" in out, out[-400:]
        case("data of the game's image in the package is refused (--image)", image)

        def console_key():
            sys.path.insert(0, os.path.join(ROOT, "tools", "package"))
            import audit_release
            fake = os.urandom(16)
            keep = audit_release.CONSOLE_KEYS
            audit_release.CONSOLE_KEYS = {fake[:1].hex(): hashlib.sha256(fake).hexdigest()}
            try:
                assert audit_release.has_console_key(b"before" + fake + b"after")
                assert not audit_release.has_console_key(b"before" + fake[:15] + b"after")
            finally:
                audit_release.CONSOLE_KEYS = keep
            assert len(keep) >= 2, keep
        case("a console key is found, by its hash", console_key)

        def no_sums():
            d = new()
            os.unlink(os.path.join(d, "SHA256SUMS"))
            code, out = audit(d, signers)
            assert code == 1 and "no SHA256SUMS" in out, out[-400:]
        case("a release without SHA256SUMS is refused", no_sums)

        def no_sources():
            code, out = audit(new(sources=False), signers)
            assert code == 1 and "need their source" in out, out[-400:]
        case("a release without its source archive is refused", no_sources)
        case("generated code in the source archive is refused",
             lambda: refused("srcgen", "looks like generated code", source_files={"generated/default/kuf_cod_recomp.0.cpp": b"x"}))
        case("an Xbox executable in the source archive is refused",
             lambda: refused("srcxex", "Xbox 360 executable", source_files={"a.bin": b"XEX2...."}))
    for ok, name, why in results:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}{': ' + why if why else ''}")
    failed = sum(not ok for ok, _, _ in results)
    print(f"RELEASE AUDIT TESTS: {len(results) - failed} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
