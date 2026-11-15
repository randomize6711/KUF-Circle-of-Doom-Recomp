# Retail import: implementation plan (M15.2, 2026-11-07)

**Status (2026-11-07, later):** the player chose a built-in key (docs/RETAIL_FILES.md "Decision of 2026-11-07"). Stage A
and the decryption of Stage B (B2 to B6, with the key built into `kuf-setup` instead of supplied by the player) are
implemented; B1, the key interface, was not needed. **2026-11-08:** the decryption is off by default and in every
release, and the gate below applies again to anything published (docs/RETAIL_FILES.md "Release decision of
2026-11-08"). The rest of this plan is kept as written.

Goal: one public AppImage setup that accepts the player's own dump, an Xbox 360 ISO or an extracted retail folder, and
distributes no game files, no generated game code and no console keys. Background: docs/RETAIL_IMPORT_RESEARCH.md.

The work is split by the legal gate in docs/RETAIL_FILES.md:

- **Stage A, implemented in M15.2:** everything that does not decrypt. Retail folders and ISOs are recognised and
  validated, unsupported versions are named, and a retail dump installs when the player also supplies an unencrypted
  `default.xex`.
- **Stage B, blocked:** the player-supplied key and decryption. It is planned below in full, but **no code for it is
  written or distributed** until the condition in docs/RETAIL_FILES.md is met (see "What unblocks Stage B").

## Stage A (implemented)

### A1. Executable identification (`installer/src/core/game_data.{h,cpp}`)
`InspectXex` reads, without any key: the encryption and compression fields, the module flags, and the execution info
header (title ID, media ID, version, base version, disc number and count). It also computes the file's SHA-256 and, when
unencrypted, the image SHA-256 as before. All field reads are bounds-checked; a file over 256 MB is refused.

The reference list (`installer/assets/game_v1.tsv`) gets directive lines, starting `@`, that older lists simply lack:
- `@title 424507D1 7092B5EC 00000001`: the supported title, media and version;
- `@retail-xex <sha256> <size>`: the SHA-256 of the original encrypted retail `default.xex`. A hash, not game data.

The verdicts, in order, each with its own message in the GUI, the unattended mode and the install step:
1. not an XEX file;
2. another game (title ID differs; the message shows the ID);
3. a title-update patch (`.xexp`, module flags) or another version of this game (the message shows both versions);
4. the supported retail executable, encrypted: "recognised, cannot be decrypted by this setup", plus the option below;
5. some other encrypted executable of the supported version (for example another region's build);
6. the supported unencrypted executable: accepted.

### A2. Retail folder detection
A folder with `$SystemUpdate` and the retail `default.xex` is named as a retail disc dump. `$SystemUpdate` and any other
extra files are ignored (they were already). File names in the reference list are matched exactly first, then
ignoring ASCII case (`Default.xex` from some dump tools).

### A3. ISO reading and validation (`installer/src/core/xiso.{h,cpp}`, new; own implementation)
- Formats: XDVDFS game partitions at the known offsets 0 (an extracted partition image), 0x2080000 (XGD3),
  0xFD90000 (XGD2) and 0x18300000 (XGD1). The volume descriptor at sector 32 of the partition must carry the magic
  `MICROSOFT*XBOX*MEDIA` at both ends.
- Directory trees: each directory is a binary tree of entries (left and right offsets in 4-byte units, start sector,
  size, attributes, name). Limits: depth 32, 100,000 entries, names 1 to 255 bytes, every sector range inside the
  image, a visited set against loops, no `.`, `..`, `/` or NUL in names.
- Output: a list of (path, offset, size). The GUI validates an ISO like a folder (present files and sizes, the
  executable's verdict from A1, read from the image into memory).
- Copying from an ISO hashes a byte range of the image into the installation, the same way a folder file is copied
  (through `.part`, renamed when complete, checked against the reference SHA-256).

### A4. The unencrypted-executable path
When the game files come from a retail folder or an ISO whose `default.xex` is encrypted, the setup offers a second
field: "Unencrypted default.xex". The file there must have the supported image SHA-256. The game data comes from the
dump and the executable from that file; nothing else changes (the local build reads the installed copy).
Unattended: `kuf-setup --unattended --game <folder or .iso> --executable <file>`.

### A5. Tests (unit tests only, no game runs)
- Synthetic XEX headers: unencrypted, encrypted, other title, other version, title-update patch, truncated or
  out-of-range header fields, oversized.
- Synthetic XDVDFS images written by the test: nested folders, the four partition offsets, case variations, malformed
  images (wrong magic, sectors past the end, a loop, too deep, bad names).
- Folder and ISO checks against a small synthetic reference list, with and without a separate executable.
- An installation from a synthetic ISO plus a separate executable (the existing install test harness).
- Read-only checks against the real retail folder and `build/game-plain` (skipped where absent): the verdicts
  "retail, recognised" and "accepted".
- One full local installation from the real retail folder plus `build/game-plain/default.xex` (`kuf_setup_e2e`;
  code generation and compiling, no game launch).
- The release audit (`tools/package/audit_release.py`) is unchanged and must still pass on the next package.

## Stage B (blocked: planned, not implemented)

### B1. The player-supplied key interface
- Shown only when A1 reports the supported retail executable (verdict 4), as a third choice beside "Unencrypted
  default.xex": a key field (32 hexadecimal characters, spaces ignored) or "Key file…" (a file of 16 bytes or 32
  hexadecimal characters).
- The setup checks the key against the SHA-256 the audit already holds (`tools/package/audit_release.py`). A wrong key is
  rejected before anything is copied ("This key does not decrypt this executable"). The setup says only what kind of key
  is needed; it does not say where to get one.
- The key lives only in locked memory (`mlock`), is zeroed after use, and is never written to disk, logs, the
  installation record, crash reports, the unattended command line (`--key-file` only, never the key itself) or
  environment variables passed on.

### B2. Encrypted XEX validation and decryption
1. Read the whole retail file into memory (it is 11.8 MB; the limit stays 256 MB).
2. Check verdict 4 (title, version, the retail SHA-256) before the key is used.
3. Session key: AES-128-ECB decryption of the header's 16-byte file key with the player's key.
4. Payload: AES-128-CBC with a zero IV, in place, over the payload from the header's data offset.
5. Clear the encryption field to 0, keep compression basic, decompress with the existing code, and compare the image
   SHA-256 with the reference list. A mismatch stops the installation with nothing installed.
6. AES: an own small implementation (FIPS-197, decryption only) with the FIPS-197 and NIST SP 800-38A test vectors,
   or the system's libcrypto loaded at run time; no new bundled dependency. LZX is not needed for this title.

### B3. Temporary files
- The decrypted executable is written only to `<install>/game/default.xex.part` (same filesystem as its destination),
  with mode 0600, fsync'd, checked, then renamed to `default.xex` (0644). Never to `/tmp`, never to the player's
  dump folder, never left beside the dump.
- The ISO path writes nothing outside the installation; the code generator's scratch output stays where it is today
  (removed after compiling, docs/LOCAL_BUILD.md).

### B4. Error recovery
- A wrong key, an unknown executable or a failed image check: nothing is copied (checked in the validation step).
- Cancel or a failure during or after decryption: the `.part` file is deleted; a new installation's staging folder is
  removed as today; a repair keeps the previous working version.
- A crash: a stale `default.xex.part` is deleted at the next start of the setup or the updater, before anything else.
- Updates: the updater never needs the key; it rebuilds from the installed, already decrypted executable.

### B5. Licensing and release changes
- docs/RETAIL_FILES.md rewritten to record who authorised it and on what basis; docs/LOCAL_BUILD.md "Decryption keys"
  and the setup's game page updated, without saying where keys are found.
- The audit stays strict: any file holding a console key fails the release, as now. A new check: the setup's binary
  contains no key schedule of the retail key (its expanded round keys, by hash), so a key cannot be built in by mistake.
- The SDK's key tables stay deleted; patch 0019 stays. Decryption lives only in the setup, not in the runtime or the code
  generator.
- No GPL code copied from other projects; credit in licenses/THIRD-PARTY-NOTICES.txt for any reused MIT code.

### B6. Tests for Stage B
- AES test vectors; a synthetic encrypted XEX made by the test with a random key (encrypt, then decrypt through the setup).
- Wrong key, wrong file, truncated file, cancel during decryption, a crash simulated by a left-over `.part` file.
- The key never appears in the log, the record, the installation folder or the process environment (a scan of each for
  the test key).
- With the player's own key, held outside the repository: a full local installation from the real retail folder and
  from an ISO, and the image SHA-256 equal to the reference.
- The release audit on the finished package.

## What unblocks Stage B

One of the following, in writing, recorded in docs/RETAIL_FILES.md (from that document's "What would change this"):
1. authority from the copyright owner of the game (and, as far as it holds rights in the protection, Microsoft) to
   decrypt a player's own copy for this purpose; or
2. a lawyer's opinion that the specific Stage B workflow (a public setup containing decryption code, a key supplied by
   the player) is lawful where it will be offered, with what it may and may not include.

Then the player's explicit approval to implement Stage B, and separately to publish a release containing it.
