# Retail game files

## Decision of 2026-11-08 (2): decryption in every build, as other recompilation projects do

After testing the Windows build, the player decided that the setup and the portable launchers decrypt a retail
`default.xex` themselves, in every build including published ones, as other Xbox 360 recompilation projects'
installers do (their public sources carry the same retail XEX key). It replaces the release decision below: that
gate was the player's own condition, and the player has now made the risk decision it was waiting for. This project
gives no legal opinion; the key is Microsoft's, and distributing it carries a risk the player accepted.

- `KUF_SETUP_RETAIL_DECRYPTION` is **on by default** and in the release scripts. The key is still read from the
  pinned SDK's git history at build time into a header in the build folder: the repository never holds it, and a
  build without that history fails instead of silently dropping decryption (`-DKUF_SETUP_RETAIL_DECRYPTION=OFF`
  builds without, on purpose).
- Only the programs that decrypt hold it: the Linux setup (`kuf-setup`), the portable launcher (`Play KUF`,
  `Play KUF.exe`) and the Windows setup (the same launcher with its payload). The game package (runtime, code
  generator, `kuf-localbuild`, the compiler) and the source archives hold no key.
- The audits (`tools/package/audit_release.py`, `audit_portable.py`) enforce exactly that: the retail key is required
  in those programs (so a release cannot lose automatic retail import by accident) and refused everywhere else, and
  no other console key is allowed anywhere.
- The player's files are only read: the encrypted `default.xex` is decrypted in memory, checked against the supported
  image's SHA-256 and written unencrypted into the installation or the portable edition's
  `Data/game`; a separate unencrypted `default.xex` still works too.

## Release decision of 2026-11-08 (1, replaced the same day): no decryption in what is published

Preparing v0.8.0-rc.3, the player asked that no binary holding the key be published, and that the key not be taken
from deleted history, until the requirements of this document are met. They are not met: neither condition of "What
would change this" has been obtained. So since 2026-11-08:

- `KUF_SETUP_RETAIL_DECRYPTION` is **off by default**, and the release scripts (`tools/package/make_release.sh`,
  `installer/appimage/build_appimage.sh`) build with it off explicitly. Without it the XEX decryption routine is not
  compiled at all (`installer/src/core/xex_decrypt.h`); a released setup recognises a retail dump and asks for a
  separate unencrypted `default.xex`.
- The release audit is strict again: any file holding a console key fails the release, the setup included.
- The code stays in the repository for local use (ON: the key from the pinned SDK's git history, not distributed).

**No distribution approach keeps automatic retail installation without meeting the condition below.** Examined:

| Approach | Why it does not resolve it |
|---|---|
| Key built in (2026-11-07) | the published setup holds the key and the routine: the case this document is about |
| Key supplied by the player (docs/RETAIL_IMPORT_PLAN.md B1) | the published setup still holds the decryption routine; 17 USC 1201(a)(2) concerns the tool, not only the key |
| Key downloaded at run time (for example from another project's source) | the same routine, plus fetching restricted material from a third party |
| The setup driving a separately installed decryption tool | the setup is then the one-click circumvention workflow; the tool's own status is unknown |
| A separately decrypted `default.xex` (Stage A, what releases do) | resolves it, but is not automatic retail installation |

Automatic installation from an encrypted retail dump in a published setup therefore still needs one of the two items
in "What would change this", then the player's approval to publish it.

## Decision of 2026-11-07 (local builds only since 2026-11-08)

The player decided that the setup decrypts a retail `default.xex` with a built-in retail XEX key, as other Xbox 360
recompilations do (docs/RETAIL_IMPORT_RESEARCH.md: GTA V Recomp, Splatterhouse-PC, Saw2Recomp, and Xenia). **This was
the player's own decision, made without either condition of "What would change this" below** (no permission from the
rights holder, no lawyer's opinion); it does not establish that it is lawful, and the assessment below still describes
the risk. Publishing a release that contains it is a separate decision of the player's (2026-11-08: not without the condition
below; see above).

How it is limited (installer/src/core/xex_decrypt.h):
- Only the setup program (`kuf-setup`) decrypts and holds the key. The runtime, the code generator, patch 0019 and the
  deleted SDK key tables are unchanged; `kuf-localbuild` in the game package does not hold it; the release audit allows
  the key in `usr/bin/kuf-setup` only and still fails on it anywhere else, and on the other console key everywhere.
- The key is not in this repository or its history: the build reads it from the pinned SDK's own git history into a
  generated header in the build folder. A build without that history (the published source archive) has no decryption.
- The setup decrypts only an executable of the supported title and version, in memory, and installs it only if its
  image is the supported one (byte for byte the executable all testing used). Nothing is written beside the player's
  files; the updater never needs the key.

## Before 2026-11-07: why the setup did not decrypt

Retail copies of `default.xex` are encrypted. Until this decision the setup carried no console keys and did not decrypt;
it accepted a retail dump (M15.2 Stage A) only together with a separately supplied unencrypted `default.xex`. A setup
built without the key still behaves that way.

## Why it was not simply added

This is the project's engineering assessment, not legal advice. In the United States:

- 17 USC 1201(a)(1)(A) forbids circumventing a measure that controls access to a work, and 1201(a)(3)(A) defines that as
  including decrypting an encrypted work "without the authority of the copyright owner". Owning a copy of the game is not
  an exception in the statute.
- 17 USC 1201(a)(2) separately forbids offering the public a technology made for circumvention. Exemptions for a user's own
  act do not cover it, so a one-click decrypting installer is a different question from a player's own act.
- The exemptions in 37 CFR 201.40 do not fit. The video game exemption in (b)(19) concerns games whose authentication server
  was shut down, and preservation by eligible libraries, archives and museums. Nothing there covers a player preparing a
  game for a native port.
- 17 USC 1201(f) allows circumvention and tools for interoperability of an independently created program, within limits.
  Whether a recompilation of the game itself qualifies is unsettled, and courts disagree on how 1201 applies at all
  (Chamberlain v. Skylink; MDY v. Blizzard). This needs a lawyer's opinion, not this project's guess.

Other countries have their own rules; the European Union's are in Directives 2001/29/EC and 2009/24/EC.

No official route is known: the project found no publicly available Microsoft tool that produces an unencrypted executable
from a retail copy.

## What would change this

Any one of these, in writing:

1. **Authority from the copyright owner of the game**, to decrypt a player's own copy for this purpose, with the owner's
   and, as far as it holds rights in the protection or the libraries, Microsoft's agreement. Developer: Blueside. Publisher:
   Microsoft Game Studios. Trademark notices name Blueside and Phantagram. Who holds the copyright today is not confirmed
   and has to be established first.
2. **A lawyer's opinion** that a specific workflow is lawful in the places it will be offered, and what it may and may not
   include.

Neither has been obtained (see "Decision of 2026-11-07" above).

## Research and plan (2026-11-07)

How other Xbox 360 recompilations handle encrypted executables: docs/RETAIL_IMPORT_RESEARCH.md. The player prefers a
player-supplied key at first, then chose a built-in key ("Decision of 2026-11-07"). docs/RETAIL_IMPORT_PLAN.md: Stage A
(retail and ISO recognition, an unencrypted executable beside a retail dump) and the built-in-key decryption are
implemented; the player-supplied-key interface of Stage B was not needed.
