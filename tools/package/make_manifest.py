#!/usr/bin/env python3
"""Writes a release's update manifest (docs/RELEASING.md; read by tools/updater/kuf_update.py).

  python3 -I tools/package/make_manifest.py PACKAGE.tar.gz [--summary FILE] [--notes FILE] [--min-installed V]
                                            [--channel C]
                                            [--published YYYY-MM-DD] [--out FILE]

The package's file name gives the version and platform (kuf-cod-recomp-<version>-<platform>.tar.gz) and must agree
with the VERSION file inside it. Output (default: next to the package, <prefix>-<version>-<platform>.update.json):
schema, product, version, platform, channel, published, summary (the in-game update summary: what changed, plain text,
at most 1,000 characters; never installation instructions, which belong on the GitHub release page), notes (the
same summary for updaters older than 0.7.1, which show only notes; --notes alone: legacy), min_installed
(the oldest installed version this release can update; older installs are told to reinstall), requires (the
package's REQUIRES: the newest glibc and libstdc++ its binaries need and the libstdc++ it carries; the updater
refuses the update on a system that cannot run it) and the package's name, size and SHA-256. Sign it with tools/package/sign_package.sh --key KEY --manifest FILE.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import tarfile

PRODUCT = "kuf-cod-recomp"
NAME_RE = re.compile(r"^kuf-cod-recomp-(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)-(linux-x86_64)\.tar\.gz$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("package")
    ap.add_argument("--summary", help="a plain-text file: the in-game update summary (what changed)")
    ap.add_argument("--notes", help="legacy: notes only (without --summary)")
    ap.add_argument("--min-installed")
    ap.add_argument("--channel", default="stable", choices=["stable", "prerelease"])
    ap.add_argument("--published", default=datetime.date.today().isoformat())
    ap.add_argument("--out")
    a = ap.parse_args()
    name = os.path.basename(a.package)
    m = NAME_RE.match(name)
    if not m:
        sys.exit(f"{name}: not kuf-cod-recomp-<release version>-linux-x86_64.tar.gz (no build metadata)")
    version, platform = m.groups()
    with tarfile.open(a.package, "r:gz") as tar:
        vfile = tar.extractfile(f"{name[:-len('.tar.gz')]}/VERSION")
        inside = vfile.read().decode().strip() if vfile else None
        try:
            rfile = tar.extractfile(f"{name[:-len('.tar.gz')]}/REQUIRES")
        except KeyError:
            rfile = None
        requires = {}
        for line in (rfile.read().decode().splitlines() if rfile else []):
            key, _, value = line.partition(" ")
            if key not in ("glibc", "glibcxx", "glibcxx-bundled") or not re.fullmatch(r"\d+(\.\d+)+", value):
                sys.exit(f"{name}: REQUIRES line {line!r}")
            requires[key.replace("-", "_")] = value
    if inside != version:
        sys.exit(f"{name}: VERSION inside is {inside!r}, not {version!r}")
    if a.min_installed and not VERSION_RE.match(a.min_installed):
        sys.exit(f"--min-installed {a.min_installed!r} is not a release version")
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", a.published):
        sys.exit("--published must be YYYY-MM-DD")
    h = hashlib.sha256()
    with open(a.package, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    summary = open(a.summary, encoding="utf-8").read().strip() if a.summary else None
    if summary is not None and len(summary) > 1000:
        sys.exit("summary longer than 1,000 characters (the game shows it in its message box)")
    if summary is not None and "AppImage" in summary:
        sys.exit("the in-game summary must not tell players to run the setup (that belongs on the release page)")
    notes = summary if summary is not None else (open(a.notes, encoding="utf-8").read().strip() if a.notes else "")
    if len(notes) > 2000:
        sys.exit("notes longer than 2,000 characters (the game shows a short text)")
    manifest = {"schema": 1, "product": PRODUCT, "version": version, "platform": platform, "channel": a.channel,
                "published": a.published, "notes": notes,
                "package": {"name": name, "size": os.path.getsize(a.package), "sha256": h.hexdigest()}}
    if summary is not None:
        manifest["summary"] = summary
    if a.min_installed:
        manifest["min_installed"] = a.min_installed
    if requires:
        manifest["requires"] = requires
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.package)),
                                f"{PRODUCT}-{version}-{platform}.update.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
    print(out)


if __name__ == "__main__":
    main()
