# Architecture

## Pipeline

```
game folder (read-only)
   │  tools/stage_input.sh v1      (cp + SHA-256 check)
   ▼
build/input/v1/default.xex ──► rexglue codegen (kuf_cod_manifest.toml + config/v1/*.toml)
   │                                   │
   │ tools/xeximage.py                 ▼
   ▼                            generated/default/*.cpp  (28,908 functions, never committed)
build/analysis/v1/                     │
  image.bin, flat.dis ◄── audit ───────┤  tools/audit_coverage.py, tools/indirect_targets.py
                                       ▼
                    kuf_cod_recomp (object library) ─┐
                    src/ (ReXApp host)               ├─► build/<preset>/bin/kuf_cod
                    rexruntime (SDK, shared)         │     + librexruntime*.so, librexgpu-xenos*.so
                    rexgpu-xenos (SDK GPU plugin) ───┘
```

At runtime, ReXGlue (Xenia-derived, BSD-3) provides:
- the kernel/XAM HLE;
- the PM4 GPU command processor with Vulkan rendering, loaded as the `xenos` plugin through `--gpu_plugin=xenos`;
- XMA audio through FFmpeg;
- SDL3 input and windowing;
- a VFS that maps the game folder as `game:`/`d:`.

Our code layers on top through `ReXApp` virtual hooks, function overrides (`REX_HOOK`, weak
`sub_XXXXXXXX` symbols) and mid-asm hooks in the codegen config.

## Runtime layer (M2)

| File | Role |
|---|---|
| `src/runtime/crash_report.*` | Last-resort fault handler: one deterministic report, exit 70; `FailFast(reason)` |
| `src/runtime/eh_trace.cpp` | Guards for the guest `setjmp`/`longjmp` bodies and `RaiseException` (K4) |
| `src/runtime/telemetry.*` | Frames, file opens, guest tick rate, CPU, `run-summary.json`, `--kuf_exit_after_seconds` |
| `src/runtime/import_audit*` | Optional per-import call audit (Linux, `KUFCOD_IMPORT_AUDIT`) |
| `src/kuf_cod_app.h` | Wires these into the ReXApp hooks; refuses a writable game mount |
| `patches/rexglue-sdk/*.patch` | SDK fixes, applied by `tools/apply_sdk_patches.sh`; configure fails if one is missing. The submodule's working tree therefore shows as modified |

Game functions are wrapped with the SDK's documented override mechanism (`REX_HOOK_RAW` on the weak
`sub_XXXXXXXX` symbols, calling `__imp__sub_XXXXXXXX`). Addresses are v1-specific and documented in
ENGINE_ANALYSIS §9.

## Planned module boundaries (from M3 on)

| Module | Purpose | Mechanism |
|---|---|---|
| `src/app` | ReXApp subclass: paths, settings, dialogs | ReXApp hooks |
| `src/runtime` | Fixes and overrides of kernel/XAM behaviour this game needs | `REX_HOOK` on imports, SDK patches in `patches/` |
| `src/game/<ver>` | Address-specific hooks (frame pacing, FOV, resolution) | mid-asm hooks, function overrides |
| `src/platform` | Input bindings, settings file, save storage | SDK services + our code |
| `src/net` | XNet/XSession/QoS emulation, transports | import overrides |

Game patches are written against **named symbols**, never raw addresses scattered in code. Each version
maps names to addresses in `config/<ver>/` (codegen-side) and `src/game/<ver>/addresses.h` (host-side).

## Executable versions (title update)

- Every version is keyed (`v1`, later e.g. `tu1`) with its own SHA-256 entry in `tools/stage_input.sh`,
  its own `config/<ver>/` and its own `build/input|analysis/<ver>/`.
- ReXGlue v0.10's manifest **no longer supports `patch_file_path`**, which XenonRecomp used to apply
  `.xexp` deltas. A TU build therefore needs a staging step that applies the delta first. Options: Xenia's
  BSD-licensed XEX patcher code, or the SDK's own XEX loader if it accepts patches. That patcher would
  write a patched XEX into `build/input/<ver>/`, and codegen then runs on that file. Function addresses
  change between versions, so hooks must go through the per-version address maps above.
- Only one version is built per build tree. The `generated/default` path is fixed by
  `generated/rexglue.cmake`, so selecting a version means a separate build directory and manifest. That's
  a small change, to make when a second version exists.
- The TU must come from the user's own console/download. Never fetch it from unofficial sources.

## Build layout

- `thirdparty/rexglue-sdk` is added with `add_subdirectory` (`REXSDK_DIR` in `CMakePresets.json`). The SDK
  normally writes its binaries into its own source tree (`out/<platform>`). Our top-level CMakeLists
  redirects `kuf_cod`, `rexruntime`, `rexglue`, `rexgpu-xenos` and `TracyClient` to `build/<preset>/bin`,
  so `$ORIGIN` rpaths resolve and presets don't overwrite each other.
- `rex_app.h` includes ImGui headers that `rex::runtime` doesn't propagate. We add ImGui's interface
  include directories to the host target (the Condemned 2 port works around the same gap).
- RelWithDebInfo binaries carry an `rd` suffix (`rexgluerd`, `librexruntimerd.so`); Debug uses `d`.
