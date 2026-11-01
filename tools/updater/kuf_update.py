#!/usr/bin/env python3
"""Update an installed KUF: Circle of Doom port from signed GitHub releases (stdlib only; Linux, Steam Deck).

  kuf_update.py --install-root DIR check            # is a newer release available? (exit 0 yes, 3 up to date)
  kuf_update.py --install-root DIR update [--yes]   # download, verify, install, switch
  kuf_update.py --install-root DIR rollback         # switch back to the previous installed version
  kuf_update.py --install-root DIR status
  kuf_update.py --install-root DIR install-package PACKAGE.tar.gz SIGNATURE   # a local signed package

Install root layout (tools/package/install.sh makes it):
  update.conf              repo = owner/name (the only source of releases), key_identity, asset_prefix; api (tests
                           only: http://127.0.0.1:<port>, a loopback release server, tools/updater/release_server.py)
  allowed_signers          the release public key(s), ssh-keygen allowed-signers format
  versions/<version>/      unpacked packages (each with VERSION and MANIFEST.sha256)
  current -> versions/<v>  the version the launcher starts
  previous                 the version switched away from last (for rollback)
  .first-start             written after an update: the launcher switches back if the new version fails to start
The player's saves, settings and logs are elsewhere (the launcher's data directory) and never touched.

A release (docs/RELEASING.md) has three signed assets: the package <prefix>-<v>-<platform>.tar.gz and its .sig, and
the update manifest <prefix>-<v>-<platform>.update.json and its .sig (version, platform, the package's name, size and
SHA-256, notes, the oldest installed version it can update).

Security (docs/STEAM_DECK.md "Updates"):
  - releases only from the repository in update.conf, over HTTPS with certificate verification; redirects only to
    HTTPS hosts of GitHub's (or, when update.conf sets a loopback api, only to that origin);
  - the newest release's manifest is verified (`ssh-keygen -Y verify`, namespace kuf-cod-recomp-manifest) before
    anything else is used; its version must be the release tag's and its platform the install's; the package
    download must have the manifest's size and SHA-256;
  - each package is verified with `ssh-keygen -Y verify` against allowed_signers (namespace kuf-cod-recomp-release)
    before it is opened; the package's own VERSION must equal the release's, and every file must match the
    package's MANIFEST.sha256;
  - no downgrade or reinstall of the running version unless asked (--allow-downgrade);
  - extraction refuses absolute paths, "..", links, devices and anything outside the package's one top directory,
    and limits sizes; it unpacks into a staging directory renamed into place; the switch is an atomic rename;
  - one updater at a time (a lock file).
Exit codes: 0 done / update available, 1 error (nothing changed), 2 usage, 3 up to date, 4 refused (verification),
5 cancelled (SIGTERM / SIGINT during an update: nothing changed).
With --json: one JSON line with the result; `update` also prints progress lines before it ({"progress": bytes,
"total": bytes} while downloading, {"stage": "verifying" | "installing"}); `check` reports the installed version,
the latest release's version, date and notes (plain text, at most 2,000 characters).
"""
import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import signal
import ssl
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

NAMESPACE = "kuf-cod-recomp-release"
MANIFEST_NAMESPACE = "kuf-cod-recomp-manifest"
MANIFEST_SCHEMA = 1
PRODUCT = "kuf-cod-recomp"
GITHUB_API = "https://api.github.com"
ALLOWED_HOSTS = ("api.github.com", "github.com", "objects.githubusercontent.com",
                 "release-assets.githubusercontent.com", "codeload.github.com")
LOOPBACK_HOSTS = ("127.0.0.1", "::1")
MAX_PACKAGE = 2 << 30          # 2 GiB download
MAX_UNPACKED = 4 << 30         # 4 GiB on disk
MAX_FILES = 10000
MAX_JSON = 8 << 20
MAX_MANIFEST = 64 << 10
MAX_SIGNATURE = 16 << 10
FIRST_START = ".first-start"
KEEP_VERSIONS = 3
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$")
REPO_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")


class Refused(Exception):
    """Verification failed: nothing was changed."""


class Cancelled(Exception):
    """The update was cancelled (a signal): nothing was changed."""


MAX_NOTES = 2000


def release_notes(rel):
    """A release's notes as plain text: control characters (except new lines) dropped, at most MAX_NOTES."""
    body = str(rel.get("body") or "")
    text = "".join(c for c in body.replace("\r\n", "\n") if c == "\n" or (c.isprintable() and c != "\x7f"))
    return text[:MAX_NOTES].strip()


class Failed(Exception):
    """An error that is not a verification failure (network, disk): nothing was changed."""


# ---- versions ----

def parse_version(text):
    m = VERSION_RE.match(text or "")
    if not m:
        raise Refused(f"not a release version: {text!r}")
    pre = m.group(4)
    # A release (no pre-release part) sorts after its pre-releases; pre-release parts compare numerically where they
    # are numbers (semver 2.0.0 precedence, build metadata not allowed).
    pre_key = (1,) if pre is None else (0,) + tuple((0, int(p), "") if p.isdigit() else (1, 0, p)
                                                   for p in pre.split("."))
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) + (pre_key,)


def version_of_tag(tag):
    return tag[1:] if tag.startswith("v") else tag


# ---- the install root ----

class Install:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.versions = os.path.join(self.root, "versions")
        self.current_link = os.path.join(self.root, "current")
        self.previous_file = os.path.join(self.root, "previous")
        self.signers = os.path.join(self.root, "allowed_signers")
        self.conf = self._read_conf()

    def _read_conf(self):
        path = os.path.join(self.root, "update.conf")
        conf = {}
        try:
            for line in open(path):
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    conf[k.strip()] = v.strip()
        except OSError as e:
            raise Failed(f"{path}: {e.strerror}")
        if not REPO_RE.match(conf.get("repo", "")):
            raise Failed(f"{path}: repo = owner/name not set (the release repository is the player's choice)")
        conf.setdefault("key_identity", "release@kuf-cod-recomp")
        conf.setdefault("asset_prefix", "kuf-cod-recomp")
        conf.setdefault("platform", "linux-x86_64")
        conf["api"] = conf.get("api", GITHUB_API).rstrip("/")
        if conf["api"] != GITHUB_API and _loopback_origin(conf["api"]) is None:
            raise Failed(f"{path}: api must be {GITHUB_API} or http://127.0.0.1:<port> (a local test server)")
        return conf

    def current(self):
        try:
            target = os.readlink(self.current_link)
        except OSError:
            return None
        return os.path.basename(target.rstrip("/"))

    def installed(self):
        try:
            return sorted((v for v in os.listdir(self.versions) if not v.startswith(".")), key=_sort_key)
        except OSError:
            return []

    def switch(self, version):
        """Points current at versions/<version> by an atomic rename; remembers the version switched away from."""
        target = os.path.join("versions", version)
        if not os.path.isfile(os.path.join(self.root, target, "VERSION")):
            raise Failed(f"versions/{version} is not an installed version")
        old = self.current()
        tmp = os.path.join(self.root, f".current.{os.getpid()}")
        if os.path.lexists(tmp):
            os.unlink(tmp)
        os.symlink(target, tmp)
        os.replace(tmp, self.current_link)
        if old and old != version:
            _write_atomic(self.previous_file, old + "\n")
        _fsync_dir(self.root)

    def prune(self):
        """Keeps the current and previous versions and the newest others up to KEEP_VERSIONS in all."""
        keep = {v for v in (self.current(), _read(self.previous_file)) if v}
        others = [v for v in self.installed() if v not in keep]  # oldest first
        for v in others[:max(0, len(others) - max(0, KEEP_VERSIONS - len(keep)))]:
            shutil.rmtree(os.path.join(self.versions, v), ignore_errors=True)
        for name in os.listdir(self.versions) if os.path.isdir(self.versions) else []:
            if name.startswith(".staging-"):  # an interrupted update
                shutil.rmtree(os.path.join(self.versions, name), ignore_errors=True)


def _sort_key(v):
    try:
        return (0, parse_version(v))
    except Refused:
        return (1, v)


def _read(path):
    try:
        return open(path).read().strip() or None
    except OSError:
        return None


def _write_atomic(path, text):
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "w") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def _fsync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


# ---- network (HTTPS to GitHub only; a loopback test server when update.conf names one) ----

def _loopback_origin(url):
    """(scheme, host, port) of an http URL on a loopback address with a port, else None (tests only)."""
    u = urllib.parse.urlsplit(url)
    try:
        port = u.port
    except ValueError:
        return None
    if u.scheme == "http" and u.hostname in LOOPBACK_HOSTS and port and not (u.username or u.password) \
            and u.path in ("", "/") and not (u.query or u.fragment):
        return ("http", u.hostname, port)
    return None


def _check_url(url, loopback=None):
    u = urllib.parse.urlsplit(url)
    try:
        port = u.port
    except ValueError:
        port = -1
    if loopback is not None and (u.scheme, u.hostname, port) == loopback and not (u.username or u.password):
        return url
    if u.scheme != "https" or u.hostname not in ALLOWED_HOSTS or u.username or u.password or port not in (None, 443):
        raise Refused(f"refused URL (only HTTPS to GitHub): {url}")
    return url


class _Redirects(urllib.request.HTTPRedirectHandler):
    def __init__(self, loopback):
        super().__init__()
        self.loopback = loopback

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        _check_url(newurl, self.loopback)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _opener(loopback=None):
    ctx = ssl.create_default_context()  # certificate and host name verification
    handlers = [urllib.request.HTTPSHandler(context=ctx), _Redirects(loopback)]
    if loopback is None:  # plain HTTP only to the configured loopback test server
        handlers.append(_NoHttp())
    return urllib.request.build_opener(*handlers)


class _NoHttp(urllib.request.HTTPHandler):
    def http_open(self, req):
        raise Refused(f"refused URL (only HTTPS to GitHub): {req.full_url}")


def _get(url, limit, out=None, accept="application/vnd.github+json", progress=None, loopback=None):
    req = urllib.request.Request(_check_url(url, loopback),
                                 headers={"Accept": accept, "User-Agent": "kuf-cod-recomp-updater"})
    try:
        with _opener(loopback).open(req, timeout=60) as r:
            _check_url(r.geturl(), loopback)
            total = 0
            data = bytearray()
            length = int(r.headers.get("Content-Length") or 0)
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                total += len(chunk)
                if total > limit:
                    raise Refused(f"{url}: larger than {limit} bytes")
                if progress:
                    progress(total, length)
                if out:
                    out.write(chunk)
                else:
                    data += chunk
            if length and total != length:  # the connection ended early: an interrupted download
                raise Failed(f"{url}: incomplete download ({total} of {length} bytes)")
            return bytes(data)
    except (Refused, Failed, Cancelled):
        raise
    except urllib.error.HTTPError as e:
        raise Failed(_http_problem(url, e.code, e.headers))
    except urllib.error.URLError as e:
        raise Failed(f"could not reach {urllib.parse.urlsplit(url).hostname} (no network connection?): {e.reason}")
    except TimeoutError:
        raise Failed(f"{urllib.parse.urlsplit(url).hostname} did not answer in time (try again later)")
    except (OSError, ValueError) as e:
        raise Failed(f"{url}: {e}")


def _http_problem(url, code, headers):
    """A player-readable reason for an HTTP error from GitHub (rate limit, missing repository or asset, outage)."""
    headers = headers or {}
    if code in (403, 429) and (headers.get("X-RateLimit-Remaining") == "0" or code == 429):
        when = ""
        try:
            when = " (after " + time.strftime("%H:%M", time.localtime(int(headers.get("X-RateLimit-Reset")))) + ")"
        except (TypeError, ValueError):
            if headers.get("Retry-After", "").isdigit():
                when = f" (in {int(headers['Retry-After']) // 60 + 1} minutes)"
        return f"GitHub's limit for update checks without an account is reached; try again later{when}"
    if code == 404:
        if "/download/" in url:
            return f"a release file is missing on GitHub (404): {url}"
        return f"the release repository was not found on GitHub (404): {url}"
    if 500 <= code < 600:
        return f"GitHub is not available right now (HTTP {code}); try again later"
    return f"GitHub answered HTTP {code} for {url}"


class GitHubSource:
    """GitHub's releases API (or the loopback test server update.conf's api names: the same requests)."""

    def __init__(self, repo, api=GITHUB_API):
        self.repo = repo
        self.api = api
        self.loopback = _loopback_origin(api) if api != GITHUB_API else None

    def releases(self):
        data = _get(f"{self.api}/repos/{self.repo}/releases?per_page=30", MAX_JSON, loopback=self.loopback)
        try:
            return json.loads(data)
        except ValueError as e:
            raise Failed(f"release list: {e}")

    def fetch(self, asset, limit):
        return _get(asset["browser_download_url"], limit, accept="application/octet-stream", loopback=self.loopback)

    def download(self, asset, dest, progress=None):
        with open(dest, "wb") as f:
            _get(asset["browser_download_url"], MAX_PACKAGE, out=f, accept="application/octet-stream",
                 progress=progress, loopback=self.loopback)


class DirectorySource:
    """Tests and offline use: releases.json and the assets in a local directory (--source-dir)."""

    def __init__(self, path, rate_kib=0):
        self.path = path
        self.rate = rate_kib * 1024

    def releases(self):
        return json.load(open(os.path.join(self.path, "releases.json")))

    def fetch(self, asset, limit):
        src = os.path.join(self.path, os.path.basename(asset["name"]))
        if os.path.getsize(src) > limit:
            raise Refused(f"{asset['name']}: larger than {limit} bytes")
        return open(src, "rb").read()

    def download(self, asset, dest, progress=None):
        src = os.path.join(self.path, os.path.basename(asset["name"]))
        size = os.path.getsize(src)
        if size > MAX_PACKAGE:
            raise Refused(f"{asset['name']}: larger than {MAX_PACKAGE} bytes")
        done = 0
        with open(src, "rb") as i, open(dest, "wb") as o:
            for chunk in iter(lambda: i.read(1 << 16), b""):
                o.write(chunk)
                done += len(chunk)
                if progress:
                    progress(done, size)
                if self.rate:
                    time.sleep(len(chunk) / self.rate)


# ---- choosing a release ----

def newest_release(install, source, allow_prerelease):
    """The newest release with a signed package and a signed update manifest:
    (key, version, package asset, its signature asset, release, manifest asset, its signature asset)."""
    best = None
    for rel in source.releases():
        if not isinstance(rel, dict) or rel.get("draft") or (rel.get("prerelease") and not allow_prerelease):
            continue
        version = version_of_tag(str(rel.get("tag_name", "")))
        try:
            key = parse_version(version)
        except Refused:
            continue
        base = f"{install.conf['asset_prefix']}-{version}-{install.conf['platform']}"
        name, manifest = base + ".tar.gz", base + ".update.json"
        assets = {a.get("name"): a for a in rel.get("assets", []) if isinstance(a, dict)}
        if any(n not in assets for n in (name, name + ".sig", manifest, manifest + ".sig")):
            continue
        if best is None or key > best[0]:
            best = (key, version, assets[name], assets[name + ".sig"], rel, assets[manifest], assets[manifest + ".sig"])
    return best


def read_manifest(install, source, best, tmp):
    """Downloads, verifies and checks the chosen release's update manifest; returns it (a dict)."""
    _, version, asset, _, _, m_asset, m_sig_asset = best
    path = os.path.join(tmp, os.path.basename(m_asset["name"]))
    with open(path, "wb") as f:
        f.write(source.fetch(m_asset, MAX_MANIFEST))
    with open(path + ".sig", "wb") as f:
        f.write(source.fetch(m_sig_asset, MAX_SIGNATURE))
    verify_signature(install, path, path + ".sig", MANIFEST_NAMESPACE)  # before it is parsed
    try:
        m = json.load(open(path, encoding="utf-8"))
    except ValueError as e:
        raise Refused(f"update manifest of {version}: not JSON ({e})")
    pkg = m.get("package") if isinstance(m, dict) else None
    if not isinstance(pkg, dict):
        raise Refused(f"update manifest of {version}: no package entry")
    problems = []
    if m.get("schema") != MANIFEST_SCHEMA:
        problems.append(f"schema {m.get('schema')!r} (this updater reads {MANIFEST_SCHEMA})")
    if m.get("product") != PRODUCT:
        problems.append(f"product {m.get('product')!r}")
    if m.get("version") != version:
        problems.append(f"version {m.get('version')!r} is not the release's {version!r}")
    if m.get("platform") != install.conf["platform"]:
        problems.append(f"platform {m.get('platform')!r} is not this install's {install.conf['platform']!r}")
    if pkg.get("name") != asset.get("name"):
        problems.append(f"package {pkg.get('name')!r} is not the release's {asset.get('name')!r}")
    size = pkg.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or not 0 < size <= MAX_PACKAGE:
        problems.append(f"package size {size!r}")
    if not (isinstance(pkg.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", pkg["sha256"])):
        problems.append("package sha256")
    if "min_installed" in m:
        try:
            parse_version(str(m["min_installed"]))
        except Refused:
            problems.append(f"min_installed {m['min_installed']!r}")
    if problems:
        raise Refused(f"update manifest of {version}: " + "; ".join(problems))
    return m


def check_compatible(manifest, current):
    """An update that needs a newer installed version than this one (a reinstall with the setup) is refused."""
    need = manifest.get("min_installed")
    if need and current and _sort_key(current) < (0, parse_version(str(need))):
        raise Refused(f"{manifest['version']} cannot update {current} (it needs {need} or later installed): "
                      "install it with the setup instead")


def verify_download(path, manifest):
    """The package file must have the size and SHA-256 its signed manifest names."""
    want = manifest["package"]
    size = os.path.getsize(path)
    if size != want["size"]:
        raise Refused(f"{want['name']}: {size} bytes, the manifest says {want['size']}")
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != want["sha256"]:
        raise Refused(f"{want['name']}: SHA-256 does not match the signed manifest")


# ---- verification and extraction ----

def verify_signature(install, package, signature, namespace=NAMESPACE):
    if not os.path.isfile(install.signers) or os.path.getsize(install.signers) == 0:
        raise Refused("no release key installed (allowed_signers)")
    with open(package, "rb") as data:
        r = subprocess.run(["ssh-keygen", "-Y", "verify", "-f", install.signers, "-I", install.conf["key_identity"],
                            "-n", namespace, "-s", signature], stdin=data, capture_output=True, text=True)
    if r.returncode != 0:
        raise Refused(f"signature not valid for {install.conf['key_identity']}: "
                      f"{(r.stderr or r.stdout).strip().splitlines()[-1:] or ['ssh-keygen failed']}")


def safe_extract(package, staging):
    """Unpacks the package's single top directory into staging; returns the top directory's name."""
    total, top = 0, None
    with tarfile.open(package, "r:gz") as tar:
        members = tar.getmembers()
        if len(members) > MAX_FILES:
            raise Refused(f"package has {len(members)} entries (limit {MAX_FILES})")
        for m in members:
            name = m.name
            parts = name.rstrip("/").split("/")
            if name.startswith("/") or "\\" in name or any(p in ("", ".", "..") for p in parts):
                raise Refused(f"package entry outside its directory: {name!r}")
            if not (m.isfile() or m.isdir()):
                raise Refused(f"package entry is not a file or directory: {name!r}")
            if m.mode & 0o7000:
                raise Refused(f"package entry with setuid/setgid/sticky bits: {name!r}")
            if top is None:
                top = parts[0]
            if parts[0] != top or top in (".", ""):
                raise Refused(f"package has more than one top directory: {name!r}")
            total += m.size
            if total > MAX_UNPACKED:
                raise Refused(f"package unpacks to more than {MAX_UNPACKED} bytes")
        if top is None:
            raise Refused("empty package")
        tar.extractall(staging, members=members, filter="data")
    return top


def verify_manifest(dir_, version):
    vfile = os.path.join(dir_, "VERSION")
    if _read(vfile) != version:
        raise Refused(f"package VERSION {_read(vfile)!r} is not the release's {version!r}")
    manifest = os.path.join(dir_, "MANIFEST.sha256")
    listed = {}
    try:
        for line in open(manifest):
            digest, name = line.rstrip("\n").split("  ", 1)
            listed[name] = digest
    except (OSError, ValueError):
        raise Refused("package MANIFEST.sha256 missing or malformed")
    found = set()
    for base, dirs, files in os.walk(dir_):
        for f in files:
            rel = os.path.relpath(os.path.join(base, f), dir_)
            if rel != "MANIFEST.sha256":
                found.add(rel)
    if found != set(listed):
        raise Refused(f"package files differ from its manifest: extra {sorted(found - set(listed))[:3]}, "
                      f"missing {sorted(set(listed) - found)[:3]}")
    for rel, digest in listed.items():
        h = hashlib.sha256()
        with open(os.path.join(dir_, rel), "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        if h.hexdigest() != digest:
            raise Refused(f"package file {rel} does not match its manifest")


def install_package(install, package, signature, version, allow_downgrade):
    """Verifies and unpacks a package into versions/<version>, then switches to it. Nothing changes on failure."""
    current = install.current()
    if current and not allow_downgrade:
        if parse_version(version) <= parse_version(current):
            raise Refused(f"{version} is not newer than the installed {current} (--allow-downgrade to force)")
    verify_signature(install, package, signature)  # before the archive is opened
    os.makedirs(install.versions, exist_ok=True)
    staging = tempfile.mkdtemp(prefix=".staging-", dir=install.versions)
    try:
        top = safe_extract(package, staging)
        unpacked = os.path.join(staging, top)
        verify_manifest(unpacked, version)
        for need in ("kuf-cod.sh", "kuf_update.py", "bin/kuf_cod"):  # what the launcher and the next update run
            p = os.path.join(unpacked, need)
            if not (os.path.isfile(p) and os.access(p, os.X_OK)):
                raise Refused(f"package has no executable {need}")
        final = os.path.join(install.versions, version)
        if os.path.exists(final):
            if current == version:
                raise Refused(f"{version} is the running version")
            shutil.rmtree(final)  # a leftover of the same version, not in use
        os.rename(unpacked, final)
        _fsync_dir(install.versions)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    install.switch(version)
    install.prune()


# ---- commands ----

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--install-root", required=True)
    ap.add_argument("--source-dir", help="read releases.json and assets from this directory (tests, offline)")
    ap.add_argument("--source-rate", type=int, default=0,
                    help="with --source-dir: copy at most this many KiB per second (to try progress and cancel)")
    ap.add_argument("--prerelease", action="store_true", help="also consider pre-releases")
    ap.add_argument("--allow-downgrade", action="store_true")
    ap.add_argument("--json", action="store_true", help="print the result as one JSON line (the game's check)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    up = sub.add_parser("update")
    up.add_argument("--yes", action="store_true", help="do not ask before installing")
    sub.add_parser("rollback")
    sub.add_parser("status")
    ip = sub.add_parser("install-package")
    ip.add_argument("package")
    ip.add_argument("signature")
    ip.add_argument("--version", help="the package's version (default: from its file name)")
    args = ap.parse_args(argv)

    def report(code, **fields):
        if args.json:
            print(json.dumps(dict(fields, exit=code)), flush=True)
        else:
            print(fields.get("message", ""))
        return code

    def on_signal(signum, frame):
        raise Cancelled(f"cancelled (signal {signum})")

    saved = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT)}
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    try:
        return _run(args, report)
    finally:  # the caller's handlers back (an ignored signal would be inherited by later child processes)
        for sig, handler in saved.items():
            signal.signal(sig, handler)


def _run(args, report):
    try:
        install = Install(args.install_root)
        os.makedirs(install.root, exist_ok=True)
        lock = open(os.path.join(install.root, ".update.lock"), "w")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise Failed("another updater is running")
        current = install.current()
        for name in os.listdir(install.root):  # downloads of an interrupted update (we hold the lock)
            if name.startswith(".download-"):
                shutil.rmtree(os.path.join(install.root, name), ignore_errors=True)
        if args.cmd == "status":
            return report(0, current=current, previous=_read(install.previous_file), installed=install.installed(),
                          repo=install.conf["repo"], message=f"current {current}, previous "
                          f"{_read(install.previous_file)}, installed {', '.join(install.installed()) or 'none'}")
        if args.cmd == "rollback":
            prev = _read(install.previous_file)
            if not prev or prev not in install.installed():
                raise Failed("no previous version to go back to")
            install.switch(prev)
            try:
                os.unlink(os.path.join(install.root, FIRST_START))
            except FileNotFoundError:
                pass
            return report(0, current=prev, message=f"switched back to {prev}")
        if args.cmd == "install-package":
            version = args.version or re.sub(rf"^{re.escape(install.conf['asset_prefix'])}-(.+)-"
                                             rf"{re.escape(install.conf['platform'])}\.tar\.gz$", r"\1",
                                             os.path.basename(args.package))
            parse_version(version)
            install_package(install, args.package, args.signature, version, args.allow_downgrade)
            return report(0, current=version, message=f"installed {version}")
        if args.source_dir:
            source = DirectorySource(args.source_dir, args.source_rate)
        else:
            source = GitHubSource(install.conf["repo"], install.conf["api"])
        allow_prerelease = args.prerelease or install.conf.get("channel") == "prerelease"
        best = newest_release(install, source, allow_prerelease)
        if best is None:
            return report(3, current=current, latest=None, message="no signed release found")
        _, version, asset, sig_asset, rel = best[:5]
        with tempfile.TemporaryDirectory(prefix=".download-", dir=install.root) as tmp:
            manifest = read_manifest(install, source, best, tmp)  # signed: nothing unverified is shown or used
            newer = current is None or parse_version(version) > parse_version(current)
            if newer:
                check_compatible(manifest, current)
            if args.cmd == "check" or not newer:
                notes = release_notes({"body": manifest.get("notes", "")})
                return report(0 if newer else 3, current=current, latest=version, update=newer,
                              published=str(manifest.get("published") or "")[:10], notes=notes,
                              size=manifest["package"]["size"],
                              message=(f"update available: {version} (installed {current})" if newer
                                       else f"up to date ({current})"))
            if not args.yes:
                if not sys.stdin.isatty() or input(f"Install {version} (installed {current})? [y/N] ").lower() != "y":
                    return report(1, message="not installed")
            package = os.path.join(tmp, asset["name"])
            signature = package + ".sig"
            size = manifest["package"]["size"]
            last = [-1]

            def progress(done, total):
                if done > size:
                    raise Refused(f"{asset['name']}: larger than the manifest's {size} bytes")
                percent = done * 100 // size
                if args.json and percent != last[0]:
                    last[0] = percent
                    print(json.dumps({"progress": done, "total": size}), flush=True)

            with open(signature, "wb") as f:
                f.write(source.fetch(sig_asset, MAX_SIGNATURE))
            source.download(asset, package, progress)
            if args.json:
                print(json.dumps({"stage": "verifying"}), flush=True)
            verify_download(package, manifest)
            # From here on the switch is atomic: a signal no longer cancels (the install completes or changes nothing).
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            if args.json:
                print(json.dumps({"stage": "installing"}), flush=True)
            install_package(install, package, signature, version, args.allow_downgrade)
            # The launcher's first start of the new version: if it fails to start, the launcher switches back.
            _write_atomic(os.path.join(install.root, FIRST_START), f"{version}\n{current or ''}\n")
        return report(0, current=version, previous=current, message=f"updated {current} -> {version}")
    except Cancelled as e:
        return report(5, error=str(e), message=str(e))
    except Refused as e:
        return report(4, error=str(e), message=f"refused: {e}")
    except Failed as e:
        return report(1, error=str(e), message=f"error: {e}")


if __name__ == "__main__":
    sys.exit(main())
