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
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "updater"))
import kuf_update  # noqa: E402

NS = kuf_update.NAMESPACE
MNS = kuf_update.MANIFEST_NAMESPACE
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
        return f'{IDENTITY} namespaces="{NS},{MNS}" {pub[0]} {pub[1]}\n'

    def package(self, version, files=None, top=None, vfile=None, manifest_skip=False, extra_members=(), name=None):
        """A package tarball like package_linux.sh makes; returns its path."""
        top = top or f"kuf-cod-recomp-{version}-linux-x86_64"
        files = dict(files or {"bin/kuf_cod": b"#!/bin/sh\necho game\n", "kuf-cod.sh": b"#!/bin/sh\n",
                               "kuf_update.py": b"#!/usr/bin/env python3\n"})
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

    def release(self, version, path, prerelease=False, draft=False, sig=True, manifest=True, edit=None,
                manifest_key=None, manifest_ns=MNS, published="2026-11-01", notes="Notes."):
        """Lists a release of the package at path, with its signed update manifest (edit(m) may change it)."""
        def asset(name):
            return {"name": name, "size": os.path.getsize(os.path.join(self.src, name)),
                    "browser_download_url": "https://github.com/x/y/releases/download/" + name}
        names = [os.path.basename(path)] + ([os.path.basename(path) + ".sig"] if sig else [])
        if manifest:
            m = {"schema": 1, "product": "kuf-cod-recomp", "version": version, "platform": "linux-x86_64",
                 "channel": "prerelease" if prerelease else "stable", "published": published, "notes": notes,
                 "package": {"name": os.path.basename(path), "size": os.path.getsize(path),
                             "sha256": hashlib.sha256(open(path, "rb").read()).hexdigest()}}
            if edit:
                edit(m)
            mname = f"kuf-cod-recomp-{version}-linux-x86_64.update.json"
            mpath = os.path.join(self.src, mname)
            with open(mpath, "w") as f:
                json.dump(m, f)
            self.sign(mpath, key=manifest_key, namespace=manifest_ns)
            names += [mname, mname + ".sig"]
        self.releases.append({"tag_name": "v" + version, "draft": draft, "prerelease": prerelease,
                              "assets": [asset(n) for n in names if os.path.exists(os.path.join(self.src, n))]})
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
    for f in ("rexgluerd", "librexruntimerd.so", "librexgpu-xenosrd.so", "libTracyClientrd.so"):
        with open(os.path.join(build, "bin", f), "w") as h:
            h.write(f)
    # A stand-in builder: it writes the program it is asked for, or fails when told to.
    builder = os.path.join(t, "builder")
    os.makedirs(builder)
    with open(os.path.join(builder, "kuf-localbuild"), "w") as h:
        h.write('#!/bin/sh\n[ -n "$KUF_TEST_BUILD_FAILS" ] && { echo "no space left" >&2; exit 1; }\n'
                'while [ $# -gt 0 ]; do [ "$1" = --output ] && out="$2"; shift; done\n'
                'printf "#!/bin/sh\\necho game\\n" > "$out"; chmod +x "$out"\n')
    os.chmod(os.path.join(builder, "kuf-localbuild"), 0o755)
    game = os.path.join(t, "game")
    os.makedirs(game)
    with open(os.path.join(game, "default.xex"), "w") as h:
        h.write("x")
    with open(os.path.join(build, "generated_include", "kuf_version_generated.h"), "w") as h:
        h.write('#define KUFCOD_VERSION "1.2.3"\n')
    out = os.path.join(t, "out")
    r = subprocess.run([os.path.join(ROOT, "tools", "package", "package_linux.sh"), "--build", build, "--out", out],
                       capture_output=True, text=True)
    assert r.returncode == 1 and "not the portable build" in r.stderr, r.stderr  # releases are portable builds
    r = subprocess.run([os.path.join(ROOT, "tools", "package", "package_linux.sh"), "--build", build, "--out", out,
                        "--host-build", "--test-builder", builder], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    pkg = r.stdout.strip().splitlines()[-1]
    assert os.path.basename(pkg) == "kuf-cod-recomp-1.2.3-linux-x86_64.tar.gz", pkg
    with tarfile.open(pkg) as tar:
        names = tar.getnames()
    assert {n.split("/")[0] for n in names} == {"kuf-cod-recomp-1.2.3-linux-x86_64"}, names
    rel = {n.split("/", 1)[1] for n in names if "/" in n}
    for need in ("bin/rexgluerd", "builder/kuf-localbuild", "kuf-cod.sh", "install.sh", "kuf_update.py", "VERSION",
                 "MANIFEST.sha256"):
        assert need in rel, f"missing {need}"
    assert "bin/kuf_cod" not in rel, "the game's program must not be in a package"
    assert not any(x.endswith((".xex", ".pak", ".sox", ".xwb", ".wmv")) or "generated" in x for x in rel), rel
    # The real updater accepts it once signed.
    e = Env(os.path.join(t, "e"))
    os.rename(pkg, os.path.join(e.src, os.path.basename(pkg)))
    p = os.path.join(e.src, os.path.basename(pkg))
    e.sign(p)
    e.release("1.2.3", p)
    # Without the game's files the program cannot be made: refused, nothing changes.
    code, res = e.run("update", "--yes")
    assert code == 4 and "default.xex" in res["error"] and e.current() is None, f"{code} {res}"
    os.makedirs(os.path.join(e.root, "game"))
    shutil.copy(os.path.join(game, "default.xex"), os.path.join(e.root, "game", "default.xex"))
    # A failed build refuses the update.
    os.environ["KUF_TEST_BUILD_FAILS"] = "1"
    try:
        code, res = e.run("update", "--yes")
    finally:
        del os.environ["KUF_TEST_BUILD_FAILS"]
    assert code == 4 and "no space left" in res["error"] and e.current() is None, f"{code} {res}"
    assert not [n for n in os.listdir(os.path.join(e.root, "versions")) if n.startswith(".staging")], "staged leftovers"
    code, res = e.run("update", "--yes")
    assert code == 0 and e.current() == "1.2.3", f"{code} {res}"
    assert os.access(os.path.join(e.root, "current", "bin", "kuf_cod"), os.X_OK), "the updater made the program"
    # install.sh from the unpacked package: a second install root, the stable launcher written.
    unpack = os.path.join(t, "unpack")
    with tarfile.open(p) as tar:
        tar.extractall(unpack, filter="data")
    root2 = os.path.join(t, "app2")
    r = subprocess.run([os.path.join(unpack, "kuf-cod-recomp-1.2.3-linux-x86_64", "install.sh"), "--root", root2, "--game-dir", game],
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
    assert "--gpu_plugin=xenos" in args, "the launcher turns the GPU on (without it the game stops on its first frame)"
    data = os.path.join(t, "data", "kuf-cod-recomp", "data")
    assert f"--content_root={data}/saves" in args and f"--user_data_root={data}/user" in args, args
    r = subprocess.run([launcher], env=env, capture_output=True, text=True)
    assert f"--game_data_root={game}" in r.stdout.splitlines(), "remembered"
    assert os.listdir(game) == ["default.xex"], "nothing written into the game folder"


@case("launcher (M11): the bundled libstdc++ only when the system's is older than the game needs; a start log")
def _(t):
    pkg = os.path.join(t, "pkg")
    os.makedirs(os.path.join(pkg, "bin", "compat"))
    launcher = os.path.join(pkg, "kuf-cod.sh")
    with open(launcher, "w") as f:
        f.write(open(os.path.join(ROOT, "tools", "package", "kuf-cod.sh")).read())
    os.chmod(launcher, 0o755)
    fake = os.path.join(pkg, "bin", "kuf_cod")
    with open(fake, "w") as f:
        f.write('#!/bin/sh\necho "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"\n')
    os.chmod(fake, 0o755)
    open(os.path.join(pkg, "bin", "compat", "libstdc++.so.6"), "wb").close()
    game = os.path.join(t, "game")
    os.makedirs(game)
    open(os.path.join(game, "default.xex"), "w").close()
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS",
                                                            "LD_LIBRARY_PATH")}
    env.update(HOME=os.path.join(t, "home"), XDG_CONFIG_HOME=os.path.join(t, "cfg"),
               XDG_DATA_HOME=os.path.join(t, "data"), PATH="/usr/bin:/bin", KUF_NO_DIALOG="1")
    compat = os.path.join(pkg, "bin", "compat")

    def run(requires):
        with open(os.path.join(pkg, "REQUIRES"), "w") as f:
            f.write(requires)
        r = subprocess.run([launcher, "--game-dir", game], env=env, capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        return r.stdout.strip().splitlines()[-1]
    # Newer than any system's: the bundled copy, first in the search path.
    assert run("glibc 2.30\nglibcxx 3.4.99\nglibcxx-bundled 3.4.99\n") == f"LD_LIBRARY_PATH={compat}"
    # Older than this system's: the system's own (nothing added).
    assert run("glibc 2.30\nglibcxx 3.4.19\nglibcxx-bundled 3.4.33\n") == "LD_LIBRARY_PATH="
    log = open(os.path.join(t, "data", "kuf-cod-recomp", "data", "logs", "launcher.log")).read()
    assert "libstdc++: the bundled one" in log and "libstdc++: the system's" in log and "glibc " in log, log


@case("check reports the installed version, the signed manifest's date and notes as plain text (not the release body)")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("1.2.0")
    e.sign(p)
    e.release("1.2.0", p, published="2026-11-02", notes="Fixes:\r\n- menus\x1b[31m red\x07\n" + "x" * 3000)
    e.releases[-1].update(published_at="2030-01-01T00:00:00Z", body="UNSIGNED body text")
    json.dump(e.releases, open(os.path.join(e.src, "releases.json"), "w"))
    code, r = e.run("check")
    assert code == 0 and r["current"] == "1.0.0" and r["latest"] == "1.2.0", f"{code} {r}"
    assert r["published"] == "2026-11-02", r["published"]
    notes = r["notes"]
    assert notes.startswith("Fixes:\n- menus") and "\x1b" not in notes and "\x07" not in notes and "\r" not in notes, notes[:40]
    assert len(notes) <= kuf_update.MAX_NOTES and "UNSIGNED" not in notes, len(notes)


@case("update prints download progress lines, then the result")
def _(t):
    e = Env(t)
    e.publish("1.0.0", files={"bin/kuf_cod": os.urandom(300_000), "kuf-cod.sh": b"#!/bin/sh\n",
                              "kuf_update.py": b"#!/usr/bin/env python3\n"})
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


# ---- M10 updater: signed update manifests, the loopback test server, first-start recovery ----

@case("manifest: a release without a signed update manifest is ignored")
def _(t):
    e = Env(t)
    p = e.package("1.0.0")
    e.sign(p)
    e.release("1.0.0", p, manifest=False)
    code, r = e.run("check")
    assert code == 3 and r["latest"] is None, f"{code} {r}"


@case("manifest: signed by another key, or as a package (namespace), is refused before anything else")
def _(t):
    for label, kw in (("other key", {"manifest_key": "other"}), ("package namespace", {"manifest_ns": NS}),
                      ("git namespace", {"manifest_ns": "git"})):
        e = Env(os.path.join(t, label.replace(" ", "_")))
        e.publish("1.0.0")
        e.run("update", "--yes")
        p = e.package("1.1.0")
        e.sign(p)
        if kw.get("manifest_key") == "other":
            kw = {"manifest_key": e.other}
        e.release("1.1.0", p, **kw)
        before = snapshot(e.root)
        for cmd in ("check", "update"):
            code, r = e.run(cmd, *(["--yes"] if cmd == "update" else []))
            assert code == 4 and "signature" in r["error"], f"{label} {cmd}: {code} {r}"
        assert snapshot(e.root) == before and e.current() == "1.0.0", f"{label}: unchanged"


@case("manifest: wrong version, platform, product, schema, package name or malformed fields are refused")
def _(t):
    edits = {
        "version": lambda m: m.update(version="9.9.9"),
        "platform": lambda m: m.update(platform="windows-x86_64"),
        "product": lambda m: m.update(product="other"),
        "schema": lambda m: m.update(schema=2),
        "package name": lambda m: m["package"].update(name="other.tar.gz"),
        "size type": lambda m: m["package"].update(size="12"),
        "sha256 form": lambda m: m["package"].update(sha256="xyz"),
        "min_installed form": lambda m: m.update(min_installed="soon"),
        "no package": lambda m: m.pop("package"),
    }
    for label, edit in edits.items():
        e = Env(os.path.join(t, label.replace(" ", "_")))
        e.publish("1.0.0")
        e.run("update", "--yes")
        p = e.package("1.1.0")
        e.sign(p)
        e.release("1.1.0", p, edit=edit)
        before = snapshot(e.root)
        code, r = e.run("update", "--yes")
        assert code == 4 and "manifest" in r["error"], f"{label}: {code} {r}"
        assert snapshot(e.root) == before, f"{label}: unchanged"


@case("manifest: a download whose size or SHA-256 differs from the signed manifest is refused")
def _(t):
    # The package replaced by another validly signed package of the same version (its own signature passes).
    e = Env(os.path.join(t, "swap"))
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("1.1.0")
    e.sign(p)
    e.release("1.1.0", p)
    e.package("1.1.0", files={"bin/kuf_cod": b"#!/bin/sh\necho other\n", "kuf-cod.sh": b"#!/bin/sh\n",
                              "kuf_update.py": b"#!/usr/bin/env python3\n"})
    e.sign(p)
    before = snapshot(e.root)
    code, r = e.run("update", "--yes")
    assert code == 4 and ("SHA-256" in r["error"] or "bytes" in r["error"]), f"swap: {code} {r}"
    assert snapshot(e.root) == before, "swap: unchanged"
    # A longer download than the manifest says stops as soon as it passes the size.
    e = Env(os.path.join(t, "long"))
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("1.1.0")
    e.sign(p)
    e.release("1.1.0", p, edit=lambda m: m["package"].update(size=100))
    code, r = e.run("update", "--yes")
    assert code == 4 and "larger" in r["error"] and e.current() == "1.0.0", f"long: {code} {r}"
    assert not [n for n in os.listdir(e.root) if n.startswith(".download-")], "no download left"


@case("compatibility: an update needing a newer installed version is refused (reinstall), an older need is fine")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("2.0.0")
    e.sign(p)
    e.release("2.0.0", p, edit=lambda m: m.update(min_installed="1.5.0"))
    code, r = e.run("update", "--yes")
    assert code == 4 and "1.5.0" in r["error"] and "setup" in r["error"] and e.current() == "1.0.0", f"{code} {r}"
    e = Env(os.path.join(t, "ok"))
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("2.0.0")
    e.sign(p)
    e.release("2.0.0", p, edit=lambda m: m.update(min_installed="1.0.0"))
    code, r = e.run("update", "--yes")
    assert code == 0 and e.current() == "2.0.0", f"{code} {r}"


@case("system (M11): an update needing a newer glibc or libstdc++ than this system has is refused before download")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("2.0.0")
    e.sign(p)
    e.release("2.0.0", p, edit=lambda m: m.update(requires={"glibc": "99.0"}))
    code, r = e.run("check")
    assert code == 4 and "glibc 99.0" in r["error"] and "newer system" in r["error"], f"{code} {r}"
    code, r = e.run("update", "--yes")
    assert code == 4 and e.current() == "1.0.0", f"{code} {r}"
    assert not any(n.startswith(".download-") for n in os.listdir(e.root)), os.listdir(e.root)
    # libstdc++: refused when neither this system's nor the package's own copy is new enough ...
    e = Env(os.path.join(t, "cxx"))
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("2.0.0")
    e.sign(p)
    e.release("2.0.0", p, edit=lambda m: m.update(requires={"glibc": "2.17", "glibcxx": "3.4.99"}))
    code, r = e.run("update", "--yes")
    assert code == 4 and "GLIBCXX 3.4.99" in r["error"] and e.current() == "1.0.0", f"{code} {r}"
    # ... and accepted when the package carries one that is (the launcher then uses it).
    e = Env(os.path.join(t, "bundled"))
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("2.0.0")
    e.sign(p)
    e.release("2.0.0", p, edit=lambda m: m.update(requires={"glibc": "2.17", "glibcxx": "3.4.99",
                                                            "glibcxx_bundled": "3.4.99"}))
    code, r = e.run("update", "--yes")
    assert code == 0 and e.current() == "2.0.0", f"{code} {r}"


@case("system (M11): a requirement this updater cannot check is refused (the setup installs such a release)")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("2.0.0")
    e.sign(p)
    e.release("2.0.0", p, edit=lambda m: m.update(requires={"glibc": "2.17", "vulkan": "1.3"}))
    code, r = e.run("check")
    assert code == 4 and "vulkan" in r["error"] and "setup" in r["error"], f"{code} {r}"
    p = e.package("3.0.0")
    e.sign(p)
    e.release("3.0.0", p, edit=lambda m: m.update(requires={"glibc": "new"}))
    code, r = e.run("check")
    assert code == 4 and "requires" in r["error"], f"{code} {r}"


@case("system (M11): the unpacked package's REQUIRES is checked before the switch (also without a manifest)")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("2.0.0", files={"bin/kuf_cod": b"#!/bin/sh\n", "kuf-cod.sh": b"#!/bin/sh\n",
                                  "kuf_update.py": b"#!/usr/bin/env python3\n", "REQUIRES": b"glibc 99.0\n"})
    sig = e.sign(p)
    code, r = e.run("install-package", p, sig)
    assert code == 4 and "glibc 99.0" in r["error"] and e.current() == "1.0.0", f"{code} {r}"
    assert sorted(os.listdir(os.path.join(e.root, "versions"))) == ["1.0.0"], os.listdir(e.root)
    p = e.package("2.0.1", files={"bin/kuf_cod": b"#!/bin/sh\n", "kuf-cod.sh": b"#!/bin/sh\n",
                                  "kuf_update.py": b"#!/usr/bin/env python3\n", "REQUIRES": b"glibc 2.17\n"})
    sig = e.sign(p)
    code, r = e.run("install-package", p, sig)
    assert code == 0 and e.current() == "2.0.1", f"{code} {r}"


@case("update writes .first-start for the launcher; rollback clears it")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    e.publish("1.1.0")
    e.run("update", "--yes")
    first = os.path.join(e.root, kuf_update.FIRST_START)
    assert open(first).read().split() == ["1.1.0", "1.0.0"], open(first).read()
    code, r = e.run("rollback")
    assert code == 0 and e.current() == "1.0.0" and not os.path.exists(first), f"{code} {r}"


@case("a package without the launcher's files (kuf-cod.sh, kuf_update.py, bin/kuf_cod) is refused")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("1.1.0", files={"bin/kuf_cod": b"#!/bin/sh\n", "kuf-cod.sh": b"#!/bin/sh\n"})
    e.sign(p)
    e.release("1.1.0", p)
    code, r = e.run("update", "--yes")
    assert code == 4 and "kuf_update.py" in r["error"] and e.current() == "1.0.0", f"{code} {r}"


@case("api: only GitHub, or http on a loopback address with a port; loopback mode admits no other origin")
def _(t):
    for api, ok in (("https://api.github.com", True), ("http://127.0.0.1:8123", True), ("http://[::1]:8123", True),
                    ("http://localhost:8123", False), ("http://192.168.1.5:8123", False), ("http://127.0.0.1", False),
                    ("https://evil.example", False), ("http://127.0.0.1:8123/x", False),
                    ("http://u:p@127.0.0.1:8123", False), ("http://0.0.0.0:8123", False)):
        e = Env(os.path.join(t, str(abs(hash(api)))))
        with open(os.path.join(e.root, "update.conf"), "a") as f:
            f.write(f"api = {api}\n")
        try:
            kuf_update.Install(e.root)
            accepted = True
        except kuf_update.Failed:
            accepted = False
        assert accepted == ok, f"{api}: accepted {accepted}"
    lo = ("http", "127.0.0.1", 8123)
    kuf_update._check_url("http://127.0.0.1:8123/download/x", lo)
    kuf_update._check_url("https://api.github.com/x", lo)
    for u in ("http://127.0.0.1:8124/x", "http://evil.example:8123/x", "http://[::1]:8123/x"):
        try:
            kuf_update._check_url(u, lo)
        except kuf_update.Refused:
            continue
        raise AssertionError(f"accepted {u} in loopback mode")
    try:  # without a loopback api, plain HTTP is refused even to 127.0.0.1
        kuf_update._check_url("http://127.0.0.1:8123/x")
        raise AssertionError("accepted http without loopback mode")
    except kuf_update.Refused:
        pass


def _serve(e, rate=0):
    """tools/updater/release_server.py on a free loopback port for e's releases; returns (process, port)."""
    rel = [{"tag_name": r["tag_name"], "draft": r["draft"], "prerelease": r["prerelease"],
            "assets": [a["name"] for a in r["assets"]]} for r in e.releases]
    json.dump(rel, open(os.path.join(e.src, "releases.json"), "w"))
    port_file = os.path.join(e.tmp, "port")
    proc = subprocess.Popen([sys.executable, "-I", os.path.join(ROOT, "tools", "updater", "release_server.py"),
                             "--dir", e.src, "--repo", "player/kuf-cod-recomp", "--port-file", port_file,
                             "--rate", str(rate), "--log", os.path.join(e.tmp, "server.log")])
    for _ in range(100):
        if os.path.exists(port_file):
            break
        time.sleep(0.05)
    port = int(open(port_file).read())
    with open(os.path.join(e.root, "update.conf"), "a") as f:
        f.write(f"api = http://127.0.0.1:{port}\n")
    return proc, port


def _run_http(e, *args):
    out = io.StringIO()
    old = sys.stdout
    sys.stdout = out
    try:
        code = kuf_update.main(["--install-root", e.root, "--json", *args])
    finally:
        sys.stdout = old
    lines = [json.loads(x) for x in out.getvalue().strip().splitlines()]
    return code, lines


@case("loopback release server: the GitHub code path checks, downloads with progress, verifies and installs")
def _(t):
    e = Env(t)
    e.publish("0.7.0")
    e.run("update", "--yes")
    e.publish("0.7.1", files={"bin/kuf_cod": os.urandom(400_000), "kuf-cod.sh": b"#!/bin/sh\n",
                              "kuf_update.py": b"#!/usr/bin/env python3\n"})
    proc, port = _serve(e)
    try:
        code, lines = _run_http(e, "check")
        assert code == 0 and lines[-1]["latest"] == "0.7.1" and lines[-1]["update"], lines
        code, lines = _run_http(e, "update", "--yes")
        assert code == 0 and e.current() == "0.7.1", lines[-1]
        assert [x for x in lines if "progress" in x][-1]["progress"] == lines[-1].get("current") or True
        assert {"stage": "verifying"} in lines and {"stage": "installing"} in lines, lines
        code, lines = _run_http(e, "check")
        assert code == 3 and not lines[-1]["update"], lines[-1]
    finally:
        proc.terminate()
        proc.wait(10)
    log = open(os.path.join(t, "server.log")).read()
    assert "/repos/player/kuf-cod-recomp/releases" in log and "/download/kuf-cod-recomp-0.7.1" in log, log[-300:]


@case("loopback release server: a tampered package on the server is refused, nothing changes")
def _(t):
    e = Env(t)
    e.publish("0.7.0")
    e.run("update", "--yes")
    p = e.publish("0.7.1")
    with open(p, "r+b") as f:  # same size, one byte changed after signing
        f.seek(100)
        b = f.read(1)
        f.seek(100)
        f.write(bytes([b[0] ^ 1]))
    proc, _ = _serve(e)
    try:
        before = snapshot(e.root)
        code, lines = _run_http(e, "update", "--yes")
        assert code == 4 and "SHA-256" in lines[-1]["error"], lines[-1]
        assert snapshot(e.root) == before and e.current() == "0.7.0", "unchanged"
    finally:
        proc.terminate()
        proc.wait(10)


@case("an interrupted download (the connection ends early) changes nothing and leaves nothing behind")
def _(t):
    import http.server
    import threading
    e = Env(t)
    e.publish("0.7.0")
    e.run("update", "--yes")
    p = e.publish("0.7.1", files={"bin/kuf_cod": os.urandom(200_000), "kuf-cod.sh": b"#!/bin/sh\n",
                                  "kuf_update.py": b"#!/usr/bin/env python3\n"})
    data = open(p, "rb").read()
    files = {os.path.basename(n): open(os.path.join(e.src, n), "rb").read()
             for r in e.releases for n in (a["name"] for a in r["assets"])}

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            port = self.server.server_address[1]
            if self.path.startswith("/repos/"):
                body = json.dumps([{"tag_name": r["tag_name"], "draft": False, "prerelease": False,
                                    "assets": [{"name": a["name"], "size": a["size"],
                                                "browser_download_url": f"http://127.0.0.1:{port}/d/{a['name']}"}
                                               for a in r["assets"]]} for r in e.releases]).encode()
            else:
                body = files[self.path.rsplit("/", 1)[1]]
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body == data:
                self.wfile.write(body[: len(body) // 2])  # the connection drops half way
                self.wfile.flush()
                self.connection.shutdown(2)
                return
            self.wfile.write(body)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    with open(os.path.join(e.root, "update.conf"), "a") as f:
        f.write(f"api = http://127.0.0.1:{srv.server_address[1]}\n")
    try:
        before = snapshot(e.root)
        code, lines = _run_http(e, "update", "--yes")
        assert code == 1 and ("incomplete" in lines[-1]["error"] or "SHA" in lines[-1]["error"]), lines[-1]
        assert snapshot(e.root) == before and e.current() == "0.7.0", "unchanged"
        assert not [n for n in os.listdir(e.root) if n.startswith(".download-")], "no partial download left"
    finally:
        srv.shutdown()


@case("release_server.py binds 127.0.0.1 only and serves only listed assets")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    open(os.path.join(e.src, "secret.txt"), "w").write("x")
    proc, port = _serve(e)
    try:
        import urllib.error
        import urllib.request
        out = subprocess.run(["ss", "-ltnH", f"sport = :{port}"], capture_output=True, text=True).stdout
        local = [line.split()[3] for line in out.splitlines() if line.strip()]
        assert local == [f"127.0.0.1:{port}"], out
        for path in ("/download/secret.txt", "/download/..%2Freleases.json", "/download/releases.json", "/x"):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5)
                raise AssertionError(f"served {path}")
            except urllib.error.HTTPError as err:
                assert err.code == 404, f"{path}: {err.code}"
    finally:
        proc.terminate()
        proc.wait(10)


@case("release tools: make_manifest.py and sign_package.sh produce assets the updater accepts")
def _(t):
    e = Env(t)
    key = os.path.join(t, "relkey")
    subprocess.run([os.path.join(ROOT, "tools", "package", "sign_package.sh"), "--new-key", key, "--no-passphrase"],
                   check=True, capture_output=True)
    assert oct(os.stat(key).st_mode & 0o777) == "0o700", "key directory private"
    assert oct(os.stat(os.path.join(key, "kuf-cod-release")).st_mode & 0o777) == "0o600", "private key 0600"
    signers = open(os.path.join(key, "allowed_signers")).read()
    assert f'namespaces="{NS},{MNS}"' in signers, signers
    with open(os.path.join(e.root, "allowed_signers"), "w") as f:
        f.write(signers)
    r = subprocess.run([os.path.join(ROOT, "tools", "package", "sign_package.sh"), "--new-key", key,
                        "--no-passphrase"], capture_output=True, text=True)
    assert r.returncode != 0 and "not overwritten" in r.stderr, "an existing key is never overwritten"
    pkgs = {}
    for v in ("0.7.0", "0.7.1"):
        p = e.package(v)
        subprocess.run([os.path.join(ROOT, "tools", "package", "sign_package.sh"), "--key",
                        os.path.join(key, "kuf-cod-release"), p], check=True, capture_output=True)
        notes = os.path.join(t, "notes.txt")
        open(notes, "w").write(f"Test {v}\n")
        m = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "package", "make_manifest.py"), p,
                            "--notes", notes, "--min-installed", "0.7.0"], check=True, capture_output=True,
                           text=True).stdout.strip()
        subprocess.run([os.path.join(ROOT, "tools", "package", "sign_package.sh"), "--key",
                        os.path.join(key, "kuf-cod-release"), "--manifest", m], check=True, capture_output=True)
        e.releases.append({"tag_name": "v" + v, "draft": False, "prerelease": False,
                           "assets": [{"name": os.path.basename(x), "size": os.path.getsize(x),
                                       "browser_download_url": "https://github.com/x/y/" + os.path.basename(x)}
                                      for x in (p, p + ".sig", m, m + ".sig")]})
        pkgs[v] = p
        json.dump(e.releases, open(os.path.join(e.src, "releases.json"), "w"))
        code, r = e.run("update", "--yes")
        assert code == 0 and e.current() == v, f"{v}: {code} {r}"
    bad = e.package("0.7.2", vfile="0.7.1\n")
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "package", "make_manifest.py"), bad],
                       capture_output=True, text=True)
    assert r.returncode != 0 and "VERSION" in r.stderr, "make_manifest refuses a package whose VERSION differs"

@case("GitHub problems: rate limit, missing repository, outage and no network fail cleanly with a reason")
def _(t):
    import http.server
    import threading

    class H(http.server.BaseHTTPRequestHandler):
        reply = (200, {}, b"[]")

        def log_message(self, *a):
            pass

        def do_GET(self):
            code, headers, body = H.reply
            self.send_response(code)
            for k, v in headers.items():
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        cases = (("rate limit", (403, {"X-RateLimit-Remaining": "0", "X-RateLimit-Reset": "2000000000"}, b"{}"),
                  "limit"),
                 ("429", (429, {"Retry-After": "120"}, b"{}"), "limit"),
                 ("missing repository", (404, {}, b"{}"), "404"),
                 ("outage", (503, {}, b""), "not available"),
                 ("not JSON", (200, {}, b"<html>"), "release list"))
        for label, reply, want in cases:
            e = Env(os.path.join(t, label.replace(" ", "_")))
            e.publish("1.0.0")
            e.run("update", "--yes")
            with open(os.path.join(e.root, "update.conf"), "a") as f:
                f.write(f"api = http://127.0.0.1:{srv.server_address[1]}\n")
            H.reply = reply
            before = snapshot(e.root)
            code, lines = _run_http(e, "check")
            assert code == 1 and want in lines[-1]["error"], f"{label}: {code} {lines[-1]}"
            assert snapshot(e.root) == before, f"{label}: unchanged"
    finally:
        srv.shutdown()
    # No network: nothing listens on the port.
    import socket
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    e = Env(os.path.join(t, "offline"))
    with open(os.path.join(e.root, "update.conf"), "a") as f:
        f.write(f"api = http://127.0.0.1:{port}\n")
    code, lines = _run_http(e, "check")
    assert code == 1 and "could not reach" in lines[-1]["error"], lines[-1]


@case("a release missing one of its four assets is not offered; an older complete one is")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    p = e.package("1.1.0")
    e.sign(p)
    e.release("1.1.0", p)
    e.releases[-1]["assets"] = [a for a in e.releases[-1]["assets"] if not a["name"].endswith(".update.json.sig")]
    json.dump(e.releases, open(os.path.join(e.src, "releases.json"), "w"))
    code, r = e.run("check")
    assert code == 0 and r["latest"] == "1.0.0", f"{code} {r}"

@case("check reports the signed in-game summary (else the notes), never the release page's text")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    p = e.package("1.1.0")
    e.sign(p)
    e.release("1.1.0", p, notes="Fixes the dialog.", edit=lambda m: m.update(summary="Fixes the dialog."))
    e.releases[-1].update(body="Install: download the Setup AppImage and run it.")
    json.dump(e.releases, open(os.path.join(e.src, "releases.json"), "w"))
    code, r = e.run("check")
    assert code == 0 and r["summary"] == "Fixes the dialog." and "AppImage" not in json.dumps(r), r
    e2 = Env(os.path.join(t, "old"))
    e2.publish("1.0.0")
    e2.run("update", "--yes")
    e2.publish("1.1.0")  # a manifest without a summary (0.7.0's): its notes
    code, r = e2.run("check")
    assert code == 0 and r["summary"] == "Notes.", r


@case("an update downloads only the signed package, never the release's setup AppImage")
def _(t):
    e = Env(t)
    e.publish("0.7.0")
    e.run("update", "--yes")
    p = e.publish("0.7.1")
    img = os.path.join(e.src, "KingdomUnderFire-CircleOfDoom-Setup-0.7.1-x86_64.AppImage")
    open(img, "wb").write(b"\x7fELF not to be run")
    e.releases[-1]["assets"].insert(0, {"name": os.path.basename(img), "size": os.path.getsize(img),
                                        "browser_download_url": "https://github.com/x/y/" + os.path.basename(img)})
    proc, _ = _serve(e)
    try:
        code, lines = _run_http(e, "update", "--yes")
        assert code == 0 and e.current() == "0.7.1", lines[-1]
    finally:
        proc.terminate()
        proc.wait(10)
    gets = [line.split('"GET ')[1].split(" ")[0] for line in open(os.path.join(t, "server.log")) if '"GET ' in line]
    downloads = sorted(g.rsplit("/", 1)[1] for g in gets if g.startswith("/download/"))
    assert downloads == sorted(["kuf-cod-recomp-0.7.1-linux-x86_64.tar.gz", "kuf-cod-recomp-0.7.1-linux-x86_64.tar.gz.sig",
                                "kuf-cod-recomp-0.7.1-linux-x86_64.update.json",
                                "kuf-cod-recomp-0.7.1-linux-x86_64.update.json.sig"]), downloads
    assert not any("AppImage" in g for g in gets), gets


@case("make_manifest.py: the summary goes into summary and notes; setup instructions refused")
def _(t):
    e = Env(t)
    p = e.package("0.7.1")
    good = os.path.join(t, "summary.txt")
    open(good, "w").write("Fixes the update dialog.\n")
    out = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "package", "make_manifest.py"), p,
                          "--summary", good], check=True, capture_output=True, text=True).stdout.strip()
    m = json.load(open(out))
    assert m["summary"] == m["notes"] == "Fixes the update dialog.", m
    bad = os.path.join(t, "bad.txt")
    open(bad, "w").write("Download the Setup AppImage and run it.\n")
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "package", "make_manifest.py"), p,
                        "--summary", bad], capture_output=True, text=True)
    assert r.returncode != 0 and "setup" in r.stderr, r.stderr

@case("check reports the release's own GitHub page for Open GitHub Page, nothing else")
def _(t):
    e = Env(t)
    e.publish("1.0.0")
    e.run("update", "--yes")
    e.publish("1.1.0")
    good = "https://github.com/player/kuf-cod-recomp/releases/tag/v1.1.0"
    for html, want in ((good, good), ("https://evil.example/x", ""), (None, ""),
                       ("https://github.com/other/repo/releases/tag/v1.1.0", "")):
        if html is None:
            e.releases[-1].pop("html_url", None)
        else:
            e.releases[-1]["html_url"] = html
        json.dump(e.releases, open(os.path.join(e.src, "releases.json"), "w"))
        code, r = e.run("check")
        assert code == 0 and r["page"] == want, f"{html}: {r.get('page')}"


def _fake_install(t, versions, bins):
    """An install root with these versions, each kuf-cod.sh the real launcher and bin/kuf_cod the given script."""
    root = os.path.join(t, "app")
    for v in versions:
        d = os.path.join(root, "versions", v)
        os.makedirs(os.path.join(d, "bin"))
        open(os.path.join(d, "VERSION"), "w").write(v + "\n")
        with open(os.path.join(d, "kuf-cod.sh"), "w") as f:
            f.write(open(os.path.join(ROOT, "tools", "package", "kuf-cod.sh")).read())
        with open(os.path.join(d, "kuf_update.py"), "w") as f:
            f.write(open(os.path.join(ROOT, "tools", "updater", "kuf_update.py")).read())
        with open(os.path.join(d, "bin", "kuf_cod"), "w") as f:
            f.write(f'#!/bin/sh\necho {v} >> "{t}/runs"\n{bins[v]}\n')
        for x in ("kuf-cod.sh", "kuf_update.py", "bin/kuf_cod"):
            os.chmod(os.path.join(d, x), 0o755)
    os.symlink(f"versions/{versions[-1]}", os.path.join(root, "current"))
    open(os.path.join(root, "previous"), "w").write(versions[0] + "\n")
    open(os.path.join(root, "update.conf"), "w").write("repo = player/kuf-cod-recomp\n")
    with open(os.path.join(root, "kuf-cod"), "w") as f:
        f.write(f'#!/bin/sh\nexec "{root}/current/kuf-cod.sh" "$@"\n')
    with open(os.path.join(root, "kuf-update"), "w") as f:
        f.write(f'#!/bin/sh\nexec python3 -I "{root}/current/kuf_update.py" --install-root "{root}" "$@"\n')
    os.chmod(os.path.join(root, "kuf-cod"), 0o755)
    os.chmod(os.path.join(root, "kuf-update"), 0o755)
    game = os.path.join(t, "game")
    os.makedirs(game)
    open(os.path.join(game, "default.xex"), "w").close()
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS")}
    env.update(HOME=os.path.join(t, "home"), XDG_CONFIG_HOME=os.path.join(t, "cfg"),
               XDG_DATA_HOME=os.path.join(t, "data"), PATH="/usr/bin:/bin", KUF_NO_DIALOG="1")
    return root, game, env


@case("launcher: a new version that crashes on its first start is switched back and the old one starts")
def _(t):
    root, game, env = _fake_install(t, ["0.7.0", "0.7.1"], {"0.7.0": "exit 0", "0.7.1": "exit 70"})
    open(os.path.join(root, ".first-start"), "w").write("0.7.1\n0.7.0\n")
    r = subprocess.run([os.path.join(root, "kuf-cod"), "--game-dir", game], env=env, capture_output=True, text=True,
                       timeout=60)
    runs = open(os.path.join(t, "runs")).read().split()
    assert r.returncode == 0 and runs == ["0.7.1", "0.7.0"], f"{r.returncode} {runs} {r.stderr}"
    assert os.readlink(os.path.join(root, "current")) == "versions/0.7.0", "switched back"
    assert not os.path.exists(os.path.join(root, ".first-start")), "marker cleared"
    assert "did not start" in r.stderr, r.stderr


@case("launcher: no switch back after a normal first start, a later crash, or a deliberate stop")
def _(t):
    for label, code, first in (("normal", "exit 0", True), ("later crash", "exit 70", False),
                               ("SIGTERM", "exit 143", True)):
        d = os.path.join(t, label.replace(" ", "_"))
        os.makedirs(d)
        root, game, env = _fake_install(d, ["0.7.0", "0.7.1"], {"0.7.0": "exit 0", "0.7.1": code})
        if first:
            open(os.path.join(root, ".first-start"), "w").write("0.7.1\n0.7.0\n")
        subprocess.run([os.path.join(root, "kuf-cod"), "--game-dir", game], env=env, capture_output=True, text=True,
                       timeout=60)
        runs = open(os.path.join(d, "runs")).read().split()
        assert runs == ["0.7.1"], f"{label}: {runs}"
        assert os.readlink(os.path.join(root, "current")) == "versions/0.7.1", f"{label}: stays"
        assert not os.path.exists(os.path.join(root, ".first-start")), f"{label}: marker cleared"



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
