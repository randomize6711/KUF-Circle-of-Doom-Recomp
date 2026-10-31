# Technical Risks

Status: M0, 2026-10-20. Severity: impact on reaching the final goal. Likelihood: of the risk materialising.

| # | Risk | Sev | Lik | Evidence | Mitigation |
|---|---|---|---|---|---|
| R1 | **Gameplay speed is tied to frame rate** | High | Certain | Xenia label `vsync-off-speedup` | M8 work: find the frame-step root (`GetElapsedTick`/`deltaTime` consumers, PhysX `simulate` step, animation, Lua coroutines). Preferred design: keep the simulation at its native tick (30 Hz **[H]**) and add render-side interpolation via mid-asm hooks. A fallback "60 fps sim with scaled constants" needs per-system audit. This is the largest single engineering item; "uncapped" may end up as interpolated rendering on top of a fixed sim tick. |
| R2 | Recompiler coverage gaps (VMX128 encodings, jump tables, `.embsec_` code, 8,007 `twi` traps, setjmp/longjmp) | Med | Likely | 66 k VMX words; XenonAnalyse patterns tuned to one game; unmerged instruction PRs upstream | ReXGlue's per-`bctr` detection; add missing instructions with unit tests against known semantics; manual `functions` overrides; trap semantics as no-op/log. M1 acceptance = zero "unrecognized instruction" outputs. |
| R3 | Runtime-compiled shaders (HLSL/XGraphics compiler is linked) | Med | Possible | `ucode\compiler`, `ssm\*` strings; `LineCurve.fx`, `SimpleTrail.fx` path strings | ReXGlue translates microcode at runtime from PM4, so source origin is irrelevant there. Only a concern if we later move to offline shader translation. Instrument at M3 to confirm. |
| R4 | **True native resolution/ultrawide on a PM4-emulation base** | High | Likely | Xenia `draw_resolution_scale` breaks KUF shaders; EDRAM tiling/resolve tied to 720p sizes | Phase it: (a) M7a integer resolution scale fixed per broken shader; (b) M7b game-side hooks for viewport/projection/render-target dimensions (Hor+ FOV, UI anchoring) so the game itself renders at the target size; (c) if PM4 emulation blocks arbitrary sizes, add selective D3D-function hooks (Unleashed-style) for the main scene targets only. This is the main reason the D3D-hook approach is kept as a documented fallback. |
| R5 | Inherited Xenia bugs (music overlap, bloom blowout, lava cave wall, Balrog lava geometry, possible softlock) | Med | Certain | Xenia issue #331 | Track as KNOWN_ISSUES. Fix in shared runtime (XMA/XACT timing, EDRAM) where possible. Reproduce with save states via the debug console. |
| R6 | Multiplayer is Xbox Live-only (no System Link) | High | Certain | Co-Optimus; `XamSession*`, `XNetQos*`, `XMsg*` imports | Emulate XSession/XNet/QoS semantics locally: XNADDR ↔ IP mapping, a session registry over LAN broadcast, and local-only security (no XNet crypto). Requires reverse engineering the game's packet layer behind `NetDll_*`. Projects such as sp00nznet/xlive and mw-recomp-mp prove the approach for System Link titles, but none yet for a Live-only title. |
| R7 | Base version only (v1), title update absent | Med | Certain | `version=0x1`, no TU in folder; Xenia: TU fixes slow camera + shading | Ask the user whether they own the TU (`.xexp` delta). Recompiler toolchains can apply it before codegen. Otherwise fix camera speed ourselves. Pick before M1 so function addresses stay stable. |
| R8 | Threading semantics: OpenMP (VCOMP) + custom CFThreadBase + `KeSetAffinityThread` on 6 hardware threads | Med | Possible | VCOMP static lib, 336/486 `lwarx`/`stwcx.` | Atomics map to host atomics (ReXGlue does reservation emulation). Keep the guest thread count; no extra parallelism until correctness is proven (M15). |
| R9 | PhysX 2.7.2 numeric determinism vs host float/VMX semantics (denormals, `vrsqrtefp` precision) | Med | Possible | Statically linked PhysX; co-op sync depends on it | Match guest FP flags (flush-to-zero, rounding) in VMX emulation. Cross-check physics traces vs Xenia. Multiplayer is host-authoritative **[H]**, which limits desync impact. |
| R10 | XMA/XACT audio correctness | Med | Likely | XACT LTCG + `XMACreateContext`; Xenia music overlap bug | Use Xenia's XMA path via ReXGlue. Separate music/SFX/voice volumes through XACT categories (`XAudioGetVoiceCategoryVolume`). |
| R11 | Save data compatibility | Low | Possible | `XamContentCreateEx` packages; format unknown | Map content packages to a per-user directory with atomic write (temp + rename + backup). Importing 360 saves is optional (STFS extraction). |
| R12 | Upstream instability of ReXGlue (pre-1.0, API churn; antivirus flag on prebuilt DLL) | Med | Likely | Release notes/issues | Pin a commit as a submodule; build from source; keep our patches as a small, rebased patch series; never ship upstream prebuilt binaries. |
| R13 | Windows parity | Med | Possible | Development on Arch only | Plan for a Windows CI runner (GitHub Actions windows-latest + clang-cl). Real testing needs a Windows machine; no claims without it. |
| R14 | Legal/provenance (generated code, leaked source) | High | Avoidable | Leaked 2006 source on Hidden Palace | Never commit game-derived artifacts. Do not use leaked source. Distribute only the toolchain and runtime, with users supplying their own files. |
| R15 | Lua gameplay code and an embedded debug console might depend on dev-only paths | Low | Possible | Console command strings | Opportunity more than risk: plain-text Lua makes modding and test automation easier. |

## Status after M1 (2026-10-20)

- **R2 (recompiler coverage): largely retired.** 0 unimplemented instructions and 0 unresolved branches.
  Only 5 hand-checked boundaries plus 103 tool-generated pointer-only functions were needed. Every emitted
  instruction was cross-checked against LLVM. Residual: exception funclets (K4) and the NaN-sign
  divergence (K1, new, low impact).
- **R12 (ReXGlue instability): confirmed, manageable.** Wiki and CLI already disagree. The pin plus local
  CMake adaptations work; no SDK source patches were needed in M1.
- **New R16: exception handling.** The game has SEH and C++ catch funclets that are not generated yet.
  Medium impact if the game throws in normal play (unknown). Investigate in M2.
- **New R17: runtime robustness.** An unhandled guest fault loops forever instead of failing (K3). It
  hides bugs and fills disks. Fix in M2 through fail-fast diagnostics.

## Biggest unknowns to resolve early (M2–M3)
1. ~~What the `.embsec_` sections contain~~ They recompile cleanly with `.pdata`; origin still presumed XDK code.
2. ~~Whether ReXGlue codegen handles the whole image~~ Yes (see RECOMPILATION.md).
3. The frame-time source for gameplay (R1), which shapes M8 scope.
4. How the main scene render targets are allocated (R4), which shapes M7 scope.
5. Why guest thread `0xF8000070` dereferences null+0x1A4 after the first draws (K2).
