#!/usr/bin/env python3
"""Updater, packaging and launcher tests (tools/updater/kuf_update.py, tools/package/*; docs/STEAM_DECK.md).

Synthetic packages signed with throw-away ed25519 keys (ssh-keygen), releases served from a local directory
(--source-dir): no network, nothing outside a temporary directory. Each case prints one line; exit 1 on a failure.
"""
import fcntl
import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "updater"))
import kuf_update  # noqa: E402

NS = kuf_update.NAMESPACE
IDENTITY = "release@kuf-cod-recomp"
results = []


def case(name):
    def wrap(fn):
        def run(t):
            try:
                fn(t)
                results.append((True, name, ""))
            except AssertionError as e:
                results.append((False, name, str(e)))
            except Exception as e:  # noqa: BLE001
                results.append((False, name, f"{type(e).__name__}: {e}"))
        run.__name__ = name
        CASES.append(run)
        return fn
    return wrap


CASES = []


class Env:
    def __init__(self, tmp):
        os.makedirs(tmp, exist_ok=True)
        self.tmp = tmp
        self.key = self.make_key("good")
        self.other = self.make_key("other")
        self.root = os.path.join(tmp, "app")
        self.src = os.path.join(tmp, "releases")
        os.makedirs(self.src)
        os.makedirs(self.root)
        with open(os.path.join(self.root, "update.conf"), "w") as f:
            f.write("repo = player/kuf-cod-recomp\nkey_identity = release@kuf-cod-recomp\n")
        with open(os.path.join(self.root, "allowed_signers"), "w") as f:
            f.write(self.signers_line(self.key))
        self.releases = []

    def make_key(self, name):
        path = os.path.join(self.tmp, f"key-{name}")
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", IDENTITY, "-f", path], check=True)
        return path

    def signers_line(self, key):
        pub = open(key + ".pub").read().split()
        return f'{IDENTITY} namespaces="{NS}" {pub[0]} {pub[1]}\n'

    def package(self, version, files=None, top=None, vfile=None, manifest_skip=False, extra_members=(), name=None):
        """A package tarball like package_linux.sh makes; returns its path."""
        top = top or f"kuf-cod-recomp-{version}-linux-x86_64"
        files = dict(files or {"bin/kuf_cod": b"#!/bin/sh\necho game\n", "kuf-cod.sh": b"#!/bin/sh\n"})
        files["VERSION"] = (vfile if vfile is not None else version + "\n").encode()
        lines = "".join(f"{hashlib.sha256(d).hexdigest()}  {n}\n" for n, d in sorted(files.items())
                        if not (manifest_skip and n == "bin/kuf_cod"))
        files["MANIFEST.sha256"] = lines.encode()
        path = os.path.join(self.src, name or f"kuf-cod-recomp-{version}-linux-x86_64.tar.gz")
        with tarfile.open(path, "w:gz") as tar:
            for n, d in sorted(files.items()):
                info = tarfile.TarInfo(f"{top}/{n}")
                info.size = len(d)
                info.mode = 0o755
                tar.addfile(info, io.BytesIO(d))
            for info, data in extra_members:
                tar.addfile(info, io.BytesIO(data) if data is not None else None)
        return path

    def sign(self, path, key=None, namespace=NS):
        if os.path.exists(path + ".sig"):
            os.unlink(path + ".sig")
        subprocess.run(["ssh-keygen", "-q", "-Y", "sign", "-f", key or self.key, "-n", namespace, path], check=True,
                       capture_output=True)
        return path + ".sig"

    def release(self, version, path, prerelease=False, draft=False, sig=True):
        assets = [{"name": os.path.basename(path), "size": os.path.getsize(path),
                   "browser_download_url": "https://github.com/x/y/" + os.path.basename(path)}]
        if sig:
            assets.append({"name": os.path.basename(path) + ".sig", "size": 1,
                           "browser_download_url": "https://github.com/x/y/s"})
        self.releases.append({"tag_name": "v" + version, "draft": draft, "prerelease": prerelease, "assets": assets})
        json.dump(self.releases, open(os.path.join(self.src, "releases.json"), "w"))

    def run(self, *args):
        out = io.StringIO()
        old = sys.stdout
        sys.stdout = out
        try:
            code = kuf_update.main(["--install-root", self.root, "--source-dir", self.src, "--json", *args])
        finally:
            sys.stdout = old
        return code, json.loads(out.getvalue().strip().splitlines()[-1])

    def current(self):
        return kuf_update.Install(self.root).current()

    def publish(self, version, **kw):
        p = self.package(version, **kw)
        self.sign(p)
        self.release(version, p)
        return p


def snapshot(path):
    """Every file under path with its content hash (to prove nothing changed)."""
    out = {}
    for base, dirs, files in os.walk(path):
        for f in files:
            if f == ".update.lock":
                continue
            p = os.path.join(base, f)
            out[os.path.relpath(p, path)] = hashlib.sha256(open(p, "rb").read()).hexdigest()
        for d in dirs:
            out[os.path.relpath(os.path.join(base, d), path) + "/"] = ""
    for link in ("current", "previous"):
        p = os.path.join(path, link)
        if os.path.islink(p):
            out[link + "@"] = os.readlink(p)
    return out


@case("a newer signed release installs and becomes current")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    assert e.run("update", "--yes")[0] == 0 and e.current() == "1.0.0", "first install"
    e.publish("1.1.0")
    code, r = e.run("check")
    assert code == 0 and r["latest"] == "1.1.0" and r["update"], f"check {code} {r}"
    code, r = e.run("update", "--yes")
    assert code == 0 and e.current() == "1.1.0" and r["previous"] == "1.0.0", f"update {code} {r}"
    assert os.path.isfile(os.path.join(e.root, "current", "bin", "kuf_cod")), "files reachable through current"


@case("up to date: the same version is not reinstalled (exit 3)")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    before = snapshot(e.root)
    code, r = e.run("update", "--yes")
    assert code == 3 and not r["update"], f"{code} {r}"
    assert snapshot(e.root) == before, "nothing changed"


@case("a package signed by another key is refused, nothing changes")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("1.1.0")
    e.sign(p, key=e.other)
    e.release("1.1.0", p)
    before = snapshot(e.root)
    code, r = e.run("update", "--yes")
    assert code == 4 and "signature" in r["error"], f"{code} {r}"
    assert snapshot(e.root) == before and e.current() == "1.0.0", "unchanged"


@case("a signature for another namespace (e.g. a git commit signature) is refused")
def _(t):
    e = Env(t)
    p = e.package("1.0.0")
    e.sign(p, namespace="git")
    e.release("1.0.0", p)
    code, r = e.run("update", "--yes")
    assert code == 4 and e.current() is None, f"{code} {r}"


@case("a package changed after signing is refused")
def _(t):
    e = Env(t)
    p = e.publish("1.0.0")
    with open(p, "ab") as f:
        f.write(b"\0")
    code, r = e.run("update", "--yes")
    assert code == 4 and e.current() is None, f"{code} {r}"


@case("a release without a signature is ignored")
def _(t):
    e = Env(t)
    p = e.package("1.0.0")
    e.release("1.0.0", p, sig=False)
    code, r = e.run("check")
    assert code == 3 and r["latest"] is None, f"{code} {r}"


@case("no release key installed: refused")
def _(t):
    e = Env(t)
    open(os.path.join(e.root, "allowed_signers"), "w").close()
    e.publish("1.0.0")
    code, r = e.run("update", "--yes")
    assert code == 4 and "no release key" in r["error"], f"{code} {r}"


@case("downgrade refused (an old signed package replayed under a newer tag is caught by VERSION)")
def _(t):
    e = Env(t)
    e.publish("1.1.0")
    e.run("update", "--yes")
    old = e.package("1.0.0")
    e.sign(old)
    code, r = e.run("install-package", old, old + ".sig")
    assert code == 4 and "not newer" in r["error"], f"downgrade {code} {r}"
    # The 1.0.0 package (VERSION 1.0.0) renamed and published as 1.2.0.
    replay = os.path.join(e.src, "kuf-cod-recomp-1.2.0-linux-x86_64.tar.gz")
    os.rename(old, replay)
    os.rename(old + ".sig", replay + ".sig")
    e.release("1.2.0", replay)
    code, r = e.run("update", "--yes")
    assert code == 4 and "VERSION" in r["error"] and e.current() == "1.1.0", f"replay {code} {r}"


@case("path traversal, absolute paths, links, devices and a second top directory are refused")
def _(t):
    e = Env(t)
    e.publish("0.9.0")
    e.run("update", "--yes")  # an existing install (the updater's normal starting point)
    bad = []
    i = tarfile.TarInfo("../evil")
    i.size = 1
    bad.append(("dotdot", [(i, b"x")]))
    i = tarfile.TarInfo("/tmp/evil")
    i.size = 1
    bad.append(("absolute", [(i, b"x")]))
    i = tarfile.TarInfo("kuf-cod-recomp-1.0.0-linux-x86_64/link")
    i.type = tarfile.SYMTYPE
    i.linkname = "/etc/passwd"
    bad.append(("symlink", [(i, None)]))
    i = tarfile.TarInfo("kuf-cod-recomp-1.0.0-linux-x86_64/hard")
    i.type = tarfile.LNKTYPE
    i.linkname = "kuf-cod-recomp-1.0.0-linux-x86_64/VERSION"
    bad.append(("hardlink", [(i, None)]))
    i = tarfile.TarInfo("kuf-cod-recomp-1.0.0-linux-x86_64/dev")
    i.type = tarfile.CHRTYPE
    bad.append(("device", [(i, None)]))
    i = tarfile.TarInfo("other-top/file")
    i.size = 1
    bad.append(("second top", [(i, b"x")]))
    i = tarfile.TarInfo("kuf-cod-recomp-1.0.0-linux-x86_64/a/../../x")
    i.size = 1
    bad.append(("inner dotdot", [(i, b"x")]))
    i = tarfile.TarInfo("kuf-cod-recomp-1.0.0-linux-x86_64/suid")
    i.size = 1
    i.mode = 0o4755
    bad.append(("setuid", [(i, b"x")]))
    outside = os.path.join(t, "evil")
    for label, members in bad:
        p = e.package("1.0.0", extra_members=members)
        e.sign(p)
        before = snapshot(e.root)
        code, r = e.run("install-package", p, p + ".sig")
        assert code == 4, f"{label}: {code} {r}"
        assert snapshot(e.root) == before, f"{label}: install root changed"
        assert not os.path.exists(outside) and not os.path.exists("/tmp/evil-kuf-test"), f"{label}: wrote outside"


@case("files not matching the package manifest are refused")
def _(t):
    e = Env(t)
    p = e.package("1.0.0", manifest_skip=True)
    e.sign(p)
    code, r = e.run("install-package", p, p + ".sig")
    assert code == 4 and "manifest" in r["error"], f"{code} {r}"


@case("rollback switches to the previous version; saves outside the install are untouched")
def _(t):
    e = Env(t)
    data = os.path.join(t, "data", "saves")
    os.makedirs(data)
    with open(os.path.join(data, "save"), "w") as f:
        f.write("progress")
    e.publish("1.0.0")
    e.run("update", "--yes")
    e.publish("1.1.0")
    e.run("update", "--yes")
    code, r = e.run("rollback")
    assert code == 0 and e.current() == "1.0.0", f"{code} {r}"
    assert open(os.path.join(data, "save")).read() == "progress", "save untouched"


@case("an interrupted update leaves the installed version running; the next run cleans up")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    stale = os.path.join(e.root, "versions", ".staging-old")
    os.makedirs(os.path.join(stale, "x"))
    assert e.current() == "1.0.0"
    e.publish("1.1.0")
    e.run("update", "--yes")
    assert not os.path.exists(stale) and e.current() == "1.1.0", "staging removed after the next update"


@case("old versions are pruned to the current, the previous and one more")
def _(t):
    e = Env(t)
    for v in ("1.0.0", "1.1.0", "1.2.0", "1.3.0", "1.4.0"):
        e.publish(v)
        e.run("update", "--yes")
    assert kuf_update.Install(e.root).installed() == ["1.2.0", "1.3.0", "1.4.0"], \
        kuf_update.Install(e.root).installed()


@case("pre-releases and drafts only when asked")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    p = e.package("1.1.0-beta.1")
    e.sign(p)
    e.release("1.1.0-beta.1", p, prerelease=True)
    code, r = e.run("check")
    assert r["latest"] == "1.0.0", r
    code, r = e.run("--prerelease", "check")
    assert r["latest"] == "1.1.0-beta.1", r
    p = e.package("1.2.0")
    e.sign(p)
    e.release("1.2.0", p, draft=True)
    code, r = e.run("--prerelease", "check")
    assert r["latest"] == "1.1.0-beta.1", f"draft ignored: {r}"
    assert parse_order(), "pre-release ordering"


def parse_order():
    pv = kuf_update.parse_version
    return pv("1.0.0-beta.2") < pv("1.0.0-beta.10") < pv("1.0.0-rc.1") < pv("1.0.0") < pv("1.0.1")


@case("only HTTPS to GitHub hosts; redirects elsewhere refused")
def _(t):
    ok = ["https://api.github.com/repos/a/b/releases", "https://objects.githubusercontent.com/x"]
    bad = ["http://api.github.com/repos/a/b/releases", "https://evil.example/x", "https://api.github.com.evil.example/",
           "https://user:pw@github.com/x", "https://github.com:8443/x", "file:///etc/passwd", "ftp://github.com/x"]
    for u in ok:
        kuf_update._check_url(u)
    for u in bad:
        try:
            kuf_update._check_url(u)
        except kuf_update.Refused:
            continue
        raise AssertionError(f"accepted {u}")


@case("the repository must be configured; one updater at a time")
def _(t):
    e = Env(t)
    with open(os.path.join(e.root, "update.conf"), "w") as f:
        f.write("repo = \n")
    code, r = e.run("check")
    assert code == 1 and "repo" in r["error"], f"{code} {r}"
    e = Env(os.path.join(t, "second"))
    lock = open(os.path.join(e.root, ".update.lock"), "w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    code, r = e.run("check")
    assert code == 1 and "another updater" in r["error"], f"{code} {r}"


@case("package_linux.sh: one top directory, VERSION, a manifest of every file, no game data")
def _(t):
    build = os.path.join(t, "build")
    os.makedirs(os.path.join(build, "bin"))
    os.makedirs(os.path.join(build, "generated_include"))
    for f in ("kuf_cod", "librexruntimerd.so", "librexgpu-xenosrd.so", "libTracyClientrd.so"):
        with open(os.path.join(build, "bin", f), "w") as h:
            h.write(f)
    with open(os.path.join(build, "generated_include", "kuf_version_generated.h"), "w") as h:
        h.write('#define KUFCOD_VERSION "1.2.3"\n')
    out = os.path.join(t, "out")
    r = subprocess.run([os.path.join(ROOT, "tools", "package", "package_linux.sh"), "--build", build, "--out", out],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    pkg = r.stdout.strip().splitlines()[-1]
    assert os.path.basename(pkg) == "kuf-cod-recomp-1.2.3-linux-x86_64.tar.gz", pkg
    with tarfile.open(pkg) as tar:
        names = tar.getnames()
    assert {n.split("/")[0] for n in names} == {"kuf-cod-recomp-1.2.3-linux-x86_64"}, names
    rel = {n.split("/", 1)[1] for n in names if "/" in n}
    for need in ("bin/kuf_cod", "kuf-cod.sh", "install.sh", "kuf_update.py", "VERSION", "MANIFEST.sha256"):
        assert need in rel, f"missing {need}"
    assert not any(x.endswith((".xex", ".pak", ".sox", ".xwb", ".wmv")) or "generated" in x for x in rel), rel
    # The real updater accepts it once signed.
    e = Env(os.path.join(t, "e"))
    os.rename(pkg, os.path.join(e.src, os.path.basename(pkg)))
    p = os.path.join(e.src, os.path.basename(pkg))
    e.sign(p)
    e.release("1.2.3", p)
    code, res = e.run("update", "--yes")
    assert code == 0 and e.current() == "1.2.3", f"{code} {res}"
    # install.sh from the unpacked package: a second install root, the stable launcher written.
    unpack = os.path.join(t, "unpack")
    with tarfile.open(p) as tar:
        tar.extractall(unpack, filter="data")
    root2 = os.path.join(t, "app2")
    r = subprocess.run([os.path.join(unpack, "kuf-cod-recomp-1.2.3-linux-x86_64", "install.sh"), "--root", root2],
                       capture_output=True, text=True)
    assert r.returncode == 0 and os.readlink(os.path.join(root2, "current")) == "versions/1.2.3", r.stderr
    assert os.access(os.path.join(root2, "kuf-cod"), os.X_OK), "launcher"


@case("launcher: asks for the game folder, remembers it, passes data paths, never inside the game folder")
def _(t):
    pkg = os.path.join(t, "pkg")
    os.makedirs(os.path.join(pkg, "bin"))
    launcher = os.path.join(pkg, "kuf-cod.sh")
    with open(launcher, "w") as f:
        f.write(open(os.path.join(ROOT, "tools", "package", "kuf-cod.sh")).read())
    os.chmod(launcher, 0o755)
    fake = os.path.join(pkg, "bin", "kuf_cod")
    with open(fake, "w") as f:
        f.write('#!/bin/sh\nfor a in "$@"; do echo "$a"; done\n')
    os.chmod(fake, 0o755)
    game = os.path.join(t, "game")
    os.makedirs(game)
    open(os.path.join(game, "default.xex"), "w").close()
    # No display and no dialogs: the test must never open a window on the desktop it runs on.
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS")}
    env.update(HOME=os.path.join(t, "home"), XDG_CONFIG_HOME=os.path.join(t, "cfg"),
               XDG_DATA_HOME=os.path.join(t, "data"), PATH="/usr/bin:/bin", KUF_NO_DIALOG="1")
    r = subprocess.run([launcher], env=env, capture_output=True, text=True, stdin=subprocess.DEVNULL)
    assert r.returncode == 1 and "game folder" in r.stderr, f"no folder: {r.returncode} {r.stderr}"
    r = subprocess.run([launcher, "--game-dir", game, "--", "--fullscreen=false"], env=env, capture_output=True,
                       text=True)
    args = r.stdout.splitlines()
    assert r.returncode == 0 and f"--game_data_root={game}" in args and "--fullscreen=false" in args, args
    data = os.path.join(t, "data", "kuf-cod-recomp", "data")
    assert f"--content_root={data}/saves" in args and f"--user_data_root={data}/user" in args, args
    r = subprocess.run([launcher], env=env, capture_output=True, text=True)
    assert f"--game_data_root={game}" in r.stdout.splitlines(), "remembered"
    assert os.listdir(game) == ["default.xex"], "nothing written into the game folder"


@case("check reports the installed version, the release's date and its notes as plain text")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    e.publish("1.2.0")
    e.releases[-1].update(published_at="2026-11-02T10:00:00Z", body="Fixes:\r\n- menus\x1b[31m red\x07\n" + "x" * 3000)
    json.dump(e.releases, open(os.path.join(e.src, "releases.json"), "w"))
    code, r = e.run("check")
    assert code == 0 and r["current"] == "1.0.0" and r["latest"] == "1.2.0", f"{code} {r}"
    assert r["published"] == "2026-11-02", r["published"]
    notes = r["notes"]
    assert notes.startswith("Fixes:\n- menus") and "\x1b" not in notes and "\x07" not in notes and "\r" not in notes, notes[:40]
    assert len(notes) <= kuf_update.MAX_NOTES, len(notes)


@case("update prints download progress lines, then the result")
def _(t):
    e = Env(t)
    e.publish("1.0.0", files={"bin/kuf_cod": os.urandom(300_000), "kuf-cod.sh": b"#!/bin/sh\n"})
    out = io.StringIO()
    old = sys.stdout
    sys.stdout = out
    try:
        code = kuf_update.main(["--install-root", e.root, "--source-dir", e.src, "--json", "update", "--yes"])
    finally:
        sys.stdout = old
    lines = [json.loads(x) for x in out.getvalue().strip().splitlines()]
    progress = [x for x in lines if "progress" in x]
    assert code == 0 and lines[-1]["exit"] == 0, lines[-1]
    assert len(progress) >= 2 and progress[-1]["progress"] == progress[-1]["total"] > 0, progress[-3:]
    assert all(a["progress"] <= b["progress"] for a, b in zip(progress, progress[1:])), "monotonic"
    assert {"stage": "verifying"} in lines, "verifying stage reported"


@case("a cancelled download (signal) changes nothing and leaves no download behind (exit 5)")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    e.publish("1.1.0")
    before = snapshot(e.root)
    original = kuf_update.DirectorySource.download

    def cancelled(self, asset, dest, progress=None):
        if asset["name"].endswith(".tar.gz"):
            open(dest, "wb").write(b"partial")
            raise kuf_update.Cancelled("cancelled (signal 15)")
        return original(self, asset, dest, progress)

    kuf_update.DirectorySource.download = cancelled
    try:
        code, r = e.run("update", "--yes")
    finally:
        kuf_update.DirectorySource.download = original
    assert code == 5 and "cancelled" in r["error"], f"{code} {r}"
    assert snapshot(e.root) == before and e.current() == "1.0.0", "nothing changed"


@case("leftovers of an interrupted update are removed at the next run")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    os.makedirs(os.path.join(e.root, ".download-killed"))
    open(os.path.join(e.root, ".download-killed", "part.tar.gz"), "wb").write(b"x" * 100)
    os.makedirs(os.path.join(e.root, "versions", ".staging-killed"))
    e.publish("1.1.0")
    code, _ = e.run("update", "--yes")
    names = os.listdir(e.root) + os.listdir(os.path.join(e.root, "versions"))
    assert code == 0 and e.current() == "1.1.0", code
    assert not any(n.startswith((".download-", ".staging-")) for n in names), names


@case("launcher: Restart after an update starts the version now current, once")
def _(t):
    root = os.path.join(t, "app")
    for v in ("1.0.0", "1.1.0"):
        d = os.path.join(root, "versions", v)
        os.makedirs(os.path.join(d, "bin"))
        with open(os.path.join(d, "kuf-cod.sh"), "w") as f:
            f.write(open(os.path.join(ROOT, "tools", "package", "kuf-cod.sh")).read())
        os.chmod(os.path.join(d, "kuf-cod.sh"), 0o755)
        with open(os.path.join(d, "bin", "kuf_cod"), "w") as f:
            # 1.0.0 "installs" 1.1.0 and asks for a restart; 1.1.0 just runs.
            restart = (f'ln -sfn versions/1.1.0 "{root}/current.new" && mv -T "{root}/current.new" "{root}/current"'
                       ' && touch "$KUF_RESTART_FILE"\n') if v == "1.0.0" else ""
            f.write(f'#!/bin/sh\necho {v} >> "{t}/runs"\n{restart}exit 0\n')
        os.chmod(os.path.join(d, "bin", "kuf_cod"), 0o755)
    os.symlink("versions/1.0.0", os.path.join(root, "current"))
    with open(os.path.join(root, "kuf-cod"), "w") as f:
        f.write(f'#!/bin/sh\nexec "{root}/current/kuf-cod.sh" "$@"\n')
    with open(os.path.join(root, "kuf-update"), "w") as f:
        f.write("#!/bin/sh\nexit 3\n")
    os.chmod(os.path.join(root, "kuf-cod"), 0o755)
    os.chmod(os.path.join(root, "kuf-update"), 0o755)
    game = os.path.join(t, "game")
    os.makedirs(game)
    open(os.path.join(game, "default.xex"), "w").close()
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS")}
    env.update(HOME=os.path.join(t, "home"), XDG_CONFIG_HOME=os.path.join(t, "cfg"),
               XDG_DATA_HOME=os.path.join(t, "data"), PATH="/usr/bin:/bin", KUF_NO_DIALOG="1")
    r = subprocess.run([os.path.join(root, "kuf-cod"), "--game-dir", game], env=env, capture_output=True, text=True,
                       timeout=60)
    runs = open(os.path.join(t, "runs")).read().split()
    assert r.returncode == 0 and runs == ["1.0.0", "1.1.0"], f"{r.returncode} {runs} {r.stderr}"
    marker = os.path.join(t, "data", "kuf-cod-recomp", "data", "user", ".restart-requested")
    assert not os.path.exists(marker), "the restart marker is removed"


def main():
    # Nothing from this test may reach the desktop session it runs in.
    for k in ("DISPLAY", "WAYLAND_DISPLAY"):
        os.environ.pop(k, None)
    os.environ["KUF_NO_DIALOG"] = "1"
    with tempfile.TemporaryDirectory(prefix="kuf-updater-test-") as top:
        for i, fn in enumerate(CASES):
            d = os.path.join(top, str(i))
            os.makedirs(d)
            fn(d)
    for ok, name, why in results:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}{': ' + why if why else ''}")
    failed = sum(not ok for ok, _, _ in results)
    print(f"UPDATER TESTS: {len(results) - failed} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
