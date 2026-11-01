# Releasing updates (the GitHub updater)

How a new version reaches players: Check for Updates in the game's title list runs the installed updater
(`tools/updater/kuf_update.py`), which reads the GitHub releases of the project's repository, verifies a signed
release and installs it beside the running version; the game then restarts into it. This page is the release
process, the signing keys, and the disposable test. Player-side details: docs/STEAM_DECK.md section 4.

**Status (2026-11-01):**

| Part | State |
|---|---|
| Updater: signed manifests, GitHub releases, rate limits / outages / no network, rollback | IMPLEMENTED, AUTOMATICALLY TESTED (40 cases) |
| Local update (loopback server, two real builds, the game's Check for Updates, restart) | MANUALLY VERIFIED by the player (2026-11-01, M10-T1) |
| Setup AppImage: unattended mode, installs an update-capable game (key, GitHub configuration) | IMPLEMENTED, AUTOMATICALLY TESTED (`appimage_test.sh selftest`, 20 checks) |
| Release tooling: make / sign / audit / publish / verify | IMPLEMENTED, dry run with a throwaway key: audit clean |
| Production key | created by the player (passphrase); public key in tools/package/allowed_signers once created |
| First release v0.7.0 on GitHub | AWAITING PUBLICATION APPROVAL |
| Update from GitHub in the game | not yet verified (needs the published release) |

The player's current installation (`~/Games/Kingdom Under Fire - Circle of Doom`, 0.7.0-dev.169) has no release key
and an updater without manifests: it updates only after one repair or reinstall with the v0.7.0 setup (section 3,
"Bootstrap"); saves, settings and the game-file copy are kept.

## 1. What a release is

A GitHub release `vX.Y.Z` (a Git tag alone, or a commit on `main`, is **not** an update) with four assets:

| Asset | What |
|---|---|
| `kuf-cod-recomp-X.Y.Z-linux-x86_64.tar.gz` | the package (tools/package/package_linux.sh): one top folder with `bin/`, `kuf-cod.sh`, `kuf_update.py`, `install.sh`, `VERSION`, `MANIFEST.sha256`, `allowed_signers` (the release public key), licences; no game data |
| `....tar.gz.sig` | its signature (`ssh-keygen -Y sign`, namespace `kuf-cod-recomp-release`) |
| `kuf-cod-recomp-X.Y.Z-linux-x86_64.update.json` | the update manifest (tools/package/make_manifest.py) |
| `....update.json.sig` | its signature (namespace `kuf-cod-recomp-manifest`) |

The manifest:

```json
{"schema": 1, "product": "kuf-cod-recomp", "version": "0.7.1", "platform": "linux-x86_64", "channel": "stable",
 "published": "2026-11-01", "notes": "plain text, at most 2,000 characters", "min_installed": "0.7.0",
 "package": {"name": "kuf-cod-recomp-0.7.1-linux-x86_64.tar.gz", "size": 61234567, "sha256": "..."}}
```

**Versions:** `MAJOR.MINOR.PATCH`, optionally `-rc.N` / `-beta.N` (semver order; build metadata such as `+g1234` is
not allowed in a release). Releases marked *pre-release* on GitHub are offered only to installs with
`channel = prerelease` in update.conf. Never re-use a version number: an install refuses to "update" to its own or an
older version, and a package whose `VERSION` differs from its tag is refused.

**Compatibility:** `min_installed` is the oldest installed version the release can update. An older install is told
to reinstall with the setup (for example when the install layout or the release key changes).

## 2. What the updater checks (in this order)

1. The release list comes only from `https://api.github.com/repos/<repo>/releases` (update.conf `repo`), with
   certificate verification; downloads only from GitHub's HTTPS hosts (redirects too). Plain HTTP is refused, except
   to a loopback test server that update.conf names explicitly (`api = http://127.0.0.1:<port>`; literal loopback
   addresses only, never a host name).
2. The newest release (drafts never; pre-releases only on the pre-release channel) that has all four assets.
3. Its manifest is downloaded (at most 64 KiB) and its signature verified against the install's `allowed_signers`
   **before it is parsed**. Then: schema 1, product, version = the tag's, platform = the install's, the package name =
   the release asset's, a sane size and a SHA-256.
4. Newer than the installed version? Compatible (`min_installed`)? Only then is an update offered; the game shows the
   manifest's (signed) date and notes, never the release page's text.
5. After the player confirms: the package is downloaded into `<root>/.download-*` (progress reported; stopped as soon
   as it is larger than the manifest says; a connection that ends early is an error), then its size and SHA-256 must
   equal the manifest's, and its own signature must verify.
6. Unpacking into `versions/.staging-*`: no absolute paths, `..`, links, devices, set-uid bits or second top folder;
   size and file-count limits; every file must match the package's `MANIFEST.sha256`; `VERSION` must be the
   release's; `kuf-cod.sh`, `kuf_update.py` and `bin/kuf_cod` must be present and executable.
7. The staging folder is renamed to `versions/X.Y.Z` and `current` switched by an atomic rename; `previous` records
   the version before; `.first-start` records the switch. Old versions beyond three are pruned.
Nothing downloaded is ever executed before it passed all of this. Any failure leaves the installation exactly as it
was (exit 4 refused / 1 error / 5 cancelled) and the next run removes leftovers.

**Recovery:** if the new version crashes (exit 70), cannot be run (126 / 127) or dies by a signal other than INT / TERM
within 60 s of its first start, the launcher switches back to the previous version and starts it, with a message. By
hand: `<root>/kuf-update rollback`. Saves, settings, bindings and logs (`~/.local/share/kuf-cod-recomp/data`), the
launcher's settings (`~/.config/kuf-cod-recomp`), the game files (`<root>/game`), the install folder and shortcuts are
never touched by an update.

## 3. The release key

The private key decides what every installed copy accepts. **It never goes into a repository, a package, the setup,
a build folder or a cloud folder that syncs automatically.**

Creating the production key (once; the player's decision):

```sh
tools/package/sign_package.sh --new-key ~/.local/share/kuf-cod-release-key   # asks for a passphrase: use a strong one
# -> kuf-cod-release (private, 0600 in a 0700 folder), kuf-cod-release.pub, allowed_signers (public)
```

Back it up right away: copy the folder to two offline places (for example an encrypted USB stick and an encrypted
external disk), and keep the passphrase in a password manager. Without the key no further update can be published to
existing installs (they would have to reinstall with a new setup); if it leaks, anyone can publish an update that
installs accept (then: a new key, a new setup, players reinstall).

**Bootstrap** (installs made before the key existed, such as the player's): run the new setup AppImage over the
installation (Repair): it installs the program with the manifest-capable updater and, because none is there, the
release key; the game files, saves and settings stay. From then on Check for Updates works.

Activating it (only after the player approves):
1. `cp ~/.local/share/kuf-cod-release-key/allowed_signers tools/package/allowed_signers` and commit it (public key
   only). From then on every package carries it and `install.sh` / the setup install it.
2. Rebuild and publish the setup (installer/appimage/build_appimage.sh), so new installs trust the key.
3. Existing installs made before (such as the player's 0.7.0-dev.169): reinstall with that setup (the game folder copy,
   saves and settings are kept), or copy `allowed_signers` into the install folder **and** update its updater by
   reinstalling; an old updater does not read manifests.

The disposable test uses a separate throwaway key without a passphrase (`<test dir>/key`), made fresh for each test
and never used for anything else.

## 4. Making a release

```sh
git switch <the branch to release>            # committed, tests passed; set_version.sh for a new base version
tools/package/make_release.sh --version 0.7.0 --notes notes-0.7.0.txt --appimage --strip
tools/package/sign_release.sh build/releases/v0.7.0 --key ~/.local/share/kuf-cod-release-key/kuf-cod-release
```

`make_release.sh` refuses a dirty tree and tracked game data; builds with `KUFCOD_RELEASE_VERSION` (then rebuild the
development version); packages (with the public key); writes the manifest; builds the setup AppImage around that
very package; writes SHA256SUMS; audits. `sign_release.sh` (the player: it asks for the key's passphrase) signs the
package and the manifest and audits again. Output in `build/releases/v0.7.0/`: the setup AppImage, the package, the
manifest, two signatures, SHA256SUMS, notes.txt, publish.txt, audit.txt.

**Audit** (`tools/package/audit_release.py`, part of each step): only the expected assets; signatures and manifest
checked with the updater's code; the package and the unpacked AppImage against allow-lists of file names; the
AppImage's package identical to the release package and both carrying the release public key; every byte scanned
for private keys, tokens, game data (names and the XEX header), home-folder paths, the user name and the git e-mail
address; SHA256SUMS. Built binaries carry no build-machine paths (`-fmacro-prefix-map`, both CMake projects).

**Publishing** (each step only with the player's approval of that exact release):
1. The source: `tools/publish_public.sh --public-dir <clone of the public repository> --rev <commit> --push`
   (one commit under the noreply address; never push this local repository).
2. `tools/package/publish_release.sh build/releases/v0.7.0 --target <the public commit> --confirm v0.7.0`: refuses
   without the matching `--confirm`, with an unclean audit, or when the tag or release exists (never replaces
   anything); creates the release (stable: marked latest); downloads every asset again and compares it byte for
   byte; runs the updater's own check against GitHub with the release key.
3. After publishing: `tools/package/set_version.sh 0.7.1` and commit (later development builds sort after 0.7.0).

**Never** edit or replace the assets of a published release; publish a new version instead. There is deliberately no
CI publishing: the private key stays offline with the player, and every release needs a person.

## 5. The disposable update test

`tools/updater/update_test.sh` tests the whole path with real builds and nothing real involved:

```sh
tools/updater/update_test.sh prepare build/update-test/<name>    # two releases (0.7.0, 0.7.1), install 0.7.0, checks
tools/updater/update_test.sh launch build/update-test/<name>     # the 0.7.0 install in a window, for the player
tools/updater/update_test.sh report build/update-test/<name>     # versions, server requests, game log, real install
```

- **prepare:** a throwaway key; the current commit released twice with `make_release.sh` (two builds of the same
  code, each stamped with its version, packaged, signed, with signed manifests; `--min-installed 0.7.0`); 0.7.0
  installed with its own `install.sh` into `<dir>/root` (update.conf: `repo = test/kuf-cod-update-test`,
  `api = http://127.0.0.1:47371`); 0.7.1 put into `<dir>/releases`; the installed updater's `check` against the test
  server (signatures, manifest, endpoint); a snapshot of the real installation, data and launcher settings.
- **launch:** bubblewrap with a private network namespace (no network at all, only its own loopback), the game
  folder read-only, only the test folder writable, HOME inside it (settings, saves, logs disposable; the settings a
  copy of the last sandboxed playtest's), the Wayland and PulseAudio sockets and the GPU. Inside: the test server on
  127.0.0.1:47371 (downloads throttled to 4 MiB/s so the progress can be seen) and the installed launcher in a window.
  No input is sent to the game.
- **report:** installed versions, the server's requests, the game log's updater lines, and whether the real
  installation is unchanged.

**AppImage tests** (`tools/package/appimage_test.sh`): the real setup AppImage, unattended (`kuf-setup --install-to /
--remove`, the same checks and installation code as its window), in bubblewrap with a disposable HOME and the game
folder read-only. `selftest DIR SETUP.AppImage`: install (folder, program, updater, key, GitHub configuration, 137
verified game files, menu and desktop shortcuts, no root-owned or set-uid files), the installed updater runs and
fails cleanly offline, the launcher passes the updater and restart file to the game, repair (a deleted game file
restored, saves kept), uninstall (shortcuts gone, saves kept), the real installation unchanged. `install` / `launch` /
`report`: an installation kept for the player's manual test from GitHub (launch shares the network).

Automated tests: `ctest -R tools.updater_security` (40 cases: GitHub rate limits, outages and no network, signatures and namespaces, manifests, sizes and hashes,
downgrades, compatibility, traversal, interrupted and cancelled downloads, the loopback server, the release tools,
rollback, the launcher's restart and first-start recovery).

## 6. Limits

- Check for Updates is in the title list (below CREDITS), not inside SETTINGS; the version line is on the title
  SETTINGS page.
- The release key cannot be replaced through an update (installs keep their `allowed_signers`); a new key means a
  reinstall.
- Recovery covers a version that fails to start; a version that starts but misbehaves later is rolled back by hand
  (`kuf-update rollback`).
- The updater needs `python3` and `ssh-keygen` on the system (present on SteamOS and the supported distributions).
