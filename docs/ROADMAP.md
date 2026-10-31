# Roadmap

Each milestone lists acceptance criteria that must be demonstrated with reproducible evidence (logs,
scenario scripts, image comparisons) before it is marked done. Order is by dependency; priorities in
brackets.

| M | Goal | Acceptance criteria |
|---|---|---|
| M0 ✅ | Feasibility | These documents; read-only analysis scripts; approval of base, workspace, TU, leaked-source decision. |
| M1 ✅ | Recompilation foundation | Workspace + CMake + pinned ReXGlue. Codegen over the whole image with **zero unrecognised instructions** and zero unresolved jump tables (or each one listed with a manual override). Generated code compiles with Clang on Linux. Instruction-semantics unit tests for every VMX128 opcode the game uses. `.embsec_` contents identified. |
| M2 ✔ (awaiting approval) | Minimal runtime | Binary reaches `main` loop: kernel init, threads, file reads from the read-only game dir (`d:`), timers. Logs show `CCircleOfDoom::InitGameFirstTime` and `initres.pak` reads. No crash for 5 min headless. Unimplemented imports logged, none silently stubbed. |
| M3 [P0] | First image | Vulkan window shows the game's first rendered frame (logo/loading). Screenshot captured by the test harness. Shader pipeline cache on disk. |
| M4 [P0] (done) | Main menu | Title movie or its skip, main menu navigable by pad and keyboard, menu SFX/BGM audible, language selection. Scripted scenario reaches the menu from a cold start 10/10 times. |
| M5 [P0] (M5A merged, M5B done) | Single-player | Create a character, load the first world, move/fight/kill an enemy, pick up loot, save, quit, reload with identical character state. M5B: saves atomic under SIGKILL, damaged saves refused, cross-run persistence tested (docs/SAVES.md). |
| M6 [P0] | Gameplay compatibility | Every world (CL, CU, DR, DU, EN, LH, RG) entered and one boss defeated per world. Inherited Xenia bugs triaged (music overlap, bloom, lava cave, Balrog). Crash-free 2-hour session. Save points in later regions (Sleep, K23) and save compatibility across worlds. |
| M7 [P1] | Display | M7a: correct output at 720p/1080p/1440p/2160p via scaled render targets, with shader fixes for this game. M7b: game-side viewport/projection hooks for arbitrary resolutions, 4:3–32:9 Hor+ FOV, HUD anchored to the 16:9 safe area, FOV slider in gameplay only (cutscene cameras untouched). Verified by screenshot sets per aspect. |
| M8 [P1] | High/uncapped FPS | Frame-time sources mapped and documented. Sim tick decoupled from render with interpolation. Measured equality of movement speed, attack timing, animation length, enemy behaviour and physics at 30/60/120/144/240/uncapped (scripted traces diffed). Cap configurable. Not "done" until the trace diffs pass. |
| M9 [P1] | Controls + settings | SDL3 KB/M with rebinding, raw mouse camera, sensitivity/invert/dead-zones, pad remap + rumble, PlayStation glyph handling. `pc_settings` file round-trips. Settings menu with live-apply and "restart required" flags. |
| M10 [P2] | Graphics/audio options | Only options that measurably change output: AF, shadow map size, LOD/draw distance (if reachable), MSAA/FXAA, motion blur/DOF/bloom toggles, vsync. Separate music/SFX/voice volumes via XACT categories, device selection. Performance instrumentation + benchmark scene. |
| M11 [P1] | LAN co-op | Network layer reverse-engineered and documented in NETWORKING.md. XNet/XSession/QoS emulation, LAN discovery, host/join from the in-game online menu, 2–4 instances on LAN complete a level together. Disconnect handled. Automated two-instance test on one machine. |
| M12 [P2] | Internet | Direct IP join, relay-free NAT guidance (port-forward/UPnP), version handshake, basic security (no unauthenticated remote code paths). |
| M13 [P3] | Invites | Optional Steam (only with an authorised AppID) / Discord activity join; core LAN/IP never depends on them. |
| M14 [P3] | Modding/polish | Loose-file override directory in front of `.pak`/`.sox`, Lua script overrides, load order file. No native plugin loading. Performance work. |
| M15 [P0 for release] | Release validation | Linux and real-Windows builds tested; full single-player playthrough; 4-player co-op session; settings persistence; CI green. |

## M1 outcome vs. acceptance criteria
- Workspace, CMake, pinned ReXGlue: done.
- Whole-image codegen with zero unrecognised instructions and zero unresolved jump tables: done (689 switch
  tables reconstructed automatically). 108 overrides, all documented (RECOMPILATION.md).
- Generated code compiles with Clang on Linux: done (RelWithDebInfo).
- Semantic tests for every VMX128 opcode the game uses: done. They cover every instruction except `mffs`
  (TESTING.md).
- `.embsec_` contents identified: **partially**. They recompile cleanly under `.pdata` and the game runs
  through them, but their library of origin has not been proven.

## M2 outcome (2026-10-20)

Acceptance as proposed in M1 (the M1 table row also mentioned a `CCircleOfDoom::InitGameFirstTime` log
line; that string was not used as evidence, since the game does not log it in retail builds as far as
observed. Archive reads plus steady main-loop frames serve as the positive evidence instead):
- **A 5-minute sandboxed run with zero unhandled faults:** met (smoke test PASS, 300 s).
- **Main-loop frames observed:** met, at 30 fps steady (8,936 frames, at least 299 per 10 s).
- **The game archives are read:** met (26 files, every boot archive plus shaders, BGM and the title movie).
- **Every stubbed import used is listed and justified:** met (docs/IMPORT_AUDIT.md).
- **Smoke test plus all existing tests green:** met (1,578/1,578 + smoke).

Delivered beyond that:
- fail-fast crash reports symbolized to guest instructions;
- the K2 root cause (`setjmp`/`longjmp`), fixed;
- guest-exception guards;
- the read-only game mount enforced on every path (SDK patch 0001);
- a clean timed exit;
- telemetry;
- the Windows investigation (not verified, docs/WINDOWS.md).

## M3 plan (proposed): first verified game image

Scope: show that the GPU emulation renders the game correctly, and make frames capturable and testable.
Still no modernisation (resolution, FOV, frame rate) and no input or gameplay work.

1. **Frame capture.** `--kuf_capture_frames=<list>` writes the presented guest frames as PNGs into the run
   directory, read back from the runtime's presenter. They're deterministic per frame index; captures stay
   in `build/` and are never committed (game content).
2. **First image.** Capture boot, logo and title-loop frames from a sandboxed run. You verify visually that
   they show the real title screen. An automated check covers non-blank output, plausible histogram and
   frame-to-frame change during the movie.
3. **Screenshot regression harness.** Local, uncommitted baselines with a perceptual-difference threshold.
   The committed part is the harness and the expected statistics, never images.
4. **Movie path.** Confirm the title-loop and attract WMV play through the guest decoder (XMEDIA + GPU
   shaders). Note any decode or colour issues.
5. **Graphics correctness review.** Log GPU warnings and errors per frame, check the shader translation and
   pipeline-cache behaviour (cache dir in the run sandbox), and record findings (e.g. Xenia's
   resolution-scale issue) without fixing modernisation items early.
6. **Hardening from M2 (done):** fail fast on `KeBugCheck`/`DbgBreakPoint` (K12); a `ctest` label for a short
   (60 s) smoke variant for routine use.
7. **Optional, with your approval:** one windowed run on your display to look at it live (my notes say
   never to use your display without explicit permission).

M3 acceptance: captured frames show the title screen as on the console (your visual confirmation),
automated image checks pass, there are no GPU errors in a 5-minute run, and all regression tests and the
smoke test pass.
