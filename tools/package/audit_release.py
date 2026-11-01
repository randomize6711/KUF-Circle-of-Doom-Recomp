#!/usr/bin/env python3
"""Audits a release folder before publication (docs/RELEASING.md "Audit"); reads only, prints a report.

  python3 -I tools/package/audit_release.py build/releases/vX.Y.Z [--signers FILE] [--unsigned] [--report FILE]

Checks:
  - the assets: exactly the package, the update manifest, their .sig files (not with --unsigned), the setup AppImage
    (when present), SHA256SUMS and the notes / publish text files; nothing else;
  - the signatures (with the updater's own code) against --signers (default tools/package/allowed_signers), the
    manifest against the package (version, platform, name, size, SHA-256), the package's VERSION;
  - the package's files against an allow-list (the program, its libraries, the launcher, installer, updater, VERSION,
    MANIFEST.sha256, README, licences, the public key) and its MANIFEST.sha256;
  - the setup AppImage: unpacked (--appimage-extract, in a temporary folder), its files against an allow-list, its
    embedded package identical to the release package (same MANIFEST.sha256 and files), its public key the same;
  - every byte of every file in both: no private keys (PEM / OpenSSH), no GitHub or other tokens, no game data (by
    name and the XEX magic), no home-folder paths, user name or the git e-mail address;
  - SHA256SUMS matches.
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

PACKAGE_ALLOWED = ["bin/kuf_cod", "bin/librexruntimerd.so", "bin/librexgpu-xenosrd.so", "bin/libTracyClientrd.so",
                   "kuf-cod.sh", "install.sh", "kuf_update.py", "VERSION", "MANIFEST.sha256", "README-PACKAGE.txt",
                   "licenses/*", "allowed_signers"]
APPIMAGE_ALLOWED = ["AppRun", "kuf-setup.desktop", "kuf-setup.png", ".DirIcon", "usr/bin/kuf-setup",
                    "usr/share/kuf-setup/assets/*", "usr/share/kuf-setup/payload/allowed_signers",
                    "usr/share/kuf-setup/payload/requires.txt", "usr/share/kuf-setup/payload/package/*"]
GAME_DATA = ("*.xex", "*.xbe", "*.pak", "*.sox", "*.xwb", "*.xsb", "*.xgs", "*.wmv", "*.xma", "*.xpr", "*.bik",
             "*.iso", "*.stfs", "*.sav", "*.gpd", "*.xbx")
SECRETS = [(re.compile(rb"-----BEGIN [A-Z ]*PRIVATE KEY-----"), "a PEM private key"),
           (re.compile(rb"openssh-key-v1\0"), "an OpenSSH private key"),
           (re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{36}\b"), "a GitHub token"),
           (re.compile(rb"\bgithub_pat_[A-Za-z0-9_]{40,}"), "a GitHub token"),
           (re.compile(rb"\bAKIA[0-9A-Z]{16}\b"), "an AWS key"),
           (re.compile(rb"xox[baprs]-[A-Za-z0-9-]{10,}"), "a Slack token")]


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
    ap.add_argument("--signers", default=os.path.join(ROOT, "tools", "package", "allowed_signers"))
    ap.add_argument("--unsigned", action="store_true", help="before signing: no .sig files expected")
    ap.add_argument("--report")
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
    expected = {pkg, manifest, "SHA256SUMS", "notes.txt", "publish.txt", "audit.txt"}
    if not a.unsigned:
        expected |= {pkg + ".sig", manifest + ".sig"}
    if appimage in names:
        expected.add(appimage)
    for n in names:
        if n not in expected:
            problems.append(f"unexpected file in the release folder: {n}")
    for n in expected - {"audit.txt", "SHA256SUMS", "publish.txt"} - ({appimage} if appimage not in names else set()):
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
            lines.append(f"package: {len(files)} files")
            lines += [f"  {f}  {os.path.getsize(os.path.join(top, f))}" for f in files]
            pkg_manifest = open(os.path.join(top, "MANIFEST.sha256")).read()

        if appimage in names:
            img = os.path.join(tmp, "appimage")
            os.makedirs(img)
            r = subprocess.run([os.path.join(d, appimage), "--appimage-extract"], cwd=img, capture_output=True,
                               env=dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1"))
            root = os.path.join(img, "squashfs-root")
            if r.returncode != 0 or not os.path.isdir(root):
                problems.append(f"the AppImage could not be unpacked: {r.stderr[-200:]!r}")
            else:
                files = scan_tree(root, "AppImage", APPIMAGE_ALLOWED, problems, personal)
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
                lines.append(f"AppImage: {len(files)} files (its package identical to the release package)")

    sums = os.path.join(d, "SHA256SUMS")
    if os.path.isfile(sums):
        for line in open(sums):
            digest, name = line.split(None, 1)
            name = name.strip().lstrip("*")
            if not os.path.isfile(os.path.join(d, name)) or sha256(os.path.join(d, name)) != digest:
                problems.append(f"SHA256SUMS: {name} does not match")
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
