# Retail game files: why the setup cannot use them

Retail copies of `default.xex` are encrypted. The setup carries no console keys and does not decrypt, so it refuses them
with an explanation. It accepts only a `default.xex` that is already unencrypted. **This release is therefore not an
installer for ordinary retail copies, and must not be described as one.**

This project provides no decryption tool, key, download or instructions.

## Why it is not simply added

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

Until then, the code path stays out of the project. Adding support later is small: the setup already has one check point for
the executable (`InspectXex`), and the rest of the installation is the same.
