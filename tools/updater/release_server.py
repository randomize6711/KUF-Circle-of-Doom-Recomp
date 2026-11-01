#!/usr/bin/env python3
"""A loopback-only stand-in for GitHub's releases API, for disposable update tests (docs/RELEASING.md).

  python3 -I tools/updater/release_server.py --dir RELEASES --repo owner/name [--port 0] [--rate KIB]
                                             [--port-file FILE] [--log FILE]

Serves what tools/updater/kuf_update.py asks GitHub for, so an install whose update.conf says
`api = http://127.0.0.1:<port>` runs the updater's real code (release list, manifest and signature, download with
progress, verification, install):
  GET /repos/<repo>/releases   RELEASES/releases.json, each asset's size and browser_download_url filled in
  GET /download/<name>         the file RELEASES/<name> (a plain file name listed as an asset; nothing else)
releases.json: [{"tag_name": "v0.7.1", "prerelease": false, "draft": false, "published_at": "...",
                 "body": "...", "assets": ["<file name>", ...]}, ...]
Binds 127.0.0.1 only (never another address). --rate limits downloads to KIB KiB/s so progress can be watched.
--port 0 picks a free port; --port-file receives it once the server listens.
"""
import argparse
import http.server
import json
import os
import signal
import sys
import time

CHUNK = 64 << 10


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--rate", type=int, default=0, help="KiB per second for downloads (0: unlimited)")
    ap.add_argument("--port-file")
    ap.add_argument("--log")
    a = ap.parse_args()
    root = os.path.abspath(a.dir)
    log = open(a.log, "a", buffering=1) if a.log else sys.stderr

    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "kuf-release-test"

        def log_message(self, fmt, *args):
            log.write(f"{time.strftime('%H:%M:%S')} {self.address_string()} {fmt % args}\n")

        def releases(self):
            base = f"http://127.0.0.1:{self.server.server_address[1]}/download/"
            out = []
            for rel in json.load(open(os.path.join(root, "releases.json"))):
                rel = dict(rel)
                assets = []
                for name in rel.get("assets", []):
                    p = os.path.join(root, name)
                    if os.path.basename(name) == name and os.path.isfile(p):
                        assets.append({"name": name, "size": os.path.getsize(p), "browser_download_url": base + name})
                rel["assets"] = assets
                out.append(rel)
            return json.dumps(out).encode()

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == f"/repos/{a.repo}/releases":
                body = self.releases()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            name = path[len("/download/"):] if path.startswith("/download/") else ""
            listed = {n for rel in json.load(open(os.path.join(root, "releases.json"))) for n in rel.get("assets", [])}
            p = os.path.join(root, name)
            if not name or os.path.basename(name) != name or name not in listed or not os.path.isfile(p):
                self.send_error(404)
                return
            size = os.path.getsize(p)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            rate = a.rate * 1024
            try:
                with open(p, "rb") as f:
                    for chunk in iter(lambda: f.read(CHUNK), b""):
                        self.wfile.write(chunk)
                        if rate:
                            time.sleep(len(chunk) / rate)
            except (BrokenPipeError, ConnectionResetError):
                log.write(f"{time.strftime('%H:%M:%S')} download of {name} ended by the client\n")

    signal.signal(signal.SIGTERM, signal.default_int_handler)  # stop on TERM even if started with it ignored
    server = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    port = server.server_address[1]
    if a.port_file:
        tmp = a.port_file + ".tmp"
        open(tmp, "w").write(f"{port}\n")
        os.replace(tmp, a.port_file)
    log.write(f"{time.strftime('%H:%M:%S')} serving {root} for {a.repo} on http://127.0.0.1:{port}\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
