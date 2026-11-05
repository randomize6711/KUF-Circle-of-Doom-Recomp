# Dependencies and Tool Survey

Status: M0, 2026-10-20. Repository facts were gathered by a research pass that read READMEs and source in
shallow clones (nothing was built or run). They are marked **[V-src]**. Items marked **[V-local]** were
checked on this machine. Impressions are marked **[I]**. Re-verify pinned versions at M1 start.

## Recompilation toolchains

| Project | License | Activity | What it gives | Fit for KUF:COD |
|---|---|---|---|---|
| [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) | BSD-3 (+ Xenia portions) | v0.10.0 2026-08-21, nightlies Oct 2026 | XenonRecomp-style AOT PPC→C++ codegen (auto save/restore helper detection, per-`bctr` jump-table detection, optional SEH) + Xenia's kernel/XAM HLE, **PM4 GPU command processor with runtime Xenos→SPIR-V/DXBC shader translation**, D3D12 + Vulkan, XMA via FFmpeg fork | **Recommended base.** No per-game D3D hooking; ~25 public game ports use it (Blue Dragon, Condemned 2, PGR4, AC6, PDZ…). API still unstable ("expect significant changes"). |
| [hedge-dev/XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | MIT | Last commit 2025-08 (dormant), unmerged instruction PRs | PPC→C++ recompiler; reads retail-AES + basic-compressed XEX directly; uses `.pdata`; all code sections incl. `.embsec_` | Reference/fallback codegen. Jump-table analyser tuned to Sonic Unleashed. |
| [hedge-dev/XenosRecomp](https://github.com/hedge-dev/XenosRecomp) | MIT | Last commit 2025-08 | Offline Xenos microcode → HLSL → DXIL/SPIR-V; scans files for `0x102A110x` containers | Reference only. KUF's shaders are in a zlib bundle (needs extraction first), and vfetch/integer-constant/memexport gaps plus Unleashed-specific vertex assumptions make it a poor primary path. |
| [hedge-dev/UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) | **GPL-3.0** | Active (2026-06) | Runtime: kernel imports, heap, SDL input, Plume RHI renderer **hooking the game's static D3D9 by guest address** (~42 hooks + reverse-engineered `GuestDevice` layout), no XMA decoder | Architecture reference only. Its D3D layer is game-specific (different XDK build), and GPL-3 would bind our port. |
| [sonicnext-dev/MarathonRecomp](https://github.com/sonicnext-dev/MarathonRecomp) | GPL-3.0 | Active | Unleashed runtime fork + Xenia-derived XMA decoder | Reference for the "hook D3D" approach on another game. |
| [nimauria/Xenon-Recomp](https://github.com/nimauria/Xenon-Recomp) | MIT | Active, 8★ | Experimental IR/AOT + shared runtime | README: no commercial title playable. Not usable. |
| [xenia-canary](https://github.com/xenia-canary/xenia-canary) | BSD-3 | Active | Kernel/XAM semantics, PM4/EDRAM, shader translators, XMA | Ground-truth reference and the baseline: **KUF is completable in Canary** (issue #331). |
| [Plume](https://github.com/renderbag/plume) | MIT | — | Thin Vulkan/D3D12 RHI | Optional for later native rendering paths. |

## Pinned versions (M1, verified by building)

| Component | Pin | Notes |
|---|---|---|
| ReXGlue SDK | `c94f5eb` (main, v0.10.0 + 2 commits), submodule `thirdparty/rexglue-sdk` | Used via `add_subdirectory` (`REXSDK_DIR`); its install step would write `~/.cmake/packages`, so we avoid it |
| SDK submodules | as pinned by `c94f5eb` | SDL3, glslang, SPIRV-Tools, Vulkan headers/loader, VMA, FFmpeg (wmarti fork), imgui, Catch2, fmt, spdlog, tomlplusplus, simde, xxHash, libmspack, … |
| Clang | 23.1.3 (system) | SDK requires ≥ 20 |
| CMake / Ninja | 4.4.4 / system | |
| PowerPC binutils (tests only) | prebuilt in the SDK (`tools/binutils`, sha256 of `as`: `626017ea…`) | Run only inside `bwrap` (`tools/ppc_asm_sandboxed.sh`) |
| Python | 3.14 + numpy 2.5.4 | Analysis tools and test generator, run with `python3 -I` |

Facts learned in M1 that differ from the M0 survey:
- ReXGlue's manifest format replaced the wiki's TOML (`[project]` + `[entrypoint]`, `includes = [...]`).
  The `patch_file_path` option for title-update deltas is gone (ARCHITECTURE.md).
- The SDK builds SDL3 with the dummy/disk audio drivers disabled.
- ReXGlue's GPU emulation is a runtime-loaded plugin (`librexgpu-xenos*.so`, `--gpu_plugin=xenos`).
  Without it, `Vd*` calls are ignored.

## Runtime libraries (planned)

| Library | Use | Notes |
|---|---|---|
| SDL3 | Window, input, gamepad, haptics, audio output | Pin a release ≥ 2 weeks old at M1. ReXGlue's own choice is to be checked; adopt it if it already uses SDL. |
| Vulkan SDK / volk, VMA | Primary renderer | Provided via ReXGlue's Xenia-derived backend. |
| FFmpeg (XMA fork used by Xenia/ReXGlue) | XMA decoding | Local system ffmpeg n9.0.2 has `xma1/xma2/wmv3/vc1/asf/xmv` **[V-local]**, but Xenia's XMA path needs the fork. |
| zlib | `.pak`/`.ccz` | For tools and the optional loose-file mod loader. |
| toml++ / ini parser | `pc_settings` | Human-readable settings. |
| GameNetworkingSockets or ENet (M11+) | LAN/direct IP transport | Decide at M11. Steam/Discord stay optional (M13). |

## Toolchain on this machine **[V-local]**

clang, cmake, ninja, git, llvm-objdump, python3 are present. Missing: `xxd`, `7z`
(not needed). Build host: Arch (Omarchy), kernel 7.2. ReXGlue and XenonRecomp require **Clang ≥ 18**.
Windows builds need clang-cl/MSVC and an actual Windows test environment. Wine tests are not proof of
Windows compatibility.

## Asset/format tools

- In-house scripts (M0): `xexinfo.py` (XEX2 headers), `xeximage.py` (basic decompress + import
  ordinals via Xenia's export tables), `soxindex.py` (full `.sox`/`.pak` index + inflate check).
- vgmstream / unxwb for XWB/XSB inspection; ffmpeg for the WMV title movies.
- External community history: XeNTaX thread t=4361 (2010 QuickBMS PAK script, now offline); XPGameSaves
  save editor (2013). There is no published `.sox` spec. Ours was derived independently.

## Licensing summary

- Our runtime code can be BSD-3/MIT compatible if built on ReXGlue/Xenia. **Avoid copying UnleashedRecomp
  or MarathonRecomp code** unless the project accepts GPL-3.
- Generated C++ from the game binary, extracted shaders and assets are **proprietary derivatives**. They live
  only in ignored build directories and are never committed or distributed: a release makes them on the player's
  machine from the player's files (docs/LOCAL_BUILD.md).
- `thirdparty/disasm` in the SDK is GNU binutils code (GPL-2.0-or-later), linked into the code generator only; FFmpeg and
  libmspack (LGPL) are linked into the runtime library. Each release carries their source (docs/LOCAL_BUILD.md "Licences").
- A leaked "Sep 2006 Source Code" of COD appears on Hidden Palace (released Feb 2026). **Recommendation: do not
  download or consult it.** It is unlicensed proprietary code and would contaminate the project's provenance.
  (User decision.)
