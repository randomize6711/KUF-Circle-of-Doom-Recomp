# Feasibility Report — KUF: Circle of Doom native PC port (M0)

Date: 2026-10-20. Companion documents: ENGINE_ANALYSIS.md, DEPENDENCIES.md, TECHNICAL_RISKS.md, ROADMAP.md.

## Verdict

**Feasible.** Reaching a playable native build (M5) is realistic. Every modernisation goal is feasible too,
with three items carrying real engineering risk: frame-rate decoupling, true native resolution/ultrawide on
an emulated-GPU base, and LAN co-op for a Live-only game.

Why it is feasible:
- **The executable is a clean target [V].** Retail XEX2 with basic compression, which existing tools load
  directly. Full `.pdata` (22,650 functions, 97 % coverage). Conventional instruction mix. No title update or
  delta. 222 standard kernel/XAM imports, all known to Xenia.
- **The emulation baseline already works.** Xenia Canary runs the game to completion with saves working.
  ReXGlue packages the same kernel, GPU and XMA code with an AOT recompiler, and about 25 Xbox 360 ports
  already use it.
- **Middleware is ordinary and runs as guest code.** PhysX 2.7.2, Lua 5.1.1, XACT, XMedia and D3DX all
  ship statically linked, so they recompile with the game and need no reimplementation.
- **The data is open [V].** Archives are plain zlib with a simple index (all 23,752 entries listed and
  verified). Scripts are plain-text Lua. Shaders are precompiled Xenos microcode. This makes asset overrides
  and modding straightforward.

## Recommended architecture

```
 user's disc files (read-only) ──► tools/ (xex/pak/sox inspectors, install step)
                                         │
 default.xex ──► ReXGlue codegen ──► generated/ (C++; never committed)
                                         │
 runtime (ReXGlue SDK, pinned) ── kernel/XAM HLE, PM4 GPU→Vulkan (D3D12 later), XMA, threads
       │
 kuf/ (our code)
   ├─ patches/   mid-asm hooks: frame pacing, FOV/aspect, resolution, camera fixes, bug fixes
   ├─ platform/  SDL3 input (KB/M + pads), settings (pc_settings.toml), save store, logging
   ├─ net/       XNet/XSession/QoS emulation → transport (LAN/direct IP) → optional Steam/Discord
   ├─ mods/      loose-file override layer in front of .pak/.sox, Lua script overrides
   └─ ui/        PC settings menu (ImGui overlay first, native-styled later)
```

Key decisions (to approve):
1. **ReXGlue (BSD-3) as the base, not the UnleashedRecomp runtime.** It needs no per-game D3D
   reverse-engineering before the first image, it decodes XMA, and its license is permissive. The
   UnleashedRecomp model (hooking D3D by address) stays a documented fallback, used selectively if
   native-resolution rendering needs it (TECHNICAL_RISKS R4).
2. **Vulkan first** (comes with ReXGlue). D3D12 remains a future option.
3. **SDL3** for windowing, input and audio output, if it fits ReXGlue's platform layer. Otherwise use what
   ReXGlue already uses and wrap it.
4. **Simulation/render split by hooks, not rewrites.** Keep the original game tick and interpolate rendering.
   Scope is decided after the M8 investigation.
5. **Gameplay networking stays the game's own.** We replace only the Xbox Live layer under it (XNet
   addressing, XSession, QoS).

## Minimum runtime to reach the main menu (M4)

Derived from the import table; nearly all of it exists in ReXGlue/Xenia already.
- **Kernel:**
  - Memory: `MmAllocatePhysicalMemoryEx`, `NtAllocateVirtualMemory`
  - Threads and synchronisation: `ExCreateThread`, events/semaphores/timers/waits, critical sections,
    spinlocks, TLS, affinity
  - Files: `NtCreateFile`/`NtReadFile`/`NtQueryDirectoryFile` with `game:`/`d:` mapped to the read-only
    install, plus `ObCreateSymbolicLink`
  - Time: `KeQueryPerformanceFrequency`, `mftb`
- **GPU:** the `Vd*` ring buffer, interrupts and swap; the PM4 command processor; EDRAM resolve; shader
  translation.
- **Audio:** XAudio render driver client and XMA contexts (XACT itself is guest code).
- **XAM:**
  - Input: `XamInputGetState`/`SetState`/`GetCapabilities`.
  - User: one signed-in local profile (`XamUserGetSigninState`, `GetXUID`, `GetName`,
    `ReadProfileSettings`).
  - Content: `XamContentCreateEx`/`Close`/`Flush`/enumerate, backed by a native save directory. The
    device selector returns the HDD.
  - Settings: `XGetLanguage`, `XGetVideoMode` (60 Hz, which keeps the PAL-50 check happy),
    `XGetAVPack`.
  - Notifications: `XNotifyGetNext`.
  - UI dialogs: signin, device selector and message box are completed immediately with defaults.
- **Live:** `XNetStartup` and the other Live calls report "offline/not signed in to Live". The
  online menu becomes reachable later via our network layer.
- **Movies:** XMEDIA WMV decoding runs as guest code on the emulated GPU. Fallback: skip the attract
  video.

## Main challenges

| Area | Challenge | See |
|---|---|---|
| Timing | Speed tied to FPS; need a fixed sim tick + interpolation | R1, ROADMAP M8 |
| Resolution | Xenia resolution scaling breaks this game's shaders; arbitrary sizes need game-side hooks | R4, M7 |
| Shaders | ~4,100 embedded microcode shaders in FXL effects. Runtime translation handles them, and a pipeline cache avoids stutter. | R3 |
| Multiplayer | Live-only co-op; emulate XSession/XNet over LAN/IP | R6, M11–M12 |
| Title update | Base v1 only; TU fixes slow camera | R7 (user decision) |
| Inherited bugs | Music overlap, bloom, lava geometry, Balrog trigger | R5 |

## Workspace (approved and created in M1)

- **Location:** a local workspace (`~/Work/kuf-cod-recomp`). This matches the existing projects (`csx-recomp`,
  `OpenCE`). `/home` has 378 GB free, which is plenty: generated C++ plus build output is estimated at
  10–20 GB.
  - Alternative: `/mnt/steam-250gb` (229 GB free, empty) for heavy build directories only.
- **Game data:** referenced in place, read-only, through a configured path (`KUF_GAME_DIR`). Nothing is
  copied unless a step needs it. If an install step is ever required, it copies into `build/install/`.
- **Git:** a new local repo, branch `main`, feature branches per milestone. No remote and no push until you
  decide (recommend a private repo).

```
kuf-cod-recomp/
├─ AGENTS.md  README.md  CMakeLists.txt  CMakePresets.json  .gitignore
├─ docs/       FEASIBILITY, ROADMAP, ARCHITECTURE, ENGINE_ANALYSIS, DEPENDENCIES, TECHNICAL_RISKS,
│              GRAPHICS, NETWORKING, TESTING, KNOWN_ISSUES, PROGRESS
├─ config/     recompiler TOML (function overrides, hooks, switch tables)
├─ thirdparty/ rexglue-sdk (pinned submodule), SDL3, …
├─ src/        hand-written runtime + game patches (platform/, patches/, net/, mods/, ui/)
├─ tools/      xexinfo.py, xeximage.py, soxindex.py, shader/asset inspectors, test drivers
├─ tests/      unit tests (instruction semantics, formats), scenario tests, image baselines (no game pixels committed)
├─ generated/  (gitignored) recompiled C++
└─ build/      (gitignored) build trees, runs/ sandbox (saves/logs per test run)
```

Game-derived outputs (generated code, extracted shaders, screenshots of game content, traces) stay in
gitignored directories.

## Decisions (approved 2026-10-20)

1. ReXGlue is the **provisional** base, to be confirmed by a proof of concept on the real executable.
   **M1 result: confirmed.** The whole image recompiles with zero unimplemented instructions, builds with
   Clang, passes 1,504 tests, and runs in a sandbox far enough to start threads, audio and GPU command
   processing. See RECOMPILATION.md.
2. Workspace a local workspace (`~/Work/kuf-cod-recomp`), local git, no remote.
3. Target the original v1 executable. Keep the design open to a title-update version later (ARCHITECTURE.md).
4. The leaked 2006 source code is never downloaded, inspected or used.
5. Small commits after verified steps; no remote, no history rewriting.
