# Updates on Windows and in the portable edition (M17G)

The Linux installation updates with `tools/updater/kuf_update.py` (docs/STEAM_DECK.md "Updates"). Windows has no
Python and no `ssh-keygen`, so the Windows packages carry `Runtime\bin\kuf-update.exe`: the same signed releases, the
same rules, the same commands, JSON lines and exit codes, so the game's **Check for Updates** works unchanged.

**Status (2026-11-10): implemented and tested; not live.** No Windows release has been signed or published, so a
player's Check for Updates finds nothing yet ("up to date"). Publishing needs the player's approval
(docs/RELEASING.md).

## What it trusts

- Releases only from the repository in `Runtime\update.conf` (`randomize6711/KUF-Circle-of-Doom-Recomp`), over HTTPS
  with Windows' own certificate checks. Redirects are followed one at a time and only to GitHub's hosts. Three ways
  to the server are tried in turn until one gets an HTTP answer (0.9.0-rc.3; rc.1 forced TLS 1.2/1.3 on WinHTTP and
  a player's Windows 11 got "error 12152"): WinHTTP with Windows' own protocol settings, WinHTTP with TLS 1.2 only,
  and WinINet. An HTTP answer (403, 404, a refused redirect) is final; the way that worked is kept for the next
  downloads. A failure names every way and its error. `KUF_UPDATE_HTTP=winhttp|winhttp-tls12|wininet` uses only one
  (tests).
- The channel: `channel = prerelease` or `channel = stable` in update.conf; without one (the packages' update.conf
  has none, and an update replaces it), an installed pre-release (0.9.0-rc.N) is offered pre-releases and a release
  only releases. The Linux updater decides the same way.
  A loopback `api = http://127.0.0.1:<port>` exists only for tests.
- The newest release's **update manifest** (`<prefix>-<v>-windows-x86_64.update.json`) must be signed by a key in
  `Runtime\allowed_signers` (the project's release key, namespace `kuf-cod-recomp-manifest`) **before it is read**.
  Its version must be the tag's, its platform `windows-x86_64`, and the package named in it the release's.
- The **package** must have the manifest's size and SHA-256 and its own signature (namespace `kuf-cod-recomp-release`)
  before it is opened. A signature of one kind is never accepted as the other.
- No downgrade or reinstall of the same version unless asked (`--allow-downgrade`). A manifest with system
  requirements the updater cannot check is refused ("install it with the setup").
- Verification is done in the program (`installer/src/core/update/`): SHA-512, Ed25519 (RFC 8032) and OpenSSH's
  SSHSIG format, checked against the standards' vectors, against signatures made by `ssh-keygen`, and against the real
  0.8.0-rc.2 release's signatures with the project's key.

## What it changes

The update package is the portable edition's own ZIP (`KUF-Circle-of-Doom-Portable-<v>-windows-x86_64.zip`). Only
`Play KUF.exe` and `Runtime\` are taken from it (names checked: no absolute paths, drive letters, `..`; every file's
CRC checked before it is written). `GameFiles\` and `Data\` (saves, settings, logs, the decrypted game files, the
build cache) are never touched.

1. **Check for Updates** in the game runs `kuf-update.exe --json check`, then `update`: download (progress in the
   game), verify, unpack into `Runtime.next` and `Play KUF.exe.next`. The running version is not touched.
2. **Restart** in the game: the game starts `Play KUF.exe --after-restart <its process>` (`$KUF_LAUNCHER`, which the
   launcher sets) and quits. The launcher waits for it, renames `Runtime` to `Runtime.previous` and `Runtime.next` to
   `Runtime` (the same for `Play KUF.exe`; Windows allows renaming the running launcher), and starts the game.
   If any rename fails, all are undone and the update stays staged for the next start.
3. If the new version's code generator and compiler are the same (the package's generation id), the prepared
   `kuf_cod.exe` is carried over: no recompiling. Otherwise the game is prepared again on that start (a few minutes).

Back to the version before: `Play KUF.exe --rollback-update` (the launcher does it; `kuf-update.exe`, which runs from
inside `Runtime\`, cannot move that folder on Windows). The same works for the setup's installation, which has the
portable layout in `%LOCALAPPDATA%\Programs\...`.

## Publishing a Windows release (with the player's approval)

```sh
V=0.9.0
tools/package/make_windows_portable.sh $V
Z=build/releases-local/windows-$V/KUF-Circle-of-Doom-Portable-$V-windows-x86_64.zip
python3 -I tools/package/make_manifest.py "$Z" --summary summary.txt     # -> ...-windows-x86_64.update.json
tools/package/sign_package.sh --key KEY "$Z"                              # -> .zip.sig
tools/package/sign_package.sh --key KEY --manifest "${Z%.zip}.update.json"
# upload the four files to the release (with the setup .exe for new installations)
```

## Tests

- `installer/tests/test_update.cpp` (setup tests): versions, JSON, ZIP and inflate (Python's archives byte for byte;
  hostile and damaged ones refused), SHA-512, Ed25519, SSH signatures, and the whole flow from a local release folder
  (check, update, apply, rollback, the carried-over game, refusals: a changed package, a wrong hash, a manifest in the
  wrong namespace, another key, a downgrade).
- `tests/update/windows_update_wine.sh` (opt-in ctest `updater.windows_wine`): the real `kuf-update.exe` under Wine,
  downloading over WinHTTP from `tools/updater/release_server.py` in a sandbox with its own network: 11 checks.
  **Wine, not Windows**; the player's Windows test is in docs/WINDOWS_TEST.md once a test release exists.

## Not yet

- A release that fails to start is not switched back automatically (the Linux launcher does this); the player uses
  `--rollback-update`.
- The Linux portable edition has the same updater code but no published update channel (its archive is a .tar.gz;
  only ZIP packages are read). The Linux installation keeps `kuf_update.py`.
