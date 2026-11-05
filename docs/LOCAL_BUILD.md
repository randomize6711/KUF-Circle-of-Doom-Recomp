# The local build

A release contains no code of the game. The setup makes the native program on the player's machine, from the player's
own files, with tools it carries. Nothing is uploaded; nothing is installed on the system.

| In a release | Not in a release |
|---|---|
| the setup, the runtime libraries, the code generator | `bin/kuf_cod` or any translated game code |
| a compiler and linker (Clang, LLD), C and C++ headers and link inputs | generated C++ or objects made from it |
| the project's own code, prebuilt (`builder/lib/libkuf_own.a`) | game files, or data from `default.xex` |
| licences and complete corresponding source | console decryption keys |

`tools/package/audit_release.py` fails a release that breaks any of these (below).

## What happens during an installation

1. The game folder is checked: every file by size, `default.xex` by the SHA-256 of its decompressed memory image.
2. The game files are copied into the installation, each checked by SHA-256. The original folder is only read.
3. `bin/rexgluerd codegen` writes C++ from `default.xex` and the tracked `config/v1` (about 170 MB, removed afterwards).
4. Clang compiles the 162 generated files into `<install>/build-cache/<id>/obj` (about 60 MB, kept).
5. LLD links them with the project's prebuilt code into `versions/<version>/bin/kuf_cod`.
6. Only then is the version switched on; a failure or Cancel changes nothing that worked before.

`installer/src/core/localbuild.cpp` does steps 3 to 5, for the setup (`kuf-setup`) and for the updater
(`builder/kuf-localbuild`). The commands are the verified build's own, taken from its build tree by
`tools/package/make_builder.py` into `builder/recipe.txt`, without debug information. The code generator's output is
the same function for function as the verified build's (28,908 functions compared); only how they are split into files
differs.

The compiler is LLVM's source release built against the Steam Runtime 3 sysroot
(`tools/toolchain/build_local_compiler.sh`), the same version as the compiler of the verified build. It needs glibc 2.30
and `libgcc_s` from the system, as the game does.

**Recovery.** Each finished object is renamed into place, so an interrupted build continues where it stopped. Cancel
removes a new installation's objects; a failure or a crash keeps them for the next attempt.

**Updates.** A package's `generation_id` covers everything the generated objects depend on: the compiler, flags, headers,
code generator sources and configuration. When it is unchanged an update only links again, in seconds; when it changed
the updater regenerates and compiles before it switches versions. Generated code and programs are never uploaded.

## Decryption keys

Retail copies of `default.xex` are encrypted. **The setup does not decrypt them.** No release component contains a
console key, and none downloads, derives or asks for one: the SDK's key tables are deleted by pattern
(`tools/apply_sdk_patches.sh`, so that no patch quotes them), and patch 0019 removes the code that used them. The XEX
loader accepts only an unencrypted executable and refuses others with a message; title-update patches, whose keys derive
from the console keys, are not supported.

See docs/RETAIL_FILES.md for why, and what would change it. So the setup needs a `default.xex` that is already unencrypted. An encrypted copy is recognised and refused with an
explanation. This project does not provide a way to decrypt one, and this is the main limitation of this design: a
player whose copy is encrypted cannot use this release.

The audit knows the keys only by their SHA-256, so it can look for them without containing them.

**The SDK's own repository.** The upstream ReXGlue SDK, which this repository links as a submodule and does not
copy, has those key tables in its source. This project cannot change that repository. A clone with submodules
fetches them from upstream until `tools/apply_sdk_patches.sh` deletes them from the working tree. Release assets and
this repository's own files do not contain them.

## Licences

Every release carries its own source, so nothing depends on a third-party repository staying online
(`tools/package/make_source_archive.sh`):

- `kuf-cod-recomp-<version>-source.tar.xz`: this project and the SDK with its submodules as built. It includes FFmpeg,
  libmspack (both LGPL) and the GNU binutils disassembler (GPL).
- `kuf-cod-recomp-<version>-toolchain-sources.tar`: GCC 14.2.0, the Steam Runtime 3 source packages of the GCC 10 runtime
  parts, glibc and the Linux headers, and the AppImage runtime with libfuse (LGPL) and squashfuse.

`licenses/THIRD-PARTY-NOTICES.txt` in the package lists each component and its terms. `bin/rexgluerd` contains GPL code
and is distributed under the GPL; the runtime libraries and the game's program do not contain it.

### Notices shown to the player

The setup's first page names the LGPL libraries and the GPL code; `kuf-cod --licenses` prints the notices; the licence
texts stay in `current/licenses/` of an installation. The game's own screens are unchanged: its Credits screen comes
from the player's game files, and the project's code displays no copyright notices while the game runs. Whether LGPL
2.1 section 6 still calls for a notice inside the game is a question for legal review.

### Replacing the LGPL libraries

FFmpeg and libmspack are compiled into `bin/librexruntimerd.so`, which the game's program links dynamically and finds by
name. To use a modified version:

```sh
tar -xJf kuf-cod-recomp-<version>-source.tar.xz && cd kuf-cod-recomp-<version>-source
tools/toolchain/setup_sniper.sh
tools/package/rebuild_runtime.sh          # edit thirdparty/rexglue-sdk/thirdparty/FFmpeg or libmspack first
cp build/runtime-only/bin/librexruntimerd.so "<install>/current/bin/"
```

No game file or generated code is involved. `tools/package/rebuild_runtime.sh` configures with the Steam Runtime 3
sysroot and builds that one library.

## The audit's checks of this model

`tools/package/audit_release.py` unpacks the package and the AppImage and fails when:

- a file named `kuf_cod` or anything resembling generated code exists;
- a binary or archive defines a `sub_8xxxxxxx` function that the project's sources do not name as a hand-written hook;
- any file holds a console key;
- with `--image FILE` (a local memory image), any file holds a sampled chunk of the game's image;
- the source archive is missing, or holds a console key, generated code or game data.

## Measured (Ryzen 7 5700X3D, 8 cores; a shared machine, at 4 jobs)

A full installation took about 3.5 minutes and 11 to 12 minutes of processor time; code generation 15 s, compiling 3 minutes,
linking under a second. The largest process used about 0.6 GiB; the setup runs one job per GiB of available memory, up to
the processor count. On disk: 4.6 GB at the peak, 4.45 GB afterwards (game 4.15 GB, program and tools 240 MB, cached
objects 60 MB). An update that keeps the generated code took 2 s; one that changes it about 100 s, using every processor thread.
The setup asks for 5.5 GB free.
