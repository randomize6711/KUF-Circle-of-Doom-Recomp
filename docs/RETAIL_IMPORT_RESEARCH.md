# Retail import: how other Xbox 360 recompilations handle encrypted executables (2026-11-07)

Research for M15.2. Question: can one public AppImage accept the player's own dump (an ISO or an extracted retail folder)
without distributing game files, generated game code or console keys? This is an engineering survey, **not legal advice**,
and another project's public release does not establish that its approach is lawful. The legal gate in
docs/RETAIL_FILES.md is unchanged. The plan built on this survey is docs/RETAIL_IMPORT_PLAN.md.

Method: shallow clones of each repository's default branch (outside this repository, never built or run), their release
listings (`gh release list/view`), and a read-only look at our own retail folder. Key bytes were redacted from every
command output; no key was copied into this repository or its notes.

## The projects

| Project (licence) | Revision looked at | What it publishes | How it treats the encrypted XEX | Where the key is |
|---|---|---|---|---|
| GTA V Recomp, `andrewnakas/gtav-recomp` (GPL-3.0) | 2a76184, 2026-11-03 | v0.3.0: Linux / Windows / macOS packages and a source archive. Launcher, ReXGlue runtime, the `rexglue` code generator and LLVM; no game code (players recompile from their own discs and title update). A taint scan and a clean-room release procedure (`tools/release/`, `docs/RELEASE_CLEANROOM.md`). | `loader/src/install.cpp` reads ISOs (XDVDFS) and STFS title updates, copies `default.xex` and `default.xexp` unchanged, and runs the shipped stock code generator on them; the generator decrypts. | In the shipped runtime and code generator. `docs/RELEASE_CLEANROOM.md`: "The Xbox 360 XEX retail key in the runtime stays (owner decision: Xenia ships it too)." |
| Splatterhouse-PC, `mattfj10/splatterhouse-PC` (MIT) | ed1d19a, 2026-10-01 | Source only, no releases. `tools/build_from_iso.py` extracts the player's ISO, decrypts, recompiles and builds locally. | `tools/xex_decrypt.py`: AES-128-ECB of the header's file key with the master key gives the session key; AES-128-CBC (IV 0) over the payload; LZX through a bundled mspack port. Its own XDVDFS reader (`tools/xiso.py`). | Hard-coded in the public source ("publicly known retail key"). |
| Saw2Recomp, `Matt-code-d/Saw2Recomp` (MIT) | 5e52556, 2026-11-04 | v0.1.10: a Windows zip with `saw2.exe` (the recompiled game code), runtime DLLs, `extract-xiso.exe`, a WPF launcher. | `launcher/Services/IsoInstaller.cs` only extracts the ISO with extract-xiso and copies the files; the stock ReXGlue runtime decrypts `Default.xex` at run time. | In the shipped runtime. The package also distributes generated game code, which this project's rules exclude. |
| `Pa3uk/Splatterhouserecomp` | d9938a4 | A README only. | n/a | n/a |
| ReXGlue SDK `rexglue/rexglue-sdk` (BSD-3-style, derived from Xenia) | v0.10.0, c94f5eb | Source and binaries. | Xenia's XEX loader. | Key tables in `src/system/xex_module.cpp` and `src/kernel/xboxkrnl/xboxkrnl_crypt.cpp`; this project deletes them by pattern (`tools/apply_sdk_patches.sh`) and patch 0019 removes the code. |

Finding: **none of them avoids distributing the retail key.** Each either ships it in a binary (GTA V, Saw2) or in public
source (Splatterhouse-PC). The only design that meets all of this project's constraints, a player-supplied key, is not
used by any of them.

## Our retail files and our implementation

| | Original folder (`~/Documents/Decomp/_Games/x360/KUF circle of doom`) | Test copy (`build/game-plain`) |
|---|---|---|
| Files | 138, all equal to `build/input/v1/game_dir.sha256` (with `$SystemUpdate`) | 137, no `$SystemUpdate` |
| default.xex SHA-256 | `3ec406cc…529ff`, 11,808,768 bytes | `c859e4d1…a256`, same size |
| Encryption / compression | 1 (normal) / 1 (basic) | 0 (none) / 1 (basic) |
| Title, media, version | `424507D1`, `7092B5EC`, 0.0.0.1 (base) | same |
| Title update | none | none |

- The installer (`installer/src/core/game_data.cpp`, `InspectXex`) already reads basic compression and compares the
  decompressed image with the reference list (`installer/assets/game_v1.tsv`, image SHA-256 `c502f661…0c90627`). A
  decrypted retail file is therefore checkable byte for byte against what all testing used.
- For this title, retail import needs: AES-128 (one ECB block, CBC over the payload), clearing the encryption field,
  and the existing basic decompression. No LZX and no title-update (delta patch) support.
- The only key material: **one 16-byte retail XEX key** (the per-file key comes from the file's own header). No
  console-specific keys, keyvaults or title-update keys.
- `~/Games/Kingdom Under Fire - Circle of Doom` is a 0.7.0-dev.169 installation (the withdrawn model: a prebuilt game
  program whose runtime decrypted at run time, as Saw2Recomp does). It holds the encrypted executable.

## Architectures considered

1. **Player-supplied key** (preferred by the player, 2026-11-07; blocked by the legal gate): one public AppImage, ISO or
   folder input; the player enters the key; the setup checks it against a SHA-256 (which the audit already holds), never
   stores or logs it, decrypts in memory, checks the image hash, and continues with the existing local build.
2. **An external tool the player has** (for example xextool): no decryption code in the release, but a fiddly setup and
   a closed tool of unclear origin.
3. **Built-in key** (GTA V, Saw2, Xenia): excluded, the player forbids restoring the removed keys.
4. **ISO and layout support without decryption**: retail copies stay unusable unless the player also has an unencrypted
   executable. This is what M15.2 implements now (docs/RETAIL_IMPORT_PLAN.md "Stage A").

## Legal and licensing concerns

- Design 1 removes the key from the release, but the release would still contain code that decrypts the executable.
  17 USC 1201(a)(2) concerns offering a circumvention technology, so a player-supplied key reduces exposure but does not
  answer the question. The condition in docs/RETAIL_FILES.md (permission from the rights holder, or a lawyer's opinion)
  still applies.
- The setup could not say where to obtain the key; it could only check one the player enters.
- Reuse: GTA V's installer is GPL-3.0; this project is MIT: study only, no copying. Splatterhouse-PC's code is MIT and
  reusable with attribution, but small and key-bearing: an own implementation is cleaner. Upstream ReXGlue's decryption
  would mean restoring the deleted tables: excluded. extract-xiso's licence is unclear (GitHub reports NOASSERTION): not
  bundled. The XDVDFS reader in M15.2 is written from the format's public description.
