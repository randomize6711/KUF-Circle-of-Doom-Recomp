#!/usr/bin/env python3
"""Audits a release folder before publication (docs/RELEASING.md "Audit"); reads only, prints a report.

  python3 -I tools/package/audit_release.py build/releases/vX.Y.Z [--signers FILE] [--unsigned] [--report FILE]
                                            [--image FILE]

Checks:
  - the assets: exactly the package, the update manifest, their .sig files (not with --unsigned), the setup AppImage
    and the source archive (when present), SHA256SUMS and the notes / publish text files; nothing else;
  - the signatures (with the updater's own code) against --signers (default tools/package/allowed_signers), the
    manifest against the package (version, platform, name, size, SHA-256), the package's VERSION;
  - the package's files against an allow-list (the runtime libraries, code generator, builder, launcher, installer,
    updater, VERSION, MANIFEST.sha256, README, licences, the public key) and its MANIFEST.sha256;
  - no code of the game anywhere (docs/LOCAL_BUILD.md): no bin/kuf_cod and no generated source; no file defines a
    translated function except the project's own hand-written replacements, listed in its sources; with --image FILE
    (a local memory image, never published) no file holds a chunk of the game's image;
  - no console keys: no file holds the Xbox retail key (compared by its SHA-256 only, so this audit does not carry it);
  - the setup AppImage: unpacked (--appimage-extract, in a temporary folder), its files against an allow-list, its
    embedded package identical to the release package (same MANIFEST.sha256 and files), its public key the same;
  - every byte of every file in both: no private keys (PEM / OpenSSH), no GitHub or other tokens, no game data (by
    name and the XEX magic), no home-folder paths, user name or the git e-mail address;
  - SHA256SUMS matches;
  - one version everywhere (release consistency, mandatory before publishing): the package's name, its VERSION, the
    manifest, the AppImage's name and its embedded package, and the version compiled into the game program (no
    development version strings left in the binaries); a stable release has no pre-release part, a pre-release
    version is published on the pre-release channel;
  - the AppImage's game package identical to the release package, file by file (SHA-256);
  - the updater present and pointing at the official repository through GitHub's API (kuf_update.py, install.sh,
    the setup's default), no update configuration (update.conf: written at installation) in either;
  - the public key is the production key (pinned fingerprint below) unless --test-key (disposable test releases;
    publish_release.sh never passes it);
  - with SOURCE (make_release.sh writes it): the game program carries that commit (both artifacts from one revision);
  - portability (M11, docs/STEAM_DECK.md "Portable build"): no binary of the package or of the setup imports a glibc
    symbol version newer than the baseline (cmake/steamrt3-sniper-version.cmake: 2.31, SteamOS and older desktops);
    the package carries its libstdc++ (bin/compat) and nothing needs a newer GLIBCXX than it defines; the setup links
    no libstdc++; REQUIRES states exactly what the binaries need, and the signed manifest's `requires` and the
    AppImage's requires.txt are the same.
Exit 0 when clean, 1 with the problems listed.
"""
import argparse
import fnmatch
import getpass
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "updater"))
import kuf_update  # noqa: E402

import struct  # noqa: E402

OFFICIAL_REPO = "randomize6711/KUF-Circle-of-Doom-Recomp"
PRODUCTION_FINGERPRINT = "SHA256:R86LsKwWaA/Nh5mLLfG1WBZN21FAcmAmEC78MVVBZoo"  # docs/RELEASING.md section 3
DEV_VERSION = re.compile(rb"\b\d+\.\d+\.\d+-dev\.\d+")
PACKAGE_ALLOWED = ["bin/rexgluerd", "builder/*", "bin/librexruntimerd.so", "bin/librexgpu-xenosrd.so", "bin/libTracyClientrd.so",
                   "bin/compat/libstdc++.so.6", "kuf-cod.sh", "install.sh", "kuf_update.py", "VERSION", "REQUIRES",
                   "MANIFEST.sha256", "README-PACKAGE.txt", "licenses/*", "allowed_signers"]
APPIMAGE_ALLOWED = ["AppRun", "kuf-setup.desktop", "kuf-setup.png", ".DirIcon", "usr/bin/kuf-setup",
                    "usr/share/kuf-setup/assets/*", "usr/share/kuf-setup/payload/allowed_signers",
                    "usr/share/kuf-setup/payload/requires.txt", "usr/share/kuf-setup/payload/package/*"]
GAME_DATA = ("*.xex", "*.xbe", "*.pak", "*.sox", "*.xwb", "*.xsb", "*.xgs", "*.wmv", "*.xma", "*.xpr", "*.bik",
             "*.iso", "*.stfs", "*.sav", "*.gpd", "*.xbx")
# The SHA-256 of the 16-byte Xbox 360 console keys the SDK's upstream carries: the audit looks for them without
# containing them.
CONSOLE_KEYS = {"20": "a0bd3f5f56adf2b0238a2702c0c0cd8548fb34b3dc19345943dc011010723fe4",
                "e1": "9e119c1592378d53a7668b6f78b2fba5f3b18e91b1dd27a46aa59f5f49c4f79f"}
TRANSLATED = re.compile(rb"\bsub_8[0-9A-Fa-f]{7}\b")


def has_console_key(data):
    """Whether a 16-byte window hashes to a key. The window's first byte narrows the search."""
    for first, digest in CONSOLE_KEYS.items():
        byte = bytes.fromhex(first)
        at = data.find(byte)
        while at != -1:
            if hashlib.sha256(data[at:at + 16]).hexdigest() == digest:
                return True
            at = data.find(byte, at + 1)
    return False


def own_hook_names():
    """The game functions the project replaces by hand in its own sources; anything else defined is translated code."""
    names = set()
    for base, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if f.endswith((".cpp", ".h")):
                text = open(os.path.join(base, f), errors="replace").read()
                names.update("sub_" + a.upper() for a in re.findall(r"\b(?:0x|sub_)?(8[0-9A-Fa-f]{7})\b", text))
    return names


def no_game_code(base, label, files, problems, lines, image=None):
    """Hard checks of the distribution model: no translated code and no console key, in any file."""
    hooks = own_hook_names()
    defined_own = 0
    chunks = []
    if image:
        import random
        data = open(image, "rb").read()
        rnd = random.Random(1)
        while len(chunks) < 3000:
            at = rnd.randrange(0, len(data) - 48)
            c = data[at:at + 48]
            if len(set(c)) >= 24:
                chunks.append(c)
        # Tables that are in everyone's code (byte sequences, CRC tables) are no evidence: what the compiler or the C
        # library, which hold nothing of the game, also hold is left out.
        for rel in files:
            if rel.endswith(("builder/toolchain/llvm", "builder/sysroot/lib/x86_64-linux-gnu/libc.so.6")):
                control = open(os.path.join(base, rel), "rb").read()
                chunks = [c for c in chunks if c not in control]
        chunks = [c for c in chunks if sum(abs(c[i + 1] - c[i]) <= 1 for i in range(47)) < 40 and
                  sum(c[i + 2] - c[i] == 1 for i in range(46)) < 20]
    hits = 0
    for rel in files:
        p = os.path.join(base, rel)
        data = open(p, "rb").read()
        if has_console_key(data):
            problems.append(f"{label}: {rel} contains the console key")
        for c in chunks:
            if c in data:
                hits += 1
                problems.append(f"{label}: {rel} contains game image data")
                break
        if os.path.basename(rel) == "kuf_cod":
            problems.append(f"{label}: {rel}: the game's program must not be distributed")
        if re.search(r"kuf_cod_(recomp|funcs|init|register)\b", os.path.basename(rel)) or "generated/" in rel:
            problems.append(f"{label}: {rel} looks like generated code")
        if data[:4] == b"\x7fELF" or data[:8] == b"!<arch>\n":
            try:
                out = subprocess.run(["nm", "--defined-only", p], capture_output=True, text=True, check=False).stdout
            except OSError:
                problems.append("nm is not available: the translated-code check did not run")
                return
            for name in re.findall(r" [TtDdRrBbWw] (sub_8[0-9A-Fa-f]{7})$", out, re.M):
                if "sub_" + name[4:].upper() in hooks:
                    defined_own += 1
                else:
                    problems.append(f"{label}: {rel} defines {name}: translated code")
                    break
    lines.append(f"{label}: no console key, no game program, no generated code; {defined_own} own hook definitions"
                 + (f"; no game image data ({len(chunks)} sampled chunks of 48 bytes, {hits} hits)" if image else ""))


SECRETS = [(re.compile(rb"-----BEGIN [A-Z ]*PRIVATE KEY-----"), "a PEM private key"),
           (re.compile(rb"openssh-key-v1\0"), "an OpenSSH private key"),
           (re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{36}\b"), "a GitHub token"),
           (re.compile(rb"\bgithub_pat_[A-Za-z0-9_]{40,}"), "a GitHub token"),
           (re.compile(rb"\bAKIA[0-9A-Z]{16}\b"), "an AWS key"),
           (re.compile(rb"xox[baprs]-[A-Za-z0-9-]{10,}"), "a Slack token")]


def glibc_baseline():
    text = open(os.path.join(ROOT, "cmake", "steamrt3-sniper-version.cmake")).read()
    return re.search(r"^set\(KUFCOD_GLIBC_BASELINE ([0-9.]+)\)$", text, re.M).group(1)


def _vkey(v):
    return tuple(int(x) for x in v.split("."))


def elf_versions(path):
    """An ELF64 file's symbol versions: ({library: {versions it needs}}, {versions it defines}); None if not ELF.
    From .gnu.version_r (Verneed) and .gnu.version_d (Verdef): exactly what the dynamic loader checks."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return None
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]

    def cstr(strtab, off):
        base = sections[strtab][4] + off
        return data[base:data.index(b"\0", base)].decode()
    needs, defines = {}, set()
    for _, stype, _, _, off, _, link, info, _, _ in sections:
        if stype == 0x6FFFFFFE:  # SHT_GNU_verneed
            at = off
            for _ in range(info):
                _, cnt, file_, aux, nxt = struct.unpack_from("<HHIII", data, at)
                lib = needs.setdefault(cstr(link, file_), set())
                a = at + aux
                for _ in range(cnt):
                    _, _, _, name, anext = struct.unpack_from("<IHHII", data, a)
                    lib.add(cstr(link, name))
                    a += anext
                at += nxt
        elif stype == 0x6FFFFFFD:  # SHT_GNU_verdef
            at = off
            for _ in range(info):
                _, _, _, cnt, _, aux, nxt = struct.unpack_from("<HHHHIII", data, at)
                if cnt:
                    defines.add(cstr(link, struct.unpack_from("<I", data, at + aux)[0]))
                at += nxt
    return needs, defines


def newest(versions, prefix):
    found = [v[len(prefix):] for v in versions if v.startswith(prefix) and re.fullmatch(r"[0-9.]+", v[len(prefix):])]
    return max(found, key=_vkey) if found else ""


def portability(top, label, problems, lines, baseline, package=True):
    """The binaries under `top`: glibc at most the baseline; for the package, its bundled libstdc++ covers the
    others' GLIBCXX needs and REQUIRES says so. Returns the REQUIRES dict the binaries call for."""
    need_glibc, need_cxx, bundled = "", "", ""
    for base, _, files in os.walk(top):
        for f in sorted(files):
            path = os.path.join(base, f)
            rel = os.path.relpath(path, top)
            # The local build's link inputs (C and C++ libraries) are never loaded from there; the compiler is a program
            # the setup starts: it has to meet the baseline, but what it needs is not the game's.
            if package and rel.startswith("builder/") and rel not in ("builder/kuf-localbuild", "builder/toolchain/llvm"):
                continue
            v = elf_versions(path)
            if v is None:
                continue
            needs, defines = v
            all_needs = set().union(*needs.values()) if needs else set()
            g = newest(all_needs, "GLIBC_")
            if g and _vkey(g) > _vkey(baseline):
                problems.append(f"{label}: {rel} needs glibc {g} (the portable baseline is {baseline})")
            need_glibc = max((x for x in (need_glibc, g) if x), key=_vkey, default="")
            if rel == "bin/compat/libstdc++.so.6":
                bundled = newest(defines, "GLIBCXX_")
                continue
            c = newest(all_needs, "GLIBCXX_")
            need_cxx = max((x for x in (need_cxx, c) if x), key=_vkey, default="")
            if not package and any("libstdc++" in lib for lib in needs):
                problems.append(f"{label}: {rel} links libstdc++ dynamically (the setup must carry it)")
    req = {k: v for k, v in (("glibc", need_glibc), ("glibcxx", need_cxx), ("glibcxx_bundled", bundled)) if v}
    if package:
        if not bundled:
            problems.append(f"{label}: no bundled libstdc++ (bin/compat/libstdc++.so.6): not a portable build")
        elif need_cxx and _vkey(need_cxx) > _vkey(bundled):
            problems.append(f"{label}: the game needs GLIBCXX {need_cxx}, its bundled libstdc++ has {bundled}")
        lines.append(f"{label}: portable: glibc {need_glibc} (baseline {baseline}), GLIBCXX {need_cxx}, "
                     f"bundled libstdc++ GLIBCXX {bundled or 'none'}")
    else:
        lines.append(f"{label}: portable: glibc {need_glibc} (baseline {baseline}), libstdc++ linked in")
    return req


def read_requires(path):
    out = {}
    for line in open(path):
        key, _, value = line.strip().partition(" ")
        if key:
            out[key.replace("-", "_")] = value
    return out


def personal_patterns():
    pats = []
    home = os.path.expanduser("~")
    if home and home != "/":
        pats.append((re.escape(home).encode(), "the home folder path"))
    user = getpass.getuser()
    if len(user) >= 4:
        pats.append((re.escape(user).encode(), f"the user name {user!r}"))
    try:
        mail = subprocess.run(["git", "-C", ROOT, "config", "user.email"], capture_output=True, text=True).stdout.strip()
    except OSError:
        mail = ""
    if mail:
        pats.append((re.escape(mail).encode(), "the git e-mail address"))
    pats.append((rb"/home/[a-z_][a-z0-9_-]*/", "a /home/<user>/ path"))
    return [(re.compile(p), why) for p, why in pats]


def scan_tree(base, label, allowed, problems, personal):
    files = []
    for d, _, names in os.walk(base):
        for n in names:
            p = os.path.join(d, n)
            rel = os.path.relpath(p, base)
            files.append(rel)
            if os.path.islink(p):
                problems.append(f"{label}: {rel} is a link")
                continue
            if not any(fnmatch.fnmatch(rel, a) for a in allowed):
                problems.append(f"{label}: {rel} is not an expected file")
            if any(fnmatch.fnmatch(n.lower(), g) for g in GAME_DATA):
                problems.append(f"{label}: {rel} looks like game data")
            data = open(p, "rb").read()
            if data[:4] == b"XEX2":
                problems.append(f"{label}: {rel} is an Xbox 360 executable")
            for rx, why in SECRETS + personal:
                if rx.search(data):
                    problems.append(f"{label}: {rel} contains {why}")
    return sorted(files)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("release")
    ap.add_argument("--image", help="a local memory image of the game: its absence from the release is checked")
    ap.add_argument("--signers", default=os.path.join(ROOT, "tools", "package", "allowed_signers"))
    ap.add_argument("--unsigned", action="store_true", help="before signing: no .sig files expected")
    ap.add_argument("--report")
    ap.add_argument("--test-key", action="store_true", help="a disposable test release: any key in --signers")
    a = ap.parse_args()
    d = os.path.abspath(a.release)
    problems, lines = [], []
    personal = personal_patterns()
    names = sorted(os.listdir(d))
    pkgs = [n for n in names if re.fullmatch(r"kuf-cod-recomp-.+-linux-x86_64\.tar\.gz", n)]
    if len(pkgs) != 1:
        print(f"AUDIT FAILED: expected one package in {d}, found {pkgs}")
        return 1
    pkg = pkgs[0]
    version = pkg[len("kuf-cod-recomp-"):-len("-linux-x86_64.tar.gz")]
    manifest = pkg[:-len(".tar.gz")] + ".update.json"
    appimage = f"KingdomUnderFire-CircleOfDoom-Setup-{version}-x86_64.AppImage"
    source_archive = pkg[:-len("-linux-x86_64.tar.gz")] + "-source.tar.xz"
    toolchain_sources = pkg[:-len("-linux-x86_64.tar.gz")] + "-toolchain-sources.tar"
    expected = {pkg, manifest, source_archive, toolchain_sources, "SHA256SUMS", "notes.txt", "publish.txt", "audit.txt",
                "SOURCE"}
    if "summary.txt" in names:
        expected.add("summary.txt")
    if not a.unsigned:
        expected |= {pkg + ".sig", manifest + ".sig"}
    if appimage in names:
        expected.add(appimage)
    for n in names:
        if n not in expected:
            problems.append(f"unexpected file in the release folder: {n}")
    for n in expected - {"audit.txt", "SHA256SUMS", "publish.txt", "SOURCE"} - ({appimage} if appimage not in names else set()):
        if n not in names:
            problems.append(f"missing: {n}")
    lines.append(f"release {version} in {d}")

    # Signatures and manifest, with the updater's code.
    with tempfile.TemporaryDirectory() as tmp:
        open(os.path.join(tmp, "update.conf"), "w").write("repo = audit/check\n")
        if not os.path.isfile(a.signers):
            problems.append(f"no public key {a.signers}")
        else:
            open(os.path.join(tmp, "allowed_signers"), "w").write(open(a.signers).read())
        inst = kuf_update.Install(tmp)
        if not a.unsigned and os.path.isfile(a.signers):
            for f, ns in ((pkg, kuf_update.NAMESPACE), (manifest, kuf_update.MANIFEST_NAMESPACE)):
                try:
                    kuf_update.verify_signature(inst, os.path.join(d, f), os.path.join(d, f + ".sig"), ns)
                    lines.append(f"signature ok: {f} ({ns})")
                except kuf_update.Refused as e:
                    problems.append(f"signature of {f}: {e}")
    try:
        m = json.load(open(os.path.join(d, manifest)))
        kuf_update.verify_download(os.path.join(d, pkg), m)
        for k, want in (("version", version), ("platform", "linux-x86_64"), ("product", "kuf-cod-recomp"),
                        ("schema", 1)):
            if m.get(k) != want:
                problems.append(f"manifest {k} {m.get(k)!r}, expected {want!r}")
        lines.append(f"manifest ok: {json.dumps(m, sort_keys=True)}")
    except (OSError, ValueError, kuf_update.Refused, KeyError) as e:
        problems.append(f"manifest: {e}")

    if os.path.isfile(a.signers) and not a.test_key:
        prints = []
        for line in open(a.signers):
            parts = line.split()
            if len(parts) >= 4:
                with tempfile.NamedTemporaryFile("w", suffix=".pub") as pub:
                    pub.write(f"{parts[2]} {parts[3]}\n")
                    pub.flush()
                    out = subprocess.run(["ssh-keygen", "-lf", pub.name], capture_output=True, text=True).stdout
                    prints.append(out.split()[1] if len(out.split()) > 1 else "?")
        if prints != [PRODUCTION_FINGERPRINT]:
            problems.append(f"the public key is not the production key {PRODUCTION_FINGERPRINT}: {prints}")
        else:
            lines.append(f"public key: the production key {PRODUCTION_FINGERPRINT}")
    try:
        m_channel = json.load(open(os.path.join(d, manifest))).get("channel")
    except (OSError, ValueError):
        m_channel = None
    pre = "-" in version
    if pre and m_channel != "prerelease":
        problems.append(f"{version} is a pre-release version: its channel must be prerelease, not {m_channel!r}")
    if not pre and m_channel != "stable":
        problems.append(f"{version} is a release version: its channel must be stable, not {m_channel!r}")
    signers_text = open(a.signers).read() if os.path.isfile(a.signers) else None
    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(os.path.join(d, pkg)) as tar:
            tar.extractall(tmp, filter="data")
        top = os.path.join(tmp, pkg[:-len(".tar.gz")])
        if not os.path.isdir(top) or len(os.listdir(tmp)) != 1:
            problems.append("the package must have exactly one top folder named after it")
        else:
            files = scan_tree(top, "package", PACKAGE_ALLOWED, problems, personal)
            if open(os.path.join(top, "VERSION")).read().strip() != version:
                problems.append("package VERSION differs from its name")
            try:
                kuf_update.verify_manifest(top, version)
            except kuf_update.Refused as e:
                problems.append(f"package: {e}")
            if signers_text is not None and (not os.path.isfile(os.path.join(top, "allowed_signers")) or
                                             open(os.path.join(top, "allowed_signers")).read() != signers_text):
                problems.append("the package does not carry this public key (allowed_signers)")
            no_game_code(top, "package", files, problems, lines, a.image)
            own = os.path.join(top, "builder/lib/libkuf_own.a")  # the project's prebuilt code carries the version stamps
            program = open(own, "rb").read() if os.path.isfile(own) else b""
            source = os.path.join(d, "SOURCE")  # the commit the release was built from (make_release.sh)
            if os.path.isfile(source):
                commit = dict(l.strip().split("=", 1) for l in open(source) if "=" in l).get("commit", "")
                if not re.fullmatch(r"[0-9a-f]{40}", commit) or b"\0" + commit[:10].encode() + b"\0" not in program:
                    problems.append(f"the game program was not built from the release's commit {commit[:10]}")
                else:
                    lines.append(f"built from commit {commit} (the game program carries {commit[:10]})")
            if not re.search(rb"(?<![0-9A-Za-z.+-])" + re.escape(version.encode()) + rb"\0", program):
                problems.append(f"the game program does not carry the version {version} (built from another version)")
            for f in files:
                if f.startswith(("bin/", "builder/lib/")) and DEV_VERSION.search(open(os.path.join(top, f), "rb").read()):
                    problems.append(f"package: {f} carries a development version string")
            for need in ("licenses/PROJECT-LICENSE.txt", "licenses/PROJECT-NOTICE.txt"):
                if not os.path.isfile(os.path.join(top, need)):
                    problems.append(f"package: no {need}")
            for need in ("builder/kuf-localbuild", "builder/recipe.txt", "builder/toolchain/llvm", "bin/rexgluerd"):
                if not os.path.isfile(os.path.join(top, need)):
                    problems.append(f"package: no {need}: it cannot make the game's program")
            recipe = os.path.join(top, "builder/recipe.txt")
            if os.path.isfile(recipe) and "/home/" in open(recipe).read():
                problems.append("package: builder/recipe.txt names a path of the build machine")
            tool = os.path.join(top, "builder/toolchain/llvm")
            if os.path.isfile(tool) and open(tool, "rb").read(2) == b"#!":
                problems.append("package: the compiler is a test stand-in")
            upd = os.path.join(top, "kuf_update.py")
            if not os.path.isfile(upd):
                problems.append("package: no updater (kuf_update.py)")
            elif f'GITHUB_API = "https://api.github.com"' not in open(upd).read():
                problems.append("package: the updater's API is not https://api.github.com")
            inst = os.path.join(top, "install.sh")
            if not os.path.isfile(inst) or f'DEFAULT_REPO="{OFFICIAL_REPO}"' not in open(inst).read():
                problems.append(f"package: install.sh does not default to {OFFICIAL_REPO}")
            if any(os.path.basename(f) == "update.conf" for f in files):
                problems.append("package: carries an update.conf (written at installation only)")
            # Portability: what the binaries need, against the baseline, REQUIRES and the signed manifest.
            req = portability(top, "package", problems, lines, glibc_baseline())
            req_file = os.path.join(top, "REQUIRES")
            if not os.path.isfile(req_file):
                problems.append("package: no REQUIRES (made by an older package_linux.sh)")
            elif read_requires(req_file) != req:
                problems.append(f"package: REQUIRES says {read_requires(req_file)}, the binaries need {req}")
            try:
                m_req = json.load(open(os.path.join(d, manifest))).get("requires")
            except (OSError, ValueError):
                m_req = None
            if m_req != req:
                problems.append(f"the manifest's requires {m_req} is not the package's {req}")
            pkg_hashes = {f: sha256(os.path.join(top, f)) for f in files}
            lines.append(f"package: {len(files)} files")
            lines += [f"  {f}  {os.path.getsize(os.path.join(top, f))}" for f in files]
            pkg_manifest = open(os.path.join(top, "MANIFEST.sha256")).read()

        if appimage in names:
            img = os.path.join(tmp, "appimage")
            os.makedirs(img)
            runnable = os.path.join(tmp, "setup.AppImage")  # a private executable copy (downloads are not)
            with open(os.path.join(d, appimage), "rb") as src, open(runnable, "wb") as dst:
                dst.write(src.read())
            os.chmod(runnable, 0o700)
            r = subprocess.run([runnable, "--appimage-extract"], cwd=img, capture_output=True,
                               env=dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1"))
            root = os.path.join(img, "squashfs-root")
            if r.returncode != 0 or not os.path.isdir(root):
                problems.append(f"the AppImage could not be unpacked: {r.stderr[-200:]!r}")
            else:
                files = scan_tree(root, "AppImage", APPIMAGE_ALLOWED, problems, personal)
                no_game_code(root, "AppImage", files, problems, lines, a.image)
                for f in files:
                    if f.endswith((".ttf", ".otf")) and not os.path.isfile(os.path.join(root, f[:-4] + "-OFL.txt")):
                        problems.append(f"AppImage: {f} has no licence file next to it")
                emb = os.path.join(root, "usr/share/kuf-setup/payload/package")
                if open(os.path.join(emb, "MANIFEST.sha256")).read() != pkg_manifest:
                    problems.append("the AppImage's game package differs from the release package")
                try:
                    kuf_update.verify_manifest(emb, version)
                except kuf_update.Refused as e:
                    problems.append(f"AppImage package: {e}")
                key = os.path.join(root, "usr/share/kuf-setup/payload/allowed_signers")
                if signers_text is not None and (not os.path.isfile(key) or open(key).read() != signers_text):
                    problems.append("the AppImage does not carry this public key")
                prefix = "usr/share/kuf-setup/payload/package/"
                emb_hashes = {f[len(prefix):]: sha256(os.path.join(root, f)) for f in files if f.startswith(prefix)}
                if emb_hashes != pkg_hashes:
                    differ = sorted(k for k in set(emb_hashes) | set(pkg_hashes) if emb_hashes.get(k) != pkg_hashes.get(k))
                    problems.append(f"the AppImage's package files differ from the release package: {differ[:5]}")
                portability(os.path.join(root, "usr/bin"), "setup", problems, lines, glibc_baseline(), package=False)
                rq = os.path.join(root, "usr/share/kuf-setup/payload/requires.txt")
                if not os.path.isfile(rq) or open(rq).read() != open(os.path.join(emb, "REQUIRES")).read():
                    problems.append("the AppImage's requires.txt is not its package's REQUIRES")
                setup = os.path.join(root, "usr/bin/kuf-setup")
                if not os.path.isfile(setup) or OFFICIAL_REPO.encode() not in open(setup, "rb").read():
                    problems.append(f"the setup does not default to {OFFICIAL_REPO}")
                if any(os.path.basename(f) == "update.conf" for f in files):
                    problems.append("AppImage: carries an update.conf")
                lines.append(f"AppImage: {len(files)} files; its package identical to the release package, file by "
                             f"file ({len(emb_hashes)} SHA-256)")
                lines += [f"  {k}  {v}" for k, v in sorted(pkg_hashes.items()) if k.startswith("bin/")]

    # The source archive: corresponding source, without the console keys, the game or generated code.
    if source_archive in names:
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["tar", "-xJf", os.path.join(d, source_archive), "-C", tmp], check=True)
            count = 0
            # Third-party documents mention /home/<someone>/ in examples; this machine's own name and path still fail.
            specific = [(rx, why) for rx, why in personal if "/home/<user>/" not in why]
            for base, _, files in os.walk(tmp):
                for f in files:
                    p = os.path.join(base, f)
                    rel = os.path.relpath(p, tmp)
                    count += 1
                    if os.path.islink(p):
                        continue
                    data = open(p, "rb").read()
                    if has_console_key(data):
                        problems.append(f"source archive: {rel} contains a console key")
                    if data[:4] == b"XEX2":
                        problems.append(f"source archive: {rel} is an Xbox 360 executable")
                    if "/generated/default/" in rel or re.search(r"kuf_cod_(recomp|funcs)\.\d+\.", f):
                        problems.append(f"source archive: {rel} looks like generated code")
                    if any(fnmatch.fnmatch(f.lower(), g) for g in GAME_DATA):
                        problems.append(f"source archive: {rel} looks like game data")
                    for rx, why in SECRETS + specific:
                        # The audit's own test feeds it a fake key.
                        if rx.search(data) and not rel.endswith("tests/updater/test_release_audit.py"):
                            problems.append(f"source archive: {rel} contains {why}")
                    if re.search(rb"0x20,\s*0xB1,\s*0x85,\s*0xA5", data):
                        problems.append(f"source archive: {rel} quotes a console key")
            lines.append(f"source archive: {count} files; no console key, game file or generated code")
    else:
        problems.append("no source archive: the release's GPL and LGPL parts need their source")
    if toolchain_sources not in names:
        problems.append("no toolchain sources archive")
    sums = os.path.join(d, "SHA256SUMS")
    if not os.path.isfile(sums):
        problems.append("no SHA256SUMS: the release is incomplete")
    if os.path.isfile(sums):
        listed = set()
        published = {n for n in names if n in expected and n not in ("SHA256SUMS", "notes.txt", "summary.txt",
                                                                       "publish.txt", "audit.txt", "SOURCE")}
        for line in open(sums):
            digest, name = line.split(None, 1)
            name = name.strip().lstrip("*")
            listed.add(name)
            if not os.path.isfile(os.path.join(d, name)) or sha256(os.path.join(d, name)) != digest:
                problems.append(f"SHA256SUMS: {name} does not match (or is not a published asset)")
        if listed != published:
            problems.append(f"SHA256SUMS lists {sorted(listed - published)} beyond the assets, misses "
                            f"{sorted(published - listed)}")
    lines.append("assets:")
    lines += [f"  {n}  {os.path.getsize(os.path.join(d, n))} bytes  sha256 {sha256(os.path.join(d, n))}"
              for n in names if n not in ("audit.txt",)]
    lines.append("AUDIT: " + ("clean" if not problems else f"{len(problems)} problem(s)"))
    lines += [f"  PROBLEM: {p}" for p in problems]
    text = "\n".join(lines) + "\n"
    if a.report:
        open(a.report, "w").write(text)
    print(text, end="")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
