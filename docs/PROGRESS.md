# Progress

> Resuming work? Read `docs/SESSION_STATE.md` first (current branch, roadmap status, next exact action).

## M0 — Feasibility (approved 2026-10-20)

Read-only analysis of the retail files: the XEX, archives, shaders and imports, plus a survey of the
recompilation tools. Deliverables: FEASIBILITY, ENGINE_ANALYSIS, DEPENDENCIES, TECHNICAL_RISKS, ROADMAP and
three analysis scripts. Commit `70666f1` on `main`.

Note: that commit also contains `.gitmodules` and the SDK submodule pointer (`c94f5eb`). They were staged
by `git submodule add` before the first commit and could not be unstaged without a `HEAD`. The commit was
left as is, because amending would rewrite history.

## M1 — Recompilation foundation (approved and merged 2026-10-20)

Branch `m1/recompilation-foundation`.

### Implemented
- Workspace `~/Work/kuf-cod-recomp`: local git, CMake presets, ReXGlue SDK `c94f5eb` as an
  in-tree submodule. Binaries go to `build/<preset>/bin`.
- Reproducible pipeline: `tools/stage_input.sh` (SHA-256-verified copy of the XEX; analysis image and
  disassembly), `rexglue codegen` with `kuf_cod_manifest.toml` + `config/v1/`, and
  `tools/codegen_iterate.sh`.
- Translation blockers fixed:
  - 5 tail-jump-only leaf functions (hand-checked boundaries);
  - 103 pointer-only functions found by `tools/indirect_targets.py`. Two of them aborted the first
    sandboxed runs.
- Independent verification: `tools/audit_coverage.py` checks every emitted instruction against LLVM's
  disassembly; `tools/insn_test_coverage.py` reports which used instructions have semantic tests.
- Tests:
  - the SDK's instruction suite;
  - 112 generated cases for every other instruction the game uses (all but `mffs`);
  - tests that run the game's own recompiled zlib/CRC functions;
  - an expected-fail suite for the NaN-sign divergence.
- `tools/run_sandboxed.sh`: the only way to start the game (bwrap, read-only game data, no network or
  display).
- Docs: AGENTS.md, README, ARCHITECTURE, RECOMPILATION, TESTING, KNOWN_ISSUES. FEASIBILITY, DEPENDENCIES,
  TECHNICAL_RISKS, ROADMAP and ENGINE_ANALYSIS are updated.

### Tested (evidence)
| Check | Result |
|---|---|
| Codegen of the whole image | 28,908 functions, 2,365,019 instructions, 0 unimplemented, 0 unresolved, ~15 s |
| Mnemonic cross-check vs LLVM | 0 mismatches |
| Code words not emitted | 15,411: 14,285 jump tables, 840 import thunks, 67 lone `blr`, 219 in 38 runs of EH funclets/dead code |
| Build (Linux, Clang 23.1.3, RelWithDebInfo) | succeeds; generated code ~2.5 min at `-j8`, ≈260 MB per TU |
| `ctest` | **1,576 / 1,576 passed** (1,458 SDK, 112 game-driven instruction cases, 5 image tests, 1 expected-fail suite), in both RelWithDebInfo and a from-scratch Release tree |
| Determinism | Fresh codegen bit-identical across runs and CLI builds (RECOMPILATION.md) |
| Instruction test coverage | 238 of 314 used instructions have dedicated tests; the other 75 are loads/stores/branches exercised by every test; untested: `mffs` (5 sites) |
| Sandboxed run, GPU emulation off (`smoke-cpu-5`) | Runs 30 s with no fatal error: memory, function table, SDL input, audio (ALSA null), D3D device setup (`Vd*` ignored); then waits on the GPU, as expected |
| Sandboxed run, GPU emulation on (`smoke-gpu-1`) | Vulkan on RX 9070 XT; PM4 processing; ~40 pipelines built from the game's shaders; then guest thread `0xF8000070` loops on a null+0x1A4 read (K2) |

### What works
- The executable recompiles completely and reproducibly, and builds natively on Linux.
- Translated instruction semantics match the PowerPC/VMX definitions in every tested case, except NaN sign
  (K1).
- The recompiled game initialises its threads, audio and graphics device, and submits GPU work.

### What remains broken / open
K1–K9 in KNOWN_ISSUES. The main ones:
- K2: null dereference after the first draws;
- K3: fault loops without diagnostics;
- K4: exception handlers not generated;
- K5: `setjmp`/`longjmp` not configured;
- K8: Windows build untested.

Not playable, and no image has been presented yet (that's M3).

### Mistakes made and handled
- A clean-tree build linked without the generated sources, because `generated/rexglue.cmake` only adds them
  if they exist at configure time. Configure now warns, and the README documents the two-step bootstrap.
- `kuf_cod --help` was run once outside the sandbox. The app has no `--help`: it created its window or
  error box and exited on the missing `--game_data_root`. No guest code ran and no game data or user
  files were touched. The only output, a one-line log next to the binary, was deleted. All runs since go
  through `tools/run_sandboxed.sh`, and AGENTS.md forbids bare runs.
- The first funclet filter in `indirect_targets.py` wrongly rejected a real function (`0x82A08D40`), which
  showed up as a runtime abort. The rule was refined: fragments are absorbed, and only control flow into
  real functions causes a rejection.

### Files changed (M1)
`CMakeLists.txt`, `CMakePresets.json`, `kuf_cod_manifest.toml`, `generated/rexglue.cmake`, `src/*`,
`config/v1/*`, `tests/**`, `tools/{stage_input,codegen_iterate,run_sandboxed,ppc_asm_sandboxed}.sh`,
`tools/{audit_coverage,indirect_targets,insn_test_coverage,gen_insn_tests}.py`, `AGENTS.md`, `README.md`,
`docs/*`.

### Commits (branch `m1/recompilation-foundation`)
- `345f5a9` build: ReXGlue project scaffold for the v1 executable
- `55cec76` codegen: staging, coverage audit and pointer-only function discovery
- `287fa5a` tests: instruction semantics and recompiled game function tests
- `3820fc9` tools: sandboxed runner for the recompiled game
- `781642a` docs: M1 report and project documentation
- build: warn about the two-step bootstrap; docs: determinism and Release verification (the commit
  containing this line)

### Next
The M2 plan is in ROADMAP.md: diagnostics first, then K2, the import audit, `setjmp`/`longjmp`, exceptions,
file I/O checks and an automated smoke test.

## M2 — Runtime stabilisation (complete 2026-10-20, awaiting approval)

Branch `m2/runtime-stabilization` (not merged).

### Runtime initialisation status
In the sandbox with GPU emulation (Vulkan, RX 9070 XT), the game:
- initialises the CRT/XAPI, 21 guest threads, the XAudio client and XMA contexts, and the D3D device with
  its GPU ring buffer;
- reads `dependency.bin` and all its boot and world archives, the shader bundle and the BGM banks;
- plays `COD_TITLE_LOOP.wmv` and then `COD_ATTRACT.wmv`;
- runs its main loop at a steady 30 fps (the original cap) for 5+ minutes, then exits cleanly on request.

Nothing has been displayed yet: frames are presented offscreen and not inspected. That's M3.

### Null-object loop (K2): root cause and resolution
- **Where:** `sub_828E6438`, `lwz r11,420(r31)` at `0x828E648C`, on guest thread `0xF8000070` (XGraphics
  runtime shader compiler).
- **Cause:** `sub_828570E0` calls `setjmp(0x7027EB30)`, and the compiler later recovers from an error with
  `longjmp(0x7027EB30, 1)`. The guest `longjmp` restores r1/r31 from the buffer, then ends in `blr`. In
  recompiled code that `blr` returns into the current host frame instead of the `setjmp` site, so
  execution continued with setjmp-time registers until `r31 = 0` was dereferenced.
- **Category:** recompilation configuration (ReXGlue needs the `setjmp`/`longjmp` addresses). It was not a
  missing object, not GPU emulation and not threading.
- **Fix:** `config/v1/codegen.toml` sets both addresses, so all 30 call sites use native
  `ppc_setjmp`/`ppc_longjmp`. The guest bodies now fail fast if they're ever reached.
- **Regression:** the fault no longer happens (5-minute smoke test). `image.unhandled guest fault…`
  guards the crash-handling path.
- **How it was found:** the new crash report, the gdb stack-dump analysis, then `REX_HOOK_RAW` tracing of
  `setjmp`/`longjmp`/`RaiseException`.

### Xbox system-function audit
210 function imports (docs/IMPORT_AUDIT.md):
- 99 fully implemented, 19 of them reviewed by hand;
- 4 partially implemented (offline networking/XAM messaging and `RtlRaiseException`);
- 2 intentionally stubbed;
- 105 unused during boot and title;
- 0 not investigated.

The offline local profile is user 0, "User", signed in locally. Save (XamContent) and LAN-relevant
imports are listed for later milestones.

### Exception handling
- `setjmp`/`longjmp` are configured and verified.
- During boot and the title screen, guest `RaiseException` is called only for thread naming (2 calls), and
  no hardware fault or C++ `throw` occurs. So the untranslated SEH/C++ EH regions (30 funclets, 3
  continuations) are not executed in what has run so far.
- Any future guest throw or non-naming exception ends in a crash report naming the C++ type, instead of
  the runtime's silent Break-and-return.
- ReXGlue's handler generation was not enabled: there is no evidence it's needed yet.

### Filesystem and archives
- 26 files opened, none failed: every `.sox`/`.pak` except `bg_dr` (not needed at boot), `CodShaders.ccz`,
  the BGM banks and both title movies.
- The game folder is mounted read-only. Write opens are denied or downgraded to read access.
- Rename was a real hole: a guest rename renamed the host file (proven by the new test failing without
  the fix). It's fixed by SDK patch 0001.
- The app refuses `--allow_game_relative_writes`.
- Saves, logs, caches and run data go to `build/runs/<id>/{user,cache,...}` in the sandbox.
- The original folder was verified unchanged after all runs (138 files, same XEX hash, no newer files).

### Threading and timing
- 35 `ExCreateThread` calls; 21 guest threads alive at 45 s, consistent states, no deadlocks.
- 6 identical workers are presumably the OpenMP pool (inference).
- The guest timebase measures 49.996 MHz against wall time (50 MHz ± 0.01 %).
  `KeQueryPerformanceFrequency` and `mftb` share one clock.
- Present rate is exactly 300 frames per 10 s: the original 30 fps limiter, untouched.
- GPU synchronisation: the D3D device poll and VdSwap pacing are steady.
- CPU use is about 230 % at the title screen (K10, spin-waits).

### Smoke test
`tools/smoke_test.py` / `ctest -L smoke` (KUFCOD_SMOKE_TEST=ON). Final run: **PASS**:
- 300 s, 8,932 frames (≥ 300 per 10 s after warm-up);
- 26 files; 29 of 29 timebase samples in tolerance;
- 0 runtime errors; 4.5 MB of output; exit 0 via the timed path;
- 105 imports recorded, the 6 partial or stubbed ones reviewed.

A negative control fails, as it should.

### Regression tests
`ctest`: **1,578 / 1,578 passed**. That's the M1 suites plus `vfs.*` (the read-only mount) and the crash
report test, and the smoke test separately.

### Remaining blockers / open issues
K1 (NaN sign), K4 (exception dispatch, not exercised), K8 (Windows unverified), K9 (TU), K10 (CPU use),
K11 (gdb SIGTRAP), K12 (BugCheck/DbgBreakPoint not fail-fast). Nothing currently blocks M3.

### Mistakes made and handled
- I ran a game binary after a failed build once (chained with `;` instead of `&&`). It ran the previous
  binary inside the sandbox, so there was no harm. Runs are now gated, and AGENTS.md says so.
- I appended `setjmp_address`/`longjmp_address` after a `[functions]` header, where TOML treats them as
  table entries. Spotted because 0 call sites converted; moved to `config/v1/codegen.toml`.
- Several debugging dead ends, documented so they aren't repeated:
  - a hardware watchpoint on the wrong premise;
  - pending gdb breakpoints that don't resolve in the sandbox;
  - a mistaken `GetFileAttributes` theory.

### Files changed (M2)
- `src/kuf_cod_app.h`, `src/runtime/*`, `CMakeLists.txt`, `CMakePresets.json`, `kuf_cod_manifest.toml`;
- `config/v1/{codegen,imports}.txt/toml`, `config/v1/import_review.toml`;
- `patches/rexglue-sdk/0001-*.patch`, `cmake/mingw-w64-x86_64.cmake`;
- `tests/{CMakeLists.txt,vfs/*,image/*}`;
- `tools/{symbolize_crash,symbolize_gdb_dump,guestmap,gen_import_audit,import_audit_report,smoke_test}.py`,
  `tools/{run_sandboxed,apply_sdk_patches}.sh`;
- `AGENTS.md`, `README.md`, `docs/*` (new: IMPORT_AUDIT.md, WINDOWS.md).

### Commits (branch `m2/runtime-stabilization`)
- `52eda08` diag: fail-fast crash reports with guest symbolization
- `5aaf1a1` fix(K2): native setjmp/longjmp; fail fast on unsupported guest exceptions
- `8d50fad` telemetry: progress evidence, timed clean exit, import audit option
- `ef3eb0e` fs: keep the game folder read-only on every path; crash-handler test
- `13100de` smoke: 5-minute sandboxed smoke test, import audit classification
- `afabe3e` windows: MinGW cross-configure probe; keep tests portable
- docs: M2 report (the commit containing this section)

### Next
The proposed M3 plan is in ROADMAP.md: frame capture, first verified image, a screenshot harness, the
movie path, graphics correctness review, and K12 hardening.

## M3 — First verified image (complete and approved 2026-10-20, merged)

Branch `m3/graphics-validation` (not merged).

### Frame capture and first image
- `--kuf_capture_frames=<list>` / `--kuf_capture_every=N` write presented guest frames as PNGs plus
  `captures.jsonl` into the run directory (never committed).
- **K13, title movie frozen: fixed** by SDK patch 0002. Shared memory had marked the movie-plane range
  valid before it was allocated, so no write watch could exist; new physical allocations now invalidate
  the GPU's view. The pre-fix run also showed the attract movie as 76 s of black.
- Captured, in order: boot logo (frame 60), a fade, the animated title screen with "Press START" (from
  frame 300), after ~70 s the attract movie (`COD_ATTRACT.wmv`), ~5 s of black, the title loop again.
  Captures are bit-identical between runs.

### Image validation
`tools/capture_check.py` (`check`, `baseline`, `compare`) and `tools/capture_test.py`: PNG structure and
dimensions, blank frames, per-range statistics from `tests/capture/title_v1.toml` (statistics only),
frame-to-frame change (catches K13), bounded fades, the attract movie actually shown, and a block-averaged
perceptual comparison against local references (`build/references/v1-title/`, created after the run was
checked). Self-test: 14 cases on synthetic images. Negative control: the pre-fix run fails.

### GPU stability (5-minute sandboxed runs, RADV RX 9070 XT)
- No errors or critical lines. GPU warnings: one ("skipping presentation due to async placeholder draw")
  at boot, while a pipeline compiled asynchronously.
- About 30 graphics pipelines created over the run, none failed; no shader-translation, texture-upload or
  render-target errors. The YUV movie formats `k_Y1_Cr_Y0_Cb_REP`/`k_Cr_Y1_Cb_Y0_REP` use the runtime's
  supported fallback format (info level).
- Memory: resident 1,082–1,083 MB from 60 s to 300 s (+1 MB). Frame rate: ≥ 299 frames every 10 s.
- Animation: 29 captures across 5 minutes, all valid, never frozen (min change 2.24), title/attract
  cycle twice. The smoke test now checks all of this on every run.

### K12: fatal kernel calls
`KeBugCheck`, `KeBugCheckEx` and `DbgBreakPoint` now end the run with a crash report (exit 70): code,
parameters, guest call site, containing function and thread, plus registers and guest backtrace. The game
has not called them so far. Regression tests fail without the fix (verified).

### Windowed live test (authorised, one run, `build/runs/m3-window-1`)
`tools/run_sandboxed.sh --window` binds only the Wayland socket (no X11, audio or network) and passes
`--fullscreen=false`; 120 s hard limit, timed exit at 110 s. Result: SDL Wayland driver, a Vulkan swapchain
on the window (1904×1043, resized by the compositor to 1890×1029), 3,233 frames presented (300 per 10 s
after boot), clean exit 0 via the window-close path, no errors, the same single boot warning as offscreen,
RSS 1,083 MB. Its 10 captures (guest output, not the window contents) pass the capture check, covering
the title loop and the start of the attract movie. What the window actually showed is for you to confirm.

### Tests
Full suite **1,585/1,585 passed** (`ctest -j8` with the smoke and capture game runs): smoke PASS (8,932
frames), capture test PASS with an exact match to the local references.

### Remaining graphics issues / not verified
- Only boot, title and attract movies are covered: menus (M4) and 3D gameplay rendering are untested.
- Colour accuracy is not compared against real hardware; it rests on your visual inspection.
- The ~5 s of black between the attract movie and the title loop is not verified against a console.
- The one skipped presentation at boot (async pipeline) is cosmetic; a shader cache warm start was not
  measured.

### Mistakes made and handled
- The first expectations file assumed the title loop plays forever; the 5-minute run showed the attract
  cycle. The expectations now model it (bounded fades, bright attract frame required) without weakening
  freeze detection.
- An external RSS sampler matched no process inside the sandbox; RSS is now recorded in-process.
- K12 tests failed when several cases shared one process: Catch2 replaces the runtime's signal handlers
  per test case. Forked children now restore them; the M2 crash test had the same latent issue.

### Commits (branch `m3/graphics-validation`)
- `a1e9382` fix(K13): invalidate GPU caches when physical memory is allocated
- `d47c4cb` capture: offscreen frame capture and capture statistics
- `64d6ca2` diag: per-call logging of selected imports; overridable log limits
- `b255958` fix(K12): KeBugCheck, KeBugCheckEx and DbgBreakPoint end the run with a crash report
- `a9224af` capture: screenshot regression harness
- `84c4c74` smoke: resident-memory telemetry, image checks, GPU warning summary
- docs: M3 report (the commit containing this section)

### Next
Your visual acceptance of the captures; then your decision on the windowed live test. M4 not started.

## M4 — Interactive menus (complete and approved 2026-10-20, merged)

Branch `m4/interactive-menus` (not merged). Details: docs/INPUT.md, docs/ENGINE_ANALYSIS.md §11.

### Input architecture
The game reads input only through `XamInputGetState`, once per frame (`sub_825EAD58`), and edge-detects
buttons itself. Translation therefore happens at the XAM input layer: a project-owned keyboard/mouse device
(`src/input`, pure core `KeyboardPad`) and the SDK's SDL controller driver both feed guest user 0, merged by
the SDK's input system. No menu code is patched. One read-only hook reads the game's own "waiting for START"
flag so that Enter can mean Start on the title screen and A everywhere else.

### Controls verified (sandboxed runs with scripted input)
- Keyboard: arrows navigate, Enter confirms (and passes "Press START"), Escape goes back, Space = A,
  F1/Pause = Start, Tab = Back; held keys use the game's own auto-repeat; OS key repeat, focus loss and
  sub-frame taps behave as specified. All bindings configurable (`--kuf_bind_*`).
- Mouse: wheel = one menu step per notch, left click = accept, right click = back; pointer mapped through
  the letterbox for any window size. No hover-to-select (K15).
- Controller (SDL virtual Xbox 360 and Switch Pro pads through the SDK's real SDL driver): Start, A, B,
  D-pad and left stick; disconnect with a button held releases it; keyboard keeps working; reconnect works;
  `--pad_face_buttons=label` for Nintendo layouts (SDK patch 0003). Real hardware: not tested (K17).

### Menu screens reached
Press START → main menu with the game's storage-device prompt (Yes → device selected by the runtime →
notice → main menu; No → main menu) → SETTINGS, LANGUAGE (8 languages), NEW GAME → character selection
(changing characters, "Play as this character?"), B → back to Press START, which accepts Enter again. No
profile screen exists: the runtime's offline profile (user 0, signed in) is used. CONTINUE/LEADERBOARD are
greyed out without a save. No game was started.

### Tests
- New: `input.*` 17 unit tests; `menu.*` 5 scenarios (opt-in, `-L menu`); capture checker `[[highlight]]`.
- Cold start to the main menu: **10/10** (`reach_menu`, three runs in parallel at a time).
- Full suite **1,607/1,607 passed** (smoke, capture and menu game runs included; menu runs match local
  references exactly). Smoke: 8,935 frames, RSS +1 MB after warm-up, 29/29 captures valid, 1 GPU warning.
- The original game folder is byte-identical to the SHA-256 manifest taken before M4 (138 files).

### Remaining limitations
K14 (sandbox audio unpaced), K15 (no mouse hover), K16 (menu sound effects not verified by ear), K17 (real
controllers untested: needs your approval to bind one device node). Enter = Start only at the title
prompt; in gameplay (M5) Enter is A, and pause is F1/Pause.

### Mistakes made and handled
- Enter first sent A and Start together; the title screen rejects that. Staggering them answered the
  storage prompt by accident. Replaced by the game's own START-prompt flag.
- An ALSA file-plugin audio capture wrote 45 GB in 40 s (the null sink is unpaced); deleted at once, the
  option reverted, replaced by the paced level meter.
- Reused run ids appended logs and capture indexes across runs, which could let an older run satisfy a
  log check. Run directories are now renamed (never deleted) before reuse, and all game-run suites were
  rerun from fresh directories.
- Two capture frames lay beyond a run's end under GPU contention; scenario frames now stay well inside.

### Commits (branch `m4/interactive-menus`)
See `git log main..m4/interactive-menus`.

## M5A — First playable gameplay (approved 2026-10-21, merged to main at e78918a)

Branch `m5a/first-playable-gameplay`, merged with the fail-closed procedure (see M5B).

### Merge of M4 (note)
The pre-merge suite had one failure (menu.keyboard_mouse) that my chained command did not stop on, so M4
was fast-forwarded with that failure unexplained. It was investigated first thing in M5A: an intermittent
invalid free in the game's D3D release (K18, refused safely by the runtime), plus movie-timing differences
in reference comparisons once audio is paced (compare now skips the animated logo). No M4 code was wrong.

### World loading
New game -> Kendal -> solo game -> saving -> loading -> intro cutscene -> **Forest of Embracing** with its
tutorial; the character save is written to the run's user folder. No loading failures, crashes or missing
assets. Character, terrain, foliage, ruins, lizardmen, HUD and the game's own prompts render correctly in
captures (no visible corruption); particle effects were not specifically examined.

### Gameplay input
- The game's input-target handle gives a third context, gameplay, next to "Press START" and menus. Keys keep
  their press-time meaning across context changes.
- Keyboard/mouse in gameplay follow the game's own Controls page (docs/INPUT.md): WASD move, mouse camera,
  left click A weapon, right click X weapon, Space Dash (B), E pick up (Y), Escape options menu, Tab status
  menu, Q/R items, Z aim, F RT ability, middle click centre camera. Menus keep the M4 bindings.
- Mouse look only in gameplay, calibrated to the measured camera response (dead zone ~12,900, then linear):
  steady, proportional turning (20 px/frame of mouse -> ~10 px/frame of view). Real pointer lock is
  unavailable offscreen (K20).
- Controller: 1:1 as on the console; verified with the virtual pad (status, inventory, both sticks,
  attacks, pause/resume). Physical Elite Series 2 in gameplay: your test (K17).

### Combat, loot, inventory
Verified on the game's own screens: EXP 0 -> 9 (and 11 in another run), HP loss (down to 3/78 once), Gold
100 -> 108, picked-up Small Red Potion, Mace, Siege Bow of Puncture, Iron Ring; inventory, item details and
synthesis information shown. No balance or loot changes.

### Stability and performance
- 37-minute gameplay session: 64,728 frames, clean timed exit, RSS 1,376-1,386 MB from minute 5 to the end
  (+10 MB), no crash, no unexpected errors (one known K18 line).
- 30 fps in exploration and menus; 18-25 fps in large fights (K19). CPU about 210-225 %.

### Tests
- New: `gameplay.*` 3 scenarios (opt-in), input unit tests 22 cases, capture checker 17 cases; script anchors,
  repeat-until and labelled captures.
- Full suite **1,615/1,615 passed** (smoke, capture, menu and gameplay runs included).
- Original game folder byte-identical to the manifest (138 files).

### Remaining
K14 (pacing side effect on the title movie), K17 (physical pad in gameplay), K18 (intermittent invalid
free), K19 (fight frame rate), K20 (pointer lock offscreen), gameplay sound effects and music (your test).

### Commits (branch `m5a/first-playable-gameplay`)
See `git log main..m5a/first-playable-gameplay`.


## M5B — Save/load reliability (complete 2026-10-21, awaiting approval)

Branch `m5b/save-load-reliability` (not merged). Details: docs/SAVES.md.

### Merging M5A: fail-closed procedure
`tools/safe_merge.sh <branch> [--new-branch <next>]` is now the only merge path (AGENTS.md rule 5): clean
tree (SDK at its pinned commit with exactly our patches), fast-forward possible, both trees built, the full
ctest suite at 100 % with at least 1,615 tests and nothing skipped, HEAD and tree unchanged afterwards, the
game folder byte-identical to its manifest (`tools/verify_game_dir.sh`, also catches added or removed files),
no game content tracked (`tools/check_tracked.py`); only then `git merge --ff-only`. One `set -e` process
with an ERR trap: any failure prints `MERGE ABORTED` before the merge line. It stopped M5A three times:
1. `gameplay.combat_loot` failed: no loot picked up in a two-minute fight. Fixed by a three-minute fight and a
   pick-up sweep (df87a1f).
2. It failed again: gold was picked up but the potions found were used during the fight, so the
   inventory-only check was wrong; and its background-sensitive diff could pass without loot. Loot is now
   "Gold number changed or items listed" (`any_of`, glyph-pixel `text` metric), validated on six recorded
   runs: the two runs without loot fail, the four with loot pass (dff25fc).
3. All 1,615 tests passed, but the gate stopped silently: this ctest prints "100% tests passed out of N",
   which the summary parser did not accept (failing closed, as intended, but without a message). Fixed and
   given an ERR trap (e78918a). The fourth gate run merged M5A (fast-forward, main e78918a) and created this
   branch.

### Original save behaviour
See docs/SAVES.md §1. One package for all characters; created with CREATE_ALWAYS only when none exists,
otherwise opened with OPEN_EXISTING; mounted for the whole session and never closed; every save is a file
rewrite plus `XamContentFlush`. Saves at character creation, world load, end of tutorial, sometimes during
play, and on Quit > "save and return to the Title Screen" (the manual save). CONTINUE lists the package's
characters. Current HP is not saved (a loaded character has full HP); EXP, gold, max HP, SP, Luck,
equipment and inventory are. The game does not validate saves (§4).

### Save-system architecture (SDK patch 0005)
Staging copy per open package; commit on flush/close by an fsynced complete copy swapped in with
`renameat2(RENAME_EXCHANGE)`; CREATE_ALWAYS keeps the old package until the replacement is committed;
manifests (size + SHA-256) verified on open, damaged packages refused (the game shows its own
corrupted-save message) and kept aside if the game deletes them; recovery of interrupted sessions on the
next start (never applied, kept for diagnostics); `[content]` log lines for every content call. Saves live
in their own directory (`--content_root`), never in the game folder (app check, read-only mount, sandbox
check). A CLI parse error now ends the run (it used to drop every option, the save location included).

### Tests
- `save.*`: 15 unit tests (666 assertions), including 40 SIGKILLs during commits: the package is always
  one complete, verified version.
- `savegame.*`: 11 chained game runs (docs/SAVES.md §5): create, fight, loot, unequip, save via Quit;
  CONTINUE in a new process with every value, equipment slot and inventory item compared; a second change
  and save loaded by a third process; SIGKILL after the game wrote its save and during the commit, each
  followed by a restart that loads the last valid save and saves again; changed byte, truncated file,
  player-chosen delete, missing character file.
- `input.*`: script files and `repeat ... press`.
- Input-script files: `--kuf_input_script_file`, `tools/run_sandboxed.sh --input-script`, `script_file` in
  scenarios; the save scenarios use them (`tests/save/*.kufscript`, up to 376 lines).

### Commits (branch `m5b/save-load-reliability`)
See `git log main..m5b/save-load-reliability`.

### Regression (2026-10-21, head 111e3c5 + these notes)
Full suite, both trees built: **100 % tests passed out of 1642** (1,615 from M5A + 15 `save.*` + 1 `input`
script-file case + 11 `savegame.*`), nothing skipped; 3,699 s wall time. Game folder unchanged (138 files
match `build/input/v1/game_dir.sha256`); `tools/check_tracked.py`: 132 tracked paths, no game content.

### Follow-up before acceptance: K22 and the manual save test (2026-10-21)
- **K22 investigated** (docs/SAVES.md §1, §3 item 8): every save after character creation is message
  0x41127 of the save manager's handler `sub_826EFA38`: character file + `XamContentFlush`, a check that the
  storage device is present, GlobalSetting (counters shared by all characters) + `XamContentFlush`. Found by
  logging which files each commit changed and the guest call chain of every flush (a temporary probe, since
  removed), then reading the handler. The two steps are one logical save; on the console each flush is
  durable alone, so an interruption between them mixes a new character file with the previous
  GlobalSetting. The game ignores flush results.
- **Fix**: the app wraps that message in a flush group (`src/runtime/save_groups.cpp`; SDK patch 0005
  `BeginFlushGroup`/`EndFlushGroup`): both flushes are held and committed once at the end of the message.
  Same files, same content, only the commit point moves; `--kuf_atomic_saves=false` restores the console's
  per-flush commits.
- **Tests**: `savegame.save_kill_between` (SIGKILL at the save's second flush: package byte-identical to the
  previous save, the new character file only in the uncommitted copy), `save_after_kill_between` (restart:
  recovery logged, stage 1's values load, saving commits both steps as one), `save_kill_between_split`
  (control, atomic saves off: the character file alone was committed). `merge.safe_merge_fails_closed`:
  the merge gate run in a throwaway repository with 11 injected failures (each aborts, main unchanged) and
  one passing case.
- **Seen while investigating**: one save_new exploration run died in the fight ("Game Over"); the game saves
  at death and the scripted status-screen steps then went astray. A random death in the scripted fight can
  fail `savegame.save_new` and `gameplay.combat_loot` (risk noted, not seen in the gate runs).
- **First full regression after the fix failed** (build/logs/k22-ctest.log: 1637/1646, 5 savegame scenarios
  failed, 4 not run). Cause: the fight's randomness (K24), not the K22 change: save_new passed with HP 6/78
  and other drops, and save_continue re-equipped RB by list position (it landed on a crossbow) and compared
  the HP maximum at a fixed position a one-digit current HP moves. Fixed in the tests (85b676a: select the
  first entry, a potion; capture_check `align_x = "right"`/`keep_px` for the HP maximum, with a self-test
  case). Re-verified on that same save: all 9 dependent scenarios pass.
- **Regression** (85b676a, build/logs/k22-ctest-2.log): **100% tests passed out of 1646**; no test failed,
  not run, disabled or skipped. Game folder unchanged before and after (138 files); tracked content clean
  (137 paths).
- **Manual persistent-save test** (docs/SAVES.md §6; windowed, audio, Elite Series 2, no network, saves in
  build/manual-saves): session 1 (`m5b-manual-1`) created a character (Celine, CL_0878), played, fought,
  collected items and saved via Quit > save; every save after creation committed both files as one
  (flush group). Session 2 (`m5b-manual-2`, saves backed up first to build/save-backups/) loaded it via
  CONTINUE: the leftover working copy held no uncommitted changes and was removed; no errors, exit 0; no
  save made, CL_0878 byte-identical to the backup. The user checked the loaded character and reported that
  everything matched the saved state (level 2, EXP 70/305, gold 117, Forest of Embracing, weapons,
  abilities, potions in LB/RB, armour). **Manual test passed (user-reported, 2026-10-21).**

## M6: campaign compatibility (in progress, branch m6/campaign-compatibility)

Approach (agreed 2026-10-21): the player plays forward in sandboxed, instrumented windowed sessions; saves at
new areas become read-only fixtures for automated CONTINUE/save/reload tests; I work on the known issues in
between. No gameplay triggers bypassed, no progression state fabricated.

### Stage 1 (2026-10-21)
- **M5B merged** with tools/safe_merge.sh (all gates, 1646/1646; main 3c7f754); user's saves and backups
  byte-identical before and after.
- **Play session m6-play-1** (33 min; window, audio, Elite Series 2): Forest of Embracing -> Shroud of Fog ->
  Shadow Depths; first sleep at an Idol, dream world, quest taken; Celine level 2 -> 6. tools/session_report.py:
  30.0 fps median (min 27.3, no interval below 27), CPU 296 % median, memory 1347 -> 1469 MB (+122 MB, new areas
  loaded), 0 errors, 14 saves each committed as one transaction. 16 screenshots: normal rendering throughout.
- **Campaign tests** (tests/campaign/, fixture build/m6-saves/celine-shadows-depths-1, read-only, not in Git):
  CONTINUE lands in "Forest of Embracing: Shadow Depths" with level 6, HP 118/118, SP 51/51, Luck 51, EXP 83/1162,
  Gold 232, the same weapons, items, armor, inventory and the Spirit Crystal quest; after Quit > save and a
  restart everything reads the same. 31 non-empty screen checks per stage; negative control fails every row.
- **K23 resolved**: Sleep works only at an Idol (greyed out elsewhere); the player's sleep, dream world and quest
  worked and were saved.
- **K25 found and fixed** (480c9ba): 14 of 24 screen checks of the M5B save chain compared empty rectangles;
  fixed and guarded (an unchanged-text rule on an empty rectangle now fails). Savegame chain 14/14 with real checks.
- **K18 characterized** (not fixed): the failing free comes from the shared free call of resource types 1/2/...,
  not texture mips; caught twice: a first release of a block that was never allocated, not a double release.
- Original saves: 18 files byte-identical before and after all of the above (sha256).
- **Full regression** (build/logs/m6-ctest-1.log): 1648/1649; menu.sdl_events missed its last capture (frame
  1600) because the run ended at frame 1598. Its 58 s run left a margin of 0-2 frames (the earlier runs reached
  1601 and 1602), so this is a test-timing margin, not a game fault. Raised to 62 s; it passes again. The other menu
  and gameplay scenarios have at least 230 frames of margin. Game folder verified before and after (138 files).
- **Player confirmation** (2026-10-21): the player confirmed that the Shadow Depths save loads with the correct
  character state (the values listed under Campaign tests above).
- **Priorities revised** (2026-10-21): extensive campaign and quest testing is paused. Next milestone: M6A
  (performance profiling and optimization; investigation of the original menu architecture). All player-facing
  settings in later milestones go into the game's own interface; no external launcher, overlay or ImGui menu.
- **Full regression after the sdl_events fix** (build/logs/m6-ctest-2.log): 1649/1649 passed, none skipped; game
  folder unchanged before and after; the player's original saves and backups unchanged (18 files, sha256).

## M6A: performance profiling and original menu architecture (branch m6a/performance-and-menus)

Authorized 2026-10-21 after M6 verification (1649/1649; M6 not merged, awaiting approval). Two objectives:
profile and optimise performance; investigate the original menus and how to add PC settings to them.

### Performance (docs/PERFORMANCE.md)

- **Tools**: `--kuf_profile` (frame times, CPU per thread, GPU busy), `--kuf_profile_samples` (CPU samples
  from perf events on the process's own threads), `--gpu_cp_stats` (GPU command processor time; SDK patch
  0006), `tools/profile_report.py`, `menu_test.py --game-arg`; workloads `tests/perf/`; tests
  `perf.profile_smoke`, `tools.profile_report_selftest`.
- **Fixed**: the silent-run audio pacer stalled the game from inside its audio callback (menus 290 instead of
  300 frames per 10 s; exploration 23.3 fps; K19's fight slowdown). Device-like pacer: exploration 29.99 fps
  (p99 36.0 ms), the K19 fight 30.00 fps (p99 35.5 ms, worst second 35.0 ms). K19 resolved as a test
  artifact.
- **Fixed** (SDK patch 0007): alertable waits on several objects spun a whole core (truncated sleep); the
  audio worker 80 % -> 0.7 % of a core.
- **Capacity** (vsync off, a measurement only): 123-198 fps in the fight; first limit the runtime's GPU
  command processor thread (94 % of a core), then the GPU (84 %). The recompiled game code is not the limit.
- **Characterised**: the 30 fps cap as seen inside (D3D ring-buffer spin, per-frame WAIT_REG_MEM); kernel
  timer behaviour (K27).
- **Shader/pipeline hitches (K26), measured**: every sandboxed run starts with an empty cache: the fight 68
  skipped presentations and 30 guest frames over 50 ms, exploration 50 and 28, mostly in the first two
  minutes. With the previous run's cache (`--cache-from`, new): 227 pipelines created at start, 3 / 1
  skipped presentations, 3 / 2 frames over 50 ms. Players keep the cache (`<user data>/cache`), so they see
  these hitches once; pre-compilation is an M6F option.

### Menu architecture (docs/MENU_ARCHITECTURE.md)

- Native code on data-driven "Blueside UI" layouts (`.uitb`), controls bound by numeric ID; Lua not involved.
- Both Settings menus (title and in-game) found; rows, widgets (row, slider, toggle, selector, action),
  input, sounds, text and lifecycle traced (`--kuf_trace_menu`, `tests/menu_arch/`).
- In-game Settings (Back button in play > Settings) traced: 8 rows, one event handler (`sub_82663FA0`),
  cues, save on Back only after a change.
- Original options persist in `GlobalSetting`: camera, SFX volume, skin, blood, brightness and vibration
  offsets verified by controlled changes (title and in-game Settings), music and voice volume by their
  defaults; written via the game's own save message when Settings is left after a change.
- Recommended extension: isolated hooks in the Settings functions plus rows cloned from the scene's own
  controls; PC settings in a separate, atomically written settings file of the port.

### Menu integration feasibility (remaining instructions, 2026-10-22)

- **Layout format mapped** on all 24 files (scene, button, list box, text, image records; placements in a
  1280 x 720 space); loader, readers, definitions vs live controls, lifecycle, live control fields, the
  in-game Settings handler (row array, bounds, input codes, highlight code) documented.
- **Runtime control creation demonstrated**: controls added to a layout in memory (the file is untouched)
  are created, drawn and destroyed by the game itself.
- **Proof of concept** (`--kuf_pc_settings_menu_poc`, off by default): a ninth row "PC Test Row" in the
  in-game Settings, cloned from Vibration, navigated through the original highlight code with the original
  cue, On/Off drawn like the original toggles, keyboard and controller; no effect, not saved. The original
  rows behave identically with it on (GlobalSetting byte-identical); leaving Settings with only the new row
  changed writes nothing. Not done: title SETTINGS, persistence, translations.
- **GlobalSetting**: all eight option offsets known (camera, three volumes, skin, blood, brightness,
  vibration).
- **Persistence design** for `pc_settings.toml` (location, validation, unknown keys kept, atomic writes,
  `.bak`/`.bad`), and a proposed PC Settings menu structure per milestone.

### Camera (docs/CAMERA.md)

- Camera behaviour is per-camera data (`d:\data\camera\*.cam`, 348-byte keys, loaded by `sub_8252D0C8`);
  the executable's debug editor names the free-camera parameters.
- Original free camera measured: continuous 360-degree yaw; pitch up a little, down not at all; pitch
  returns to default on release.
- **Verified**: the vertical limits are key fields +228/+232 (raising them in memory lets the camera look far
  up and down). +236/+240 are not the vertical recentring. M6B plan drafted (not started).

### Verification

- Full regression after the profiler, audio pacer and patches 0006/0007 (build/logs/m6a-ctest-1.log):
  1651/1651 passed, none skipped; game folder unchanged before and after; the player's original saves and
  backups unchanged (18 files, sha256); `check_tracked` clean.
- SDK patches 0001-0007 applied to a clean SDK checkout reproduce the working tree byte for byte.
- Final full regression with all M6A commits (aea783f, build/logs/m6a-ctest-2.log): 1651/1651 passed, none
  skipped; game folder unchanged before and after; original saves unchanged (18 files); `check_tracked` clean.
- Regression after the PoC, camera diagnostics and docs (ef8f8d5, build/logs/m6a-ctest-3.log): **1656/1656
  passed**, none skipped (1651 earlier tests plus the 5 `ui.*` layout tests); game folder unchanged before and
  after; original saves unchanged (18 files); `check_tracked` clean.

M6A accepted by the user (2026-10-22, not merged).

## M6B: modern camera and native PC Settings (branch m6b/modern-camera, from M6A)

Authorized 2026-10-22: a modern third-person camera (free yaw and pitch, direct mouse, adjustable controller
response, recentring option) with its settings in the game's own Settings menus, stored in `pc_settings.toml`.

### Root cause of the camera restrictions (docs/CAMERA.md section 3)
- The gameplay cameras' vertical free-camera mode 3 is **absolute**: the pitch follows the stick position times
  the key's limit (-0.05..0.18 rad) and eases back to 0 when the stick is centred: the tiny range and the snap
  back. The stick reader also turns a vertical value below 0.1 into 0.1 (drift up while turning).
- Horizontal turns go through the game's smoothing, which keeps about 40 % of each turn, spread over several
  frames: the slow, soft turning (69 degrees per second at full deflection).
- Found with: the camera files of all eight characters, gdb write/read watchpoints, a field watcher, call traces.

### Implementation
- **Camera** (`modern_camera.cpp`, `camera_math.cpp`): own stick shaping (radial dead zone, power curve), mouse
  counts straight to the camera (`TakeMouseLook`), speeds per second of real time; the view direction and the
  view height are held and imposed on the game's gameplay cameras each frame (the game's own turn value is set
  to the exact remaining difference; the pitch is applied after the game's geometry step and before its
  collision step); the game's own systems see the unpitched orientation, the renderer the pitched view; event
  cameras untouched, after them the view starts from the game's. Modern Camera Off = the original exactly.
- **PC Settings** (`pc_settings_menu.cpp`, `ui_layout.cpp`): a PC Settings row in both the in-game Settings and
  the title SETTINGS (layouts extended in memory), opening a page that reuses the original eight rows and
  widgets: Mouse Horizontal, Mouse Vertical, Stick Sensitivity, Stick Dead Zone (sliders), Vertical Range
  (selector), Mouse Y-Axis, Camera Recentering, Modern Camera (toggles), Restore Defaults. docs/SETTINGS.md.
- **Persistence** (`pc_settings.cpp`): `pc_settings.toml` in the user data directory; validated, damaged files
  kept aside, unknown keys kept, atomic writes with a `.bak`, written on Back after a change.
- **Tests**: `camera.*` and `settings.*` unit suites, 11 sandboxed scenarios (`-L camera`, `-L settings`),
  `input.*` direct mouse look; tools/menu_test.py checks `log_value`, `pc_settings_expect`,
  `pc_settings_absent`, `pc_settings_from_run`/`pc_settings_file`, `--gdb`; run_sandboxed.sh
  `--pc-settings-from`.

### Verification
- First full regression (build/logs/m6b-ctest-1.log, stopped after the failure): `savegame.save_new` failed:
  the run stayed at the tutorial's camera lesson ("click R to center the camera behind you"), because the modern
  camera held its direction against the game's own centring turn. Fixed (the game's own turns are taken: a
  deviation larger than the last correction) and covered by `camera.camera_mouse`; save_new, camera_mouse and
  camera_stick pass again.
- Second full regression (build/logs/m6b-ctest-2.log): 1683/1688. Fixed and re-verified:
  - `gameplay.combat_loot` (reproducible: original camera passed, modern failed 3 of 4): the tutorial route
    depends on the game's own camera motion. The original camera trails behind the walking character and its
    lesson cameras use their own heights; the modern camera held its direction and height. Now the player's
    turns are imposed exactly and then the game's own motion runs again, and the height is the game's until
    the player moves it vertically. Same yaw timeline as the original on the route; combat_loot passes.
  - `camera.camera_cutscene`: when gameplay starts the game turns its camera 170 degrees behind the character;
    the test now checks that this turn is taken and nothing is added (it expected the old held view).
  - `menu.keyboard_mouse`, `menu.controller`, `menu.sdl_events`: their local reference images (M4) have no
    PC Settings row; only the SETTINGS frames differed (2.6 % of blocks). New reference sets recorded after
    inspection (`references = "v1-menu-<name>-m6b"`; the M4 sets are kept unchanged); every other frame still
    matches the M4 references.
  - `camera.camera_stick` dead-zone check: the game's own idle camera motion now passes through (0.34 degrees in
    2 s); tolerance +/-1 degree (a stick turn would be 30+).

- Third full regression (build/logs/m6b-ctest-3.log, 842b165): **1687/1688**; game folder and the player's saves
  unchanged. `gameplay.new_game`'s mouse-look shift check measured +113 px (sub-windows -37..+135): the modern
  camera turns 6 degrees in the 3 frames between its captures (4x the M5A calibration) and the camera stood in
  foliage whose parallax made the measurement ambiguous. The check is unchanged; the test's mouse input is scaled
  to the new sensitivity (5 counts per frame: about 30 px, as calibrated). new_game: 3/3 (-19, -20, -20 px).

- **Final full regression** (build/logs/m6b-ctest-4.log, efb56e8): **1688/1688 passed**, none skipped (1656 before
  M6B + 10 camera and 9 settings unit cases, 1 ui and 1 input case, 11 camera/settings game runs); game folder
  unchanged before and after; the player's saves and backups unchanged (18 files, sha256); `check_tracked` clean.

### Orbit correction (after the user's first manual test, 2026-10-22)
The user found that looking up stopped the camera at a minimum height and tilted it in place (the character
left the view, with a snap). Root cause (docs/CAMERA.md section 4, measured per frame with
`--kuf_trace_camera_orbit`): the port's own pitch code held the eye 40 units above the camera target and then
tilted; the game's collision kept the eye 150 units above the ground (parameter 248); and the game's position
smoothing and the collision's height smoothing moved the camera towards the orbit along straight lines, so it
lagged and on fast moves passed close to the character. Changes:

- `ApplyOrbit` replaces `ApplyPitch`: a pure orbit around the game's camera target in both directions, always
  aimed at it, below it when looking up, with a soft ground limit 30 units above the feet and the camera then
  coming in along the ground.
- While the view is pitched: the game's position smoothing is done on the game's own eye before the orbit
  (the game's smoothing state is handed the result); the collision's ground clearance is 30 instead of 150 and
  its height smoothing is undone during vertical input. Unpitched, and with Modern Camera Off, the game's
  code runs untouched.
- New test `camera.camera_orbit` (every frame, slow and fast, mouse and stick; 39 checks).
- The collision's lowered clearance applies to its ground ray only (its two reads of parameter 248 for the
  ceiling ray keep 150): a first version lowered both.
- `camera_mouse`'s collision check now compares the rendered distance at the lower limit with the start
  (707 -> 616) instead of rendered/requested: with the smoothing done before the orbit, the collision's pull-in
  reaches the game's own camera before the orbit, and that ratio stays 1.000 (the captures are identical to
  the run before the change). `camera_orbit`'s stick segment bounds the per-frame step change by the largest
  step (45): the script starts and releases the stick at full deflection within one frame.

Measured (`camera_orbit`, before = the first M6B camera, after = the correction):

| | before | after |
|---|---|---|
| slow up to the limit: aim off the character, max | 87.5 degrees | 0.03 degrees |
| slow up: lowest eye above the character's feet | 219.9 | 30.0 |
| slow up: largest eye movement per frame (1 degree per frame) | 7.4 | 34.5 (sliding in along the ground) |
| fast flicks: aim off the character, max | 91.2 degrees | 0.00 degrees |
| fast flicks: closest to the pivot | 391.8 (tilting; with the tilt removed but the smoothing kept: 73.7) | 150.5 |
| rendered pitch behind the requested one, fast | with the tilt removed but the smoothing kept: still 10 degrees short 12 frames after a jump to the limit | none (same frame) |

- **Orbit-correction regression** (build/logs/m6b-ctest-5.log): **1691/1692**; game folder unchanged
  before and after; the player's saves and backups unchanged (18 files, sha256). The one failure,
  `menu.reach_menu`, passed every check but its process ended with 137 after the requested exit (K30, new;
  1 run of 4: the three reruns passed). Total 1,692: 3 more camera unit cases than at efb56e8 and
  `camera.camera_orbit`.

### Final camera settings (after the orbit was approved, 2026-10-23)
The user approved the orbit ("feels perfect") and asked for three additions, without changing the approved
behaviour: Camera Distance, Field of View and a much higher sensitivity range, on the native PC page.

- **Distance** (50-200 %, steps of 10, default 100 % = the game's own): the game's camera offset is scaled
  before the orbit (eased in over 0.15 s). Two things found on the way had to change for a chosen distance to
  hold: (1) the collision step pulls the game's own eye towards the eye it is given (a global that is that
  eye, hit or not): with the orbit that made the game's distance drift with the view angle (707 -> about 605
  units held 12 degrees up; the first build's "pull-in under the trees" was this, the collision trace showed
  no hit) and a longer distance would feed on itself; the pull is now made in the game's terms (none without
  scenery, as for the game's own camera); (2) the game's collision moves the camera out of the scenery at once
  and back at once: a far camera jumped in and out; now a needed correction is still taken at once, but one no
  longer needed eases away (0.3 s), relative to the camera's arm.
- **Position smoothing around the character**: at the new sensitivity maximum a 150-degree turn in one frame
  swung the camera to 227 units from the character (the game smooths along a straight line). The smoothing is
  now done on every modern-camera frame, by the game's rule, around the target: identical without a turn, the
  same lag in a turn, but no chord.
- **Field of View** (the game's own, shown as 58, then 60-110 in steps of 5; horizontal at 16:9): the game's
  projection takes a vertical angle from the camera key (0.6 rad) and the viewport's aspect (static analysis,
  confirmed by the projection trace); the gameplay path's per-frame slot gets the player's angle (each key
  scaled by the same factor); HUD/menus (orthographic), event cameras, the draw-distance term and Modern Camera
  Off are unchanged.
- **Sensitivity**: 20 levels; 1-10 as before (saved values keep their speed, defaults unchanged), 11-20 a fixed
  factor each: mouse up to 1.5 degrees per count (6x), stick up to 600 degrees per second (4x).
- **PC page**: a second page below Restore Defaults (Down scrolls to it, Up back), with the game's sliders and
  the values in the labels; the other rows, their frames and slider tracks are hidden there (layout IDs from
  both menu files). Title and in-game. The first page is unchanged.
- **Measured approved orbit at 100 %** (`camera_orbit`): aim 0.03 degrees, the same continuity; the distance now
  stays the game's while pitched (two of camera_orbit's own stick bounds raised from 45 to 60 units per frame
  for that, documented in the test). `camera_mouse`'s "collision under the trees" check, which measured the
  drift, now checks that looking down keeps the distance (-1.6 units).
- New tests: 9 game runs (`settings.pc_settings_view`, `_view_persist`, `_view_title`, `camera.camera_distance_
  near/far`, `camera_sensitivity_max/min`, `camera_fov_min/max`), 5 unit cases; `camera_original` also checks
  that Distance and Field of View do not act with Modern Camera Off.
- Full regression (`build/logs/m6b-ctest-6.log`, 61bc429): **1706/1706 passed** (7496 s); game folder unchanged
  (138 files) and the player's saves unchanged (18 files) before and after.

### Manual validation
The playable build is this branch's `build/linux-amd64-relwithdebinfo`. A windowed session with sound and the
Elite controller, starting from a *copy* of the Shadow Depths save (the copy is made inside the run directory;
the fixture and the player's own saves are only read):

```sh
tools/run_sandboxed.sh -t 3600 -n m6b-manual-1 --gpu --window --audio \
  --pad /dev/input/by-id/usb-Microsoft_Controller_30324D4930303333393433333434-event-joystick \
  --saves-from build/m6-saves/celine-shadows-depths-1 -- --kuf_exit_after_seconds=3590
```

The PC settings chosen in that session are in `build/runs/m6b-manual-1/user/pc_settings.toml`; a second session
can start with them: add `--pc-settings-from build/runs/m6b-manual-1`. For the original camera, set PC Settings >
Modern Camera to Off (no restart needed).

**Approved by the user (2026-10-23)** after playing the final build (session `m6b-manual-4`, ended normally;
settings left at Distance 60 %, Field of View 110): "Everything feels perfect. I approve the camera's current
behavior." The orbit, Camera Distance, Field of View, the expanded sensitivity and the PC page are the
reference implementation from here on (docs/CAMERA.md section 4); nothing in the camera's behaviour or defaults
is to change.

### Final verification (after the approval, 2026-10-23)
No camera behaviour changed; only a trace, tests and documentation were added.

- **camera_orbit, stick bound 45 -> 60 units per frame (made in 61bc429)**: required. Per-frame decomposition of
  the stick segment (`build/runs/ctest-camera-camera_orbit*`): the turn's own part is unchanged (15-20 units per
  frame in both builds); the largest steps are where the eye meets the ground at the lower limit and the game's
  ground slide pulls it in. The approved orbit before the distance setting slid from 637 units (its distance had
  crept in while pitched) and measured 37.3 under 45; with the distance kept at the game's 707 the same slide
  measures 50.3 (19.1 turn, 46.7 slide; 48.2 in the final regression). The bound keeps the same margin (60 / 50.3 as 45 / 37.3); the other 37
  camera_orbit checks are unchanged. Documented in the test.
- **camera_mouse, "collision under the trees" replaced by "looking down keeps the distance"**: correct. The old
  check expected the distance to drop by 50+ units looking down; the collision trace showed no hit there at all:
  the drop was the orbit's distance creep (707 -> 616), the defect fixed with Camera Distance. The new check
  (within 15 units of the start; measured -1.6 and -1.1 in the last two regressions) fails on that creep. The collision coverage it was meant to give
  is now a test of its own at the default distance, `camera_distance_default` (a hit while turning in the
  forest, the release gradual: largest increase 37.9 units per frame, bound 45; back on the orbit), besides
  `camera_distance_near` / `_far`.
- **Fault injection** (`camera.checks_fault_injection`, tests/tools/test_camera_checks.py): the camera runs' logs
  pass as they are, and 11 injected regressions on copies of them each fail the intended check (a 75-unit eye
  snap; aim 3 degrees off; the distance creep 707 -> 616; a 300 / 346-unit snap back after a collision; no
  collision at 100 %; a gameplay frame drawn with the stale 0.5 rad; one projection at another angle; Field of
  View acting with Modern Camera Off; the original's handover broken; the top mouse level as slow as the old).
- **First-frame field of view**: not a defect. The render side reads the per-frame slot with its own index
  (0x82AE7610) one game frame behind the game side (0x82AE760C); new trace lines (`--kuf_trace_projection`) show
  which write each render projection reads. At gameplay start the frame written before the switch is drawn with
  its own (loading) angle, 0.5 rad, and every frame written with the gameplay angle with the setting; with Modern
  Camera Off the original does exactly the same (0.5, then 0.6). Nothing changed in the projection; the FOV tests
  and `camera_original` check the handover. (Removed from the limitations.)
- **Persistence**: new `pc_settings_sensitivity` / `_persist` (20/20/20 set in the menu, loaded after a restart,
  used: 100 counts = 149.8 degrees; Restore Defaults). With the existing runs and unit tests every point is
  covered (docs/SETTINGS.md table). Title and in-game menus pass.
- **Full regression** (`build/logs/m6b-ctest-7.log`, 945c4db): **1710/1710 passed** (8085 s), none skipped or
  not run; the fault injection ran on that run's own camera logs (11/11). Game folder unchanged (138 files) and
  the player's saves and backups unchanged (18 files) before and after; `check_tracked` clean (212 paths).

### Merge plan (prepared, not executed; awaiting approval)
The branches form one line: `main` (3c7f754, M5B) <- `m6/campaign-compatibility` (+6 commits) <-
`m6a/performance-and-menus` (+13) <- `m6b/modern-camera` (+12; 31 ahead of main, 0 behind). Each contains the
previous one, so there is nothing to resolve and no conflict is possible: every merge is a fast-forward.

Recommended: one fail-closed fast-forward of `main` to the M6B tip, which brings M6, M6A and M6B in their
order with every commit kept: `git checkout m6b/modern-camera && tools/safe_merge.sh m6b/modern-camera
--new-branch m6c/<name>` (clean tree and SDK patches, both build trees, the full suite on that commit, game folder,
tracked content, then `git merge --ff-only`). Merging M6 and M6A first, one by one, is possible but not
better: their tips need the SDK without the M6A patches 0006/0007 (safe_merge's clean-tree gate refuses a
submodule with patches its commit does not list), so each would need the SDK re-patched, a rebuild and another
two-hour suite, for a result identical to the single fast-forward; both were verified on their own tips before
(1649/1649, 1656/1656). Notes: the audit tree (`build/linux-amd64-audit`) was last built for M6A and is rebuilt
by the gate; the merge gate runs the suite again (about 2 h 15 min).


## M6C: native resolutions, display modes, widescreen and ultrawide (branch m6c/display-and-resolution)

Authorized 2026-10-23 with the M6B merge (main at a9bf2d8, 1710/1710 in the gate): PC resolutions and display
modes, real widescreen and ultrawide rendering, HUD and menus correct at every shape, the display settings in the
game's own PC Settings. docs/DISPLAY.md.

### Implementation
- **3D (Hor+ / Vert+)**: the game's perspective builder (`sub_826FC108`) gets the output's aspect for the 16:9
  projections of its three callers and the view's stored aspect follows, so drawing and culling agree; wider
  outputs keep the vertical angle (21:9: 57.6 -> 72.9 degrees horizontally; 32:9: 95.5), narrower keep the 16:9
  horizontal angle. Field of View keeps its meaning (the angle at 16:9). The approved camera is unchanged (a
  data race in its projection trace set, debug only, was fixed).
- **HUD and menus**: a mid-asm hook in the game's 2D renderer (0x8222C9B4) scales each quad's clip-space vertices
  into a centred 16:9 box; full-width layers (fades, pause dimming) stay full. The presenter stretches the frame to
  the output. The pointer mapping (the input driver's 16:9 fit) equals the box (unit-tested at 16 shapes).
- **Movies**: the title loop and the attract movie are drawn by the game's movie player, not the 2D renderer;
  hooks on its play and stop calls present them at 16:9 (pillarboxed). Found by the ultrawide check of the
  attract movie, which was stretched before.
- **Window and monitors** (`display_window.cpp`, SDL3 on the UI thread): Windowed (resizable, sizes from the
  monitor's modes that fit its usable area), Borderless (desktop), Fullscreen (the monitor's modes and refresh
  rates; emulated by the compositor on Wayland). No fixed resolution lists. Resize, scale change and restore
  update the output from the next frame.
- **PC Settings > Display** (third PC page, in-game and title): Mode, Size, Monitor, Widescreen, Apply with
  "Keep?" and revert on Back, on leaving the page, or after 15 s (timer thread). Widescreen takes effect at once
  (projection, HUD box and presenter).
- **pc_settings.toml `[display]`**: mode, width, height, refresh_millihertz, monitor, widescreen; validated like
  the M6B keys; a disconnected monitor falls back to the primary one and keeps its name; M6B files load unchanged.
- **Render scale**: the SDK can render at multiples of 1280x720, but any scale above 1 tints the HDR post chain
  red (KNOWN_ISSUES K31); default 1x, `--kuf_render_scale_auto` experimental.

### Performance (Shadow Depths exploration loop, gameplay from 20 s after it starts, warm shader cache, RX 9070 XT)
Simulated outputs: the presenter's scaling to the real screen is not exercised offscreen.

| Run | fps | frame median / 99th / max (ms) | guest CPU (% core) | GPU-command thread | GPU busy |
|---|---|---|---|---|---|
| 1280x720, cold cache | 29.99 | 33.3 / 36.4 / 76 | 112 | 14 | 36 % |
| 1280x720 | 29.97 | 33.4 / 36.7 / 107 | 113 | 17 | 36 % |
| 3440x1440 (21:9) | 30.00 | 33.3 / 36.5 / 65 | 113 | 17 | 36 % |
| 5120x1440 (32:9) | 30.00 | 33.5 / 36.2 / 54 | 112 | 17 | 24 % |
| 3840x2160 | 30.00 | 33.3 / 35.8 / 58 | 112 | 13 | 30 % |
| 3840x2160, render scale 3x3 (experimental, K31) | 29.99 | 33.3 / 36.2 / 77 | 113 | 16 | 65 % |
| uncapped 1280x720 | 224 | 4.3 / 6.4 / 73 | 188 | 96 | 85 % |
| uncapped 3440x1440 | 181 | 5.3 / 10.7 / 76 | 173 | 95 | 84 % |
| uncapped 1920x1080, scale 2x2 (2560x1440) | 132 | 5.6 / 14.6 / 15 | 144 | 71 | 98 % |
| uncapped 3840x2160, scale 3x3 | 76 | 10.1 / 23.6 / 31 | 127 | 33 | 100 % |

- The game is capped at 30 fps; at every output it holds 30.0 with the GPU 24-36 % busy. The wider views cost
  about 20 % in uncapped frame rate (224 -> 181 fps at 21:9: more of the scene drawn), GPU-bound.
- Rendering at higher resolution is GPU-bound (98-100 % busy) and still far above 30 fps (76 fps at 4K), but
  unusable until K31 is fixed. "Uncapped" here is `--vsync=false` for measurement only (no high-FPS work in M6C).
- Shader compilation: the cold run's pipeline thread used 3.6 % of a core in its first 72 s (boot, title,
  loading) and 0.6 % in gameplay; with the cache warm, 0.0-0.7 %. No gameplay stutter from compilation in either.
- Mode switch: Apply's own time is logged (`[kuf-display] display: ... (N ms)`); offscreen it is 0 ms (no real
  mode change). On real hardware it is for the manual session.

### Verification
- **Full regression** (`build/logs/m6c-ctest-1.log`, fc7e439 + docs 4ccb67a): **1741/1741 passed** (9608 s), none
  skipped. 31 new: 12 display unit tests, 7 settings unit tests, 12 display scenarios (`-L display`). One
  existing test changed for the intended behaviour: `pc_settings_view` (Down at the end of page 2 now opens the
  Display page). The image tests got a stub for the new mid-asm hook (they link the recompiled code alone).
- Game folder unchanged (138 files), the player's saves and backups unchanged (18 files) before and after;
  `check_tracked` clean. Apply's time in the display tests: 0-1 ms offscreen (31 applies).
- Simulated, not real hardware: every automated run is offscreen with a simulated output size and monitor.
  Real window resizing, monitors, Alt+Tab and Fullscreen on Wayland await the manual session.

### Follow-up: native render resolution (2026-10-23, same branch; not merged)

The player did not accept M6C as complete while the 3D scene was rendered at 1280x720: native-resolution
rendering at 1080p, 1440p, 4K, 21:9 and 32:9 was required, with the red cast at higher resolutions (K31)
root-caused rather than hidden. docs/DISPLAY.md section 1.

#### Root cause of the red cast (K31, fixed)
- Found with a GPU trace added to the SDK (patch 0009, `--gpu_trace_frame`: per-draw shaders, render targets and
  texture fetch constants, per-resolve copy registers, and dumps of every resolve's data and of the textures loaded
  from resolves). One gameplay frame traced at scale 1 and 2: the first colour resolve of the scene (the HDR scene
  target) was already wrong at 2 (green -40 %, blue -37 %, red -11 %). The only resolved textures those scene draws
  sample are the two shadow maps (dumped after loading: identical to scale 1) and four 32x32 floating-point cube
  maps (wrong, faces with red far above green and blue). The cube maps are built once at level load: the level's
  environment cube is projected into two 64x64 targets (identical at both scales), reduced into a 4x4 texture of
  lighting coefficients (wrong at scale 2: different values in the four host pixels of one guest pixel, some the
  pass's own fill value 1,0,0), and expanded into six faces per cube that point-sample those coefficients.
- The reduction pass's pixel shader (6882C5AE6F67A076) has no interpolated inputs: it uses the pixel position
  (Xenos `ps_param_gen`, VPOS) as the index into its weight tables. The SDK delivered that position at scale 2
  as `floor(host position) / 2` (3.5 for the second host pixel of guest pixel 3); the hardware delivers the
  integer pixel position. Fix (SDK patch 0008, Vulkan and D3D12 translators): every host pixel gets the integer
  position of the guest pixel it covers, `floor((floor(host) + 0.5) / scale)`. The cube maps are now bit-identical
  to scale 1 at 2x2, 3x2, 3x3 and 4x2, and the frame's colours match scale 1 (below). The other shaders that read
  the pixel position (a shadow-filter dither in the scene shaders) keep the original pattern per guest pixel.
- No colour correction and no effect disabled: the post-processing chain (HDR scene, quarter-size blur ping-pong,
  composite) runs unchanged at the render scale.

#### Implementation
- **Render resolution**: the SDK renders the game's 1280x720 frame at a whole multiple per axis (every render
  target, resolve and texture made from a resolve scaled). PC Settings > Display > **Render** (sixth row, below
  Apply; `[display] render_scale`): **Native** (default) = the smallest multiple at least the output on each axis
  (1080p 2x2 = 2560x1440 filtered down; 1440p 2x2 and 4K 3x3 exact; 3440x1440 3x2 = 3840x1440; 5120x1440 4x2
  exact), or 1x-4x. Saved at once, in effect from the next start (`*` on the row until then): the game renders its
  lighting cube maps once per level and keeps them only in the scaled form, so the scale cannot change during play.
  `[kuf-display] start: ... render scale 3x2 (frame 3840x1440); Render Native` in the log. The old experimental
  `--kuf_render_scale_auto` is gone.
- Unchanged and independent of the render scale: the projection (Hor+ / Vert+, FOV), the camera, the HUD box and
  the pointer mapping (clip-space), the presenter's 16:9 letterbox for movies and Widescreen Off (it uses the guest
  video mode's aspect, not the texture size). The HUD and menus are drawn into the scaled frame, so they are as
  sharp as the scene; the game's textures and the movies keep their resolution.
- Tests: `capture_check.py` `[format] guest_coordinates` (a native capture reduced to the 1280x720 frame by block
  averages, so rules in frame pixels hold at any scale) and `render_scale`; the render_ref / render_native
  scenarios and `tests/display/render_compare.py` (docs/TESTING.md); native versions of the 16:10, 4:3, movie,
  Widescreen Off, camera orbit, maximum field of view and combat scenarios; the Render row and its next start.
  The display scenarios written for the 1280x720 frame run with Render 1x (one `--kuf_pc_settings` addition each,
  checks unchanged); their native counterparts use the same rules.

#### Performance (Shadow Depths exploration, at the 30 fps cap; docs/PERFORMANCE.md section 6)

| Output | Rendered frame | fps | frame p99 | GPU busy (mean) | VRAM (game) | uncapped fps |
|---|---|---|---|---|---|---|
| 1280x720 | 1280x720 | 29.99 | 36.3 ms | 25 % | 0.95 GB | 306 |
| 3840x2160, Render 1x (output scaling only) | 1280x720 | 29.99 | 36.1 ms | 23 % | 0.95 GB | |
| 1920x1080 | 2560x1440 | 29.98 | 36.8 ms | 33 % | 2.17 GB | 171 (2x2) |
| 2560x1440 | 2560x1440 | 29.98 | 36.1 ms | 39 % | 2.08 GB | 171 |
| 3440x1440 | 3840x1440 | 29.98 | 36.3 ms | 50 % | 2.60 GB | 128 |
| 3840x2160 | 3840x2160 | 29.97 | 36.2 ms | 56 % | 3.17 GB | 99 |
| 5120x1440 | 5120x1440 | 29.96 | 37.7 ms | 58 % | 2.94 GB | 100 |

- 30 fps held everywhere; the game's CPU time is the same at every scale (112-114 % of a core); the cost of
  native rendering is GPU time (about 0.9 ms per additional million pixels: native 4K about 10 ms of the 33 ms
  frame) and VRAM. Output scaling alone (1x at 4K) costs nothing measurable offscreen.

#### Verification
- **Colours**: the GPU trace of one gameplay frame at scale 1 and 2 before the fix showed the scene resolve 40 %
  low in green; after it, every render_native run's lighting cube maps are bit-identical to the 1x reference in
  every host block (2x2, 3x3, 3x2, 4x2), the HDR resolves' channel means match and the final images' channel means
  are within 0.11 levels of the 1x reference (8x8 block difference 0.36-0.41; the tests allow 1.5 and 4).
  `render_compare.py` run on a pre-fix pair fails on the cube maps (every texel different), so the test catches
  K31.
- **Internal resolution**: frames captured at 2560x1440 (1080p, 1440p), 3840x2160 (4K), 3840x1440 (3440x1440),
  5120x1440 (32:9); every resolve of the traced frame in the scaled buffer with exactly scale_x x scale_y times the
  data, the same resolve sequence as at 1x.
- **Full regression** (`build/logs/m6c-native-ctest-1.log`, 6e9200c): 1,764 tests, 1,750 passed, 1 failed,
  13 not run: `savegame.save_new` (1x, unchanged) failed on its log sequence because the run had already saved and
  returned to the title about 1,000 frames before its script expected (scripted-run drift, the K24 kind; the save
  itself committed and verified), and the 13 save tests that start from its saves were therefore not run. The
  savegame chain run again (`build/logs/m6c-native-ctest-1-savegame-rerun.log`): **14/14 passed**. The three
  native camera / combat scenarios were registered after that run's configure and ran separately with the camera
  tests they share a fixture with (`build/logs/m6c-native-ctest-1-native-camera.log`): **11/11 passed**
  (`camera_orbit_native`, `camera_fov_max_native`, `combat_loot_native`, the fault injection). Every test of the
  1,767 has passed on this commit.
- The display and settings subset before that (`build/logs/m6c-native-display-1.log`): 74/76, then 76/76 after
  correcting the new Render-row test's label rectangle (the feature passed: its log and saved file were right).
- Game folder unchanged (138 files), the player's saves and backups unchanged (18 files) before and after;
  `check_tracked` clean.
- Simulated outputs only: every automated run is offscreen; the presenter's scaling to a real window, real
  monitors and mode switches are for the player's session.

#### Remaining limitations
- Whole multiples only (the SDK's EDRAM emulation): 1080p, 3440x1440, 2560x1080, 16:10 and 4:3 render more pixels
  than the output and are filtered down (bilinear) by the presenter; exactly native at 2560x1440, 3840x2160 and
  5120x1440 (and 1280x720).
- The render resolution changes at the next start, not during play.
- The game's textures, fonts and movies keep their resolution; native rendering sharpens geometry, shadows,
  effects and edges, not texture detail.
- Pixel-position dither patterns (shadow filtering) are the original's per guest pixel, i.e. as coarse on screen
  as at 1280x720 (exact to the hardware; the SDK's former fractional position was finer but broke the lighting).
- Not exercised on real hardware yet: window/monitor/mode behaviour at native resolution, VRAM on GPUs with less
  memory (3.2 GB for native 4K here).

#### Manual validation (to do: the player's session)
The playable build is this branch's `build/linux-amd64-relwithdebinfo` (a windowed session with sound and the
Elite controller, from a *copy* of the Shadow Depths save; the camera settings of the approved M6B session):

```sh
tools/run_sandboxed.sh -t 3600 -n m6c-manual-1 --gpu --window --audio \
  --pad /dev/input/by-id/usb-Microsoft_Controller_30324D4930303333393433333434-event-joystick \
  --saves-from build/m6-saves/celine-shadows-depths-1 --pc-settings-from build/runs/m6b-manual-4 \
  -- --kuf_exit_after_seconds=3590
```

It starts Borderless on the primary monitor at its desktop resolution with Render Native (the log's
`[kuf-display] start:` line names the rendered frame). To check: colours as at 1280x720 (no red cast); sharpness
of the scene, the HUD and the menus; Hor+ at the monitor's shape; Settings > PC Settings > Display: Mode, Size,
Monitor, Apply / Keep / revert; Render 1x-4x (`*` until a restart; quit, start again with
`--pc-settings-from build/runs/m6c-manual-1` and the frame is rendered at the new scale).

#### Final verification (2026-10-24; not merged)
- **Startup resize fix** (2fa8412): in the player's first 4K session the borderless window was mapped at 1904x1043
  and then resized to 3840x2160; `TakeWindowSize` stored the new width but, through `||` short-circuiting, never
  the height, so the game drew for 3840x1043 (aspect 3.68) on a 16:9 screen. `StoreWindowSize` (display_math)
  always stores both; a unit case covers the resize and single-axis changes and fails with the old expression.
  The next windowed session logged `window resized 3840x2160 (16:9): drawn for aspect 1.7778`. Offscreen runs
  never resize, which is why no scenario saw it.
- **save_new failure of the previous full run investigated** (742be2e, docs/KNOWN_ISSUES.md K24): not timing drift
  but a death in the scripted fight. A scan of all 64 stored runs of the fight found 4 deaths (all at
  +9740..+10400) and 2 runs stuck in the tutorial's camera lesson (2026-10-22); save_new failed on each, but
  **combat_loot had passed its 3** (its before/after screen comparisons also pass on the Game Over screen).
  combat_loot, combat_loot_native and save_new now require the status screen to open after the fight (rejects
  exactly the 6 bad stored runs, accepts the other 58), and the scripted player drinks the 5 Small Red Potions
  from +6300 every 700 frames instead of from +4100 every 300 (they were gone by +5300 at near-full HP): lowest
  HUD HP after +8400 65-89 % in 3 runs vs 17-72 % in 6. Test scripts only: no game code, combat values, potion
  behaviour or save format changed.
- **SDK patches**: 0001-0009 apply in order with `git apply --check --whitespace=error-all` to a fresh clone of the
  pinned SDK (c94f5eb); the result is identical to the working SDK. 0008 touches only the shader translators; 0009
  shares `vulkan/command_processor.cpp` with 0006 and applies on top of it; the trace is off by default.
- **Camera**: since the M6B merge (a9bf2d8) `camera_math` and `CameraSettings` (defaults) are unchanged and the
  camera tests are unchanged; `modern_camera.cpp` gains only the display aspect hook (16:9 left exactly as the
  game's, unit-tested) and a mutex for the projection trace.
- **Full regression** (`build/logs/m6c-final-ctest-1.log`, 742be2e, both trees built first): **1,768 tests, 1,768
  passed, 0 failed, 0 not run** (12,213 s). Includes the savegame chain, campaign, camera (orbit, FOV, sensitivity,
  collision, native), display (native 1080p/1440p/4K/21:9/32:9 compared with 1x: resolves, lighting cube maps bit
  for bit, HDR, colours; HUD, menus, pointer mapping, widescreen, movies, Render row and persistence), settings
  and the fight scenarios with the death check.
- Game folder unchanged (138 files) and the player's saves and backups unchanged (18 files) before and after;
  `check_tracked` clean; no test, game or wait processes left running.
- Outside the suite: one of six parallel K24 exploration runs (`k24l3-k24_late`) ended with exit 137 mid-fight, no
  crash report, no OOM event (K30).
- Still for the player's session: windowed / borderless / fullscreen and switching between them, real monitors,
  the presenter's final scaling (every automated run is offscreen). Manual command as above with
  `-n m6c-manual-3`.


#### Merged (2026-10-24)
- Approved by the player and merged with `tools/safe_merge.sh m6c/display-and-resolution --new-branch m6d/high-fps`
  (log `build/logs/merge-m6c_display-and-resolution.log`, ctest `build/logs/merge-ctest.log`): **1,768 tests, 1,768
  passed**, fast-forward, **`main` = d3734d2**. No history rewritten.
- SDK patches 0001-0009 unchanged and applied (the submodule shows "m" by design).
- Game folder (138 files, `build/input/v1/game_dir.sha256`) and the player's 18 save files unchanged after the merge
  and after the M6D probes (checked again 2026-10-24).
- Still unverified by automation: real monitors, display-mode switching and the presenter's final scaling (the
  player's manual session).

## M6D: high frame rates (branch m6d/high-fps; not merged)

Read-only timing investigation, no code changes: **docs/HIGH_FPS.md**.
- The game is frame-locked: one 1/30 s tick per rendered frame (game and render threads linked by two semaphores,
  0x82AC6658), no measured delta time; the 30 fps cap is the D3D present interval 2 chosen at 0x826EF2E4.
- Four probes (guest refresh 60/120/240, vsync off) measured 29.97/59.5/117.0/176.3 fps and the game running
  1.00x/1.99x/3.91x/5.88x real time; same walk distance per frame in all four; menus and loading sped up too; the
  modern camera's turn is real-time (differs per rate).
- Recommended: fixed 30 Hz simulation on a host clock, render thread presenting independently with interpolation
  (camera and player first, then the world); 30 (original) stays the default. Phases D1-D6 proposed; D1 + D2
  approved by the player (2026-10-24).

### D1: rendering and timing diagnostics (docs/HIGH_FPS.md section 9)
- **Tools** (opt-in, nothing changes by default):
  - `--kuf_render_repeat` draws the latest tick again;
  - the input script's `freeze <seconds> <label>` holds the game and diffs all guest memory (3.2 GB) over the
    tick's first frame, the repeated frames and a control period;
  - `--kuf_trace_render_lists`.
- **The renderer cannot draw a tick twice.** It consumes the tick's data as it draws it, in at least seven places:
  - at the frame end: the slot's draw list, deferred resource commands, three frame-memory pools, keep-alive
    releases;
  - in the draw: each view's per-slot lists, the game's render command queue, lists in other objects including a
    PhysX callback object.

  With all of those found so far deferred, repeated frames are still black. So D2 keeps one draw per tick (K32).
- **The simulation runs only in the tick.** With the game held and nothing drawn, 43 bytes of gameplay memory
  changed in 2 s.
- **The camera** (all modern-camera updates) and input run on the game thread; the render thread only builds its
  projection.
- **Movies run on their own clock:** the title movie decodes in real time while the game is held.
- **Loading screens** are presented by the game thread itself.

### D2: fixed 30 Hz tick (docs/HIGH_FPS.md section 10)
- **`--kuf_fixed_tick`, default on.** The game thread starts its ticks on a host clock: 30 per second from an
  absolute schedule, catching up at most two ticks, dropping a larger backlog. Nothing else in the loop changes,
  `--kuf_fixed_tick=false` restores the original pacing, and there are no busy waits.
- **Measured one run at a time** (`build/runs/d2s-*`): the game ran at 29.98 / 30.00 / 30.00 / 30.00 ticks per real
  second at a 60 / 144 / 240 Hz guest vblank and with vsync off. The original pacing ran at 29.97 at 60 Hz and at
  203.67 (6.79x) with vsync off.
  - Presented frames equal ticks; tick interval p99 33.38 ms at 144 / 240 Hz and vsync off.
  - Same CPU and GPU cost as the original at the default settings.
  - Captures at the same tick match the original run.
- **New tests:**
  - `timing.*` (4 game runs: fixed default, 240 Hz, vsync off, and the original with vsync off as a control that
    must speed up);
  - `pacing.*` (3 unit cases);
  - the `freeze` parser case.

  `tools/menu_test.py` gains `[scenario.timing]`. Run summaries gain `pacing`, `sim_tick_timeline`,
  `game_counter_timeline` and `tick_frames_presented`.
- **Regression:** 1,771/1,776 in the full suite. Four camera handover checks missed the frame drawn before the angle change: with the fixed tick it is drawn before the change is written, outside the trace's window. The trace now logs the last frame drawn before the change; the four tests, their dependants and the fault injection passed on rerun (10/10). Details in docs/HIGH_FPS.md section 10.4.
- **Game folder and saves:** `tools/verify_game_dir.sh`: 138 files unchanged. The 41 save files in build/manual-saves, build/save-backups and build/m6-saves have the same SHA-256 as before D2. `tools/check_tracked.py` clean.
- **Final D2 regression (2026-10-25, HEAD e57c39e):** 1,776 of 1,776 passed, none skipped or not run
  (build/logs/m6d-d2-ctest-final.log). Camera (M6B, 16) and display (M6C, 31) tests all passed. Game folder: 138
  files unchanged. 41 save files unchanged. `check_tracked` clean.
- **Not done in D2:** frame rates above 30 (K32).

### D3a: render frame data lifetime (research only; docs/HIGH_FPS.md section 12)
- **Diagnostics** (opt-in; nothing changes by default):
  - `--kuf_render_builds` (render-only slot builds between ticks, with `freeze`), with
    `--kuf_render_build_submit` and `--kuf_render_build_same_slot`;
  - `--kuf_trace_render_lists` with builds: slot containers, and who fills the frame packet (guest call chains);
  - `--kuf_trace_slot_writes`: guest words changed per phase of 4 ticks;
  - a `rand()` counter;
  - SDK patch 0009's GPU trace now logs draw state and pixel-shader constants.
- **Exact cause of the black frames** (K32), three separate stages:
  1. The frame packet's object list (renderer + 26584 + slot * 272) is filled by the game step's submission pass
     (`state->vf28`, plus one object from `vf24`) and erased by the frame end: no world.
  2. The HUD's UI lists are filled by the game step (`sub_825BE6B0`): no HUD.
  3. Without the HUD's draws, the SDK's default `fbo` render-target path resolves the final frame as zeros (K34).
     On the EDRAM-exact `fsi` path, both rebuilt frames and D1 repeat frames show the correct scene.
- **A render-only build** (the tick's scene build again, no game step) keeps the game at 30.00 ticks/s (1.00x).
  It calls the CRT `rand()` about 22 times per build (seed to be preserved). Not every word it changes could be
  attributed, so a gameplay-state audit is a D3b test.
- **Effect clocks** (shader time constants) advance 1/30 s per drawn frame on the render thread, so effects would
  run faster at higher frame rates (GPU trace at 120 vs 30).
- **Recommended:** rebuild render slots per presented frame from the tick's retained game-step outputs. Interpolate
  only presentation data: the camera as view matrices, never the M6B simulation camera; objects later. Phases
  D3b-0 (fix K34) to D3b-3. Risks and validation tests in docs/HIGH_FPS.md 12.8-12.11.
- **Default-path check** after adding the diagnostics' hooks: unit (pacing, input), `timing.*` (4),
  `menu.reach_menu`, `camera.camera_original` and `gameplay.new_game`: 35 of 35 passed.
- **Display render tests** (they use the GPU trace that patch 0009 extends): 18 of 19 passed.
  `display.render_compare_32_9` failed, and also fails with the exact binary that passed the full D2 regression.
  The cause is a swap-numbered trace frame that lands on a different moment of play (K35), not D3a.
- **Not done:** D3b, interpolation, frame rates above 30. Awaiting approval. M6D is not merged.

### D3b-0, K35, README and the D3b-1 investigation (2026-10-25; docs/HIGH_FPS.md section 13)
Approved: D3b-0, the K35 correction, the README, and D3b-1 as an investigation with a proposal. Not implemented, as
instructed: the D3b-1 design, D3b-2, interpolation. M6D is not merged.

- **K34 fixed at its root (SDK patch 0010).** The game's 4x MSAA depth-only passes at EDRAM tile 720 claim the whole
  EDRAM, the frame's colour at 0 included; a frame without the HUD's draws resolves those tiles from the depth buffer,
  and the SDK's dump addressed a render target owning tiles through EDRAM wrapping outside its image (zeros). The dump
  now addresses each part in the render target's own period. Frames without the HUD show the scene on `fbo`, matching
  `fsi`. In normal play the correction affects one full-frame copy per title/menu frame whose texture is overwritten
  before use: captures bit-identical (title) or within run-to-run noise (play). K32 is unchanged.
- **K34 tests:** `gpu.*` (the dump addressing; 4 of 5 fail without the fix) and `render.k34_*` (black before; now
  matching the tick frame and the `fsi` path).
- **K35 corrected (tests only).** Cause measured: the fixed swap 1840 landed on gameplay +339..+344, and the 32:9 bloom
  difference depends on the moment (K36). The input script's `trace` traces a tick frame (`@gameplay+340`, the M6C
  moment); render_compare.py checks the anchor and accepts only the bloom's two alternating buffers exchanged.
  Tolerances unchanged. Alone 2 of 2 pass (4 of 4 failed before); with K31 injected it fails.
- **README** rewritten for the current state (playable on Linux; 30 fps; M6D in progress; Windows not built).
- **D3b-1 investigation:** object identity (renderer object indices), per-slot world matrices (object + 28 + slot * 64)
  for objects and the view, the HUD lists, costs (a scene build 2.6 ms; 119.8 frames per second at 30.00 ticks/s with
  three serial builds per tick), the retained set's gaps (the HUD and ten objects' draws). **Recommended:** A,
  render-only builds from retained game-step outputs; proposal and risks in HIGH_FPS 13.4-13.5. Diagnostic:
  `--kuf_render_build_restore`.
- **New issues:** K36 (bloom at render scales a few per cent darker, scene-dependent), K37 (one run in 55 entered
  gameplay late through the intro skip), K38 (scripted camera input can miss a tick and double the next:
  intermittent camera_orbit continuity failures).
- **Regression on 8229178** (build/logs/d3b0-*.log): unit and tool tests (`ctest -LE smoke`) 1,714 of 1,716; the two
  were `camera.camera_orbit` (K38) and `camera.checks_fault_injection`, not run because it depends on it. Game-run
  tests of display, render, camera, menu and timing (59) 58 of 59; the failure `camera.camera_orbit_native` (K38).
  Both camera tests and the fault injection then passed 3 of 3 reruns (9 of 9 each). `gameplay.new_game` passed. The
  game folder was verified before and after, the 41 save files' hashes are unchanged, and no tracked path contains
  game data. Covered: K34 (`gpu.*`, `render.k34_*`), 30 fps rendering and the native resolutions and ultrawide
  (`display.*` render comparisons and captures), the HUD and menus (`menu.*`, display captures), the camera projection
  (`camera.*`), the 30 Hz tick (`timing.*`), the K35 detection (fault injection with patch 0008 reversed, above).
- **Commits:** 5057c83 (K34 fix and tests), c4b7d47 (README), 506fe5a (K35), 8229178 (D3b-1 diagnostic), and the
  documentation (the commit containing this section).

### D3b-1: render-only frames and K38 (2026-10-26; docs/HIGH_FPS.md section 14)
Approved: option A (HIGH_FPS 13.4) and the K38 correction. Not done, as instructed: camera and player interpolation
(D3b-3), world interpolation (D4). M6D is not merged.

- **Checkpoint A, K38 fixed (6d560e4).** Script time is game ticks: input items apply on the game thread at the start
  of the tick after their time, the virtual pad updated there; capture, trace and freeze after tick frame N (it shows
  tick N - 1). New check `input_ticks` in the seven camera tests: 408 of 408 items one tick each (105 and 109 wrong
  in the stored failing runs), also at 54, 101 and 195 presented frames per second. No camera code or bound changed;
  the 2026-10-22 stick failure was the old bound of 15 (raised in 61bc429), not K38.
- **Checkpoint B, complete frames.** `--kuf_frame_rate=N` draws render-only frames from the tick's retained data
  (`src/runtime/render_frames.cpp`): restored object, two-pass and UI lists; HUD and frame-memory releases deferred
  to the next tick into the slot; bone palettes kept. 540 of 540 draws as in the tick's frame (vertex fetch addresses
  differ; shadow constants within 2.3e-4, K40); images within 0.006 levels on average at 1280x720 and native 4K,
  0.008 at 32:9.
- **Checkpoint C, isolation.** No physics step, no rand() and the clocks a frame advances put back: 0 rand and 0
  PhysX calls in builds at every rate, positions as in 30 fps runs within run-to-run variance.
- **Checkpoint D, presentation.** Builds only while the render thread is idle and when they fit before the next tick;
  frames are dropped, never ticks. 30.00 ticks per second at every rate. Presented: 30 -> 30.0, 60 -> 59.9,
  120 -> 119.6, 144 -> 137.0, 240 -> 158.2, uncapped 159.0 (CPU-bound near 160, K39); native 4K and 32:9 at 240:
  68.0 and 69.1 (GPU 100%). Build 0.87 ms against a tick's 2.55 ms. Tick-to-present p50 47.3 ms at 30, 8.8 ms at 120.
  Soak 30 minutes at 240: 366,719 frames, 0 restore failures, memory flat. Level load at 120 (`gameplay.new_game`):
  pass, 10,191 builds, 0 restore failures. No motion is smoother yet: render-only frames repeat their tick (K39).
- **New tests:** `render.frames_complete*`, `render.frames_check`, `render.frames_check_detects` (catches the K34
  frames without the HUD), `timing.tick_rate_frames_120/_240/_uncapped`.
- **Issues:** K32 (no high frame rates) and K38 resolved; new K39 (no interpolation; ceiling about 160 fps) and K40
  (shadow constants within 2.3e-4 in render-only frames).
- **Regression on f8ca22c's tree** (build/logs/d3b1-regression.log, d3b1-ctest-full.log): `ctest` 1,793 of 1,793
  passed (all game-run tests included, 3 h 51 min). The game folder was verified before and after, the 47 save files'
  hashes are unchanged, the SDK is pin c94f5eb with patches 0001-0010, no tracked path contains game data.
- **Commits:** 6d560e4 (K38), be108c1 (render-only frames), f8ca22c (tests), and the documentation (the commit
  containing this section).

### D3b-3: camera and player interpolation (2026-10-26; docs/HIGH_FPS.md section 15)
Approved: presentation-only interpolation of the camera and the player character between ticks. Not done, as
instructed: world, enemy, particle and bone interpolation (D4), settings integration (D5), optimisation (D6). M6D is
not merged.

- **Timeline.** Snapshots at each tick's scene build (the view's slot matrix and field of view, the M6B camera's
  record of the tick, the player model's slot matrix); each frame shows game time N - 1 + alpha (alpha from the
  tick's start to when the frame reaches the renderer, at most 1): interpolation between two completed ticks, never
  extrapolation, one tick of delay. Written into the slot before the scene build, so culling, shadows, projection and
  skinning use the presented camera and placement. Slots the game did not rewrite get the game's value back.
- **Camera.** The M6B camera unchanged (it records per tick what it did: `kuf::camera::TickView`). Interpolated around
  its pivot (azimuth the shorter way or the turn it took whole, elevation, distance; aimed at the pivot within 0.001
  degrees); a collision pull-in taken at once; field of view interpolated; other cameras rigidly. Found by the flick
  test and fixed: the turn hint assumed the whole input turn where M6B's smoothing turns the shorter way (a 337-degree
  wrong-way spin).
- **Player.** The skinned model at the camera's attach point; its world matrix interpolated rigidly (bones, parts and
  animation unchanged until D4: the parts follow the parent, the bones are model space).
- **Discontinuities.** Explicit signals first (loading, menus, level change, no view, event cameras, camera changes,
  the camera handover, recentring, collision pull-ins), thresholds as backstops (HIGH_FPS 15.4). Found in the new game
  and the fight and fixed: the opening's handover (1,615 units in a tick) and change of shot, a pull-in that also
  tilted the view 55 degrees, another character taking the player's place for two ticks, the player lost when stopping
  from a run.
- **Isolation.** With every presented camera 400 units off, interpolation off and in the 30 fps mode: the seed and
  every object equal at every tick for 600 ticks, the simulation camera within its own run-to-run variation (0.1).
- **Frame rates** (timing route, gameplay): 30 -> 30.0, 60 -> 59.9, 120 -> 119.6, 144 -> 137.3, 240 -> 158.6,
  uncapped 159.5; 30.00 ticks per second at every rate; frame time p99 10.6 ms at 120; no cost against interpolation
  off. Latency at 120: full response 42 ms (57 at 30 fps, 9 without interpolation).
- **Tests:** `kuf_presentation_tests` (11 cases), `presentation.*` (6 game runs, 6 checks), `gameplay.new_game_120`,
  `gameplay.combat_loot_120`, the seven `camera.*_120`; `render.frames_complete*` now run with interpolation off.
- **Issues:** K39 updated (only the camera and the player are interpolated; the rest is D4), new K41 (threshold
  rules where the game gives no signal), new K42 (`gameplay.controller_gameplay` intermittent, not caused by D3b-3).
- **Regression:** 1,824 of 1,825 ctest tests passed (16,462 s); SDK patches 0001-0010 apply, the game folder and the saves unchanged, no game content tracked. The failure, `gameplay.controller_gameplay`, is K42: intermittent and reproduced on HEAD 8ced876 without the D3b-3 changes (2 of 4 runs), so not caused by them; the test is unchanged and the cause is open.
- **Commits:** 4b21293 (camera signals), 38ef16e (camera and player interpolation, tests; one commit because
  they share one module and snapshot), and this documentation commit. No history rewritten, not merged.

### K42: the controller test's intermittent failure (2026-10-26)
Approved: investigate and fix K42 before D4a.

- **Symptom:** `gameplay.controller_gameplay` failed in about half the runs since 2026-10-26 (6 of 7 with the D3b-3
  tree, 2 of 4 on 8ced876): the tutorial's "End tutorial?" dialog, fully on screen, ignored the A press and all input
  after it.
- **Traced** with a new debug cvar, `--kuf_trace_game_input` (user 0's buttons as the game polled them, the press
  handler's gates, the input target, every press or release the game sent): in failing runs A was already held when
  the dialog opened, so the scripted A press gave the game no new down edge (it sent only the release). The dialog
  needs a fresh press; the controller, the gates and the input target were fine.
- **Cause (test tooling):** the scenario skips the intro with `repeat 60 until gameplay pad down a` and `pad up a` 3
  ticks later. When the game entered gameplay between such a down and its up, the input script stopped the repeat
  and dropped the pending up (only a `press`'s own release was kept), leaving A held. Gameplay starting inside that
  3-tick window depended on loading by a tick (1863 failed, 1864 passed).
- **Fix:** the script's scheduler (`TakeDue`, `Releases` in input_script_parse.cpp, now pure and unit-tested) applies
  a stopped repeat's pending releases once: key and pad button ups, sticks and triggers back to 0, real SDL key and
  button ups. A release cannot leave anything held. Only this scenario uses such a repeat, so no other test's input
  timing changes; the game, the test's checks and its thresholds are unchanged.
- **Tests:** `input.input script: a repeat that ends leaves nothing held (K42)` replays the failing schedule (gameplay
  entered in ticks 1861 to 1863) and fails with the old rule (verified); `input.input script: what counts as a
  release`. Validation: 8 of 8 traced controller runs passed, 3 of them entering gameplay with A held (one at tick 1863, the original failing case: its release applied at tick 1864); the new unit test fails with the old rule; only `controller_gameplay` has a repeated explicit release, so no other scenario's input timing changes. The input unit tests (28) and the `menu`, `gameplay`, `camera` and `settings` labels (45 game tests) passed except one `gameplay.combat_loot` run that picked up no loot (Gold unchanged, inventory empty, EXP gained: the fight's own randomness, K24; its keyboard script is unchanged by this fix); 3 reruns of `combat_loot` and `controller_gameplay` passed.

### D4a: world object interpolation (2026-10-27)
Approved after K42: interpolate the world's object transforms between ticks, presentation only. Not started, as
instructed: D4b (bones, particles, effects), D5, D6; not merged. Details: docs/HIGH_FPS.md section 16.

- **Objects:** every object in the tick's draw list with a world matrix of its own (the lizardmen and other characters,
  dropped items, moving props; not the view, the characters' parts, which copy their parent's matrix in the scene
  build, or the player, which keeps D3b-3's rules) is drawn between its two ticks: its slot matrix interpolated
  (position and scale linear, rotation slerp), written into the slot before each scene build, the game's own value put
  back at the slot's next tick where the game has not written it. No simulation state is written.
- **Identity:** object indices are reused (in the fight 111 reused indices, 46 of them with the new object at the same
  address and class). A hook on the game's release (`sub_821A4F90`) counts releases per index; an object is the same
  across ticks only with the same index, release count, address and class. Identity changes without a release: 0 in
  every run.
- **Cuts:** first, gap, not world, another renderer; new (not listed before: spawns, reuse, back from culling); another
  object; teleport (400 units); turn (149 degrees); a matrix that is not rigid.
- **Measured** (the tutorial fight from a new game at 120, every object of every frame traced): 168,283 object-ticks
  interpolated, 612,484 object frames on the path at their alpha within 0.015 units, 0 across a cut, 0 going back;
  alpha sweeps of the objects alone in the fight step evenly; the same at native 4K and 32:9. Isolation: every
  presented object raised by 750 units, the game's own object positions never took it; seed and paired object positions
  equal to the 30 fps run over 600 ticks (the list differs for single ticks by culling; strict per-tick comparisons
  stay on the Shadow Depths route).
- **Cost:** under 1 us per frame for up to 106 objects; no measurable change in frame rate, CPU, GPU or memory (fight:
  112.7 at 120, 115-129 at 240 and uncapped, run-to-run variation up to 10 %).
- **New options:** `--kuf_frame_interpolation_objects` (on; off draws only the camera and the player between ticks),
  `--kuf_trace_presentation_objects` (debug trace).
- **Tests:** 3 new unit cases, `presentation.objects_120` + `objects_check`, `presentation.objects_isolation` +
  `objects_isolation_check`; the existing isolation and 30 fps comparisons now also offset every world object.
  `state_compare.py` reads rotated logs.
- **Remaining (D4b):** bones (limbs pose per tick), particles, effects and their clocks, world-anchored HUD elements;
  K39 and K41 updated.
- **Regression:** 1,832 of 1,834 ctest tests passed (17,194 s); SDK patches 0001-0010 apply, the game folder unchanged (138 files), no save directory bound or backed up and the save fixtures untouched, no game content tracked. The two failures, `presentation.interpolated_matches_30` and `disabled_matches_30`, share their 30 fps reference run, in which the player's position differed by one 0.01 log step at 6 of 600 ticks: the M6B camera's real-time ease under the suite's parallel load (K43), with D4a inactive in that run (0 object ticks, writes or restores). Rerun on their own: 7 of 7 passed, the tests unchanged.

### D4b-0: K43, the camera timed by the tick (2026-10-27)
Approved with D4b-1 and D4b-2. Details: docs/HIGH_FPS.md 17.1, docs/KNOWN_ISSUES.md K43.

- **Cause:** the M6B camera's dt was the host clock between its updates (`ElapsedSeconds`, `modern_camera.cpp`). With
  the fixed tick it still jitters with the host's load (5-111 ms between updates with four runs at once). The camera
  is simulation state and the player walks relative to it, so every run differed slightly: four runs at once, compared
  at 6 decimals, differed in every pair (the view by up to 0.29 units, the player by up to 0.00018). The 0.01 state
  trace showed it only when a value crossed a rounding step: the once-seen K43 failure.
- **Correction:** with the fixed tick the camera's dt is one tick (1/30 s; the camera updates once per tick in
  gameplay, 2,821 of 2,821 ticks). Its rates and easing per second are unchanged; the original pacing keeps the host
  clock. `--kuf_camera_host_dt` (debug) restores the old timing.
- **Comparisons now exact:** the state trace logs 6 decimals; `state_compare.py` requires every object and the
  simulation camera exactly equal (the camera allowed 0.25 units). Six runs at once (30 fps twice, 120 with and
  without interpolation, the isolation offset) identical in every pair. New: `presentation_host_dt` and
  `presentation.state_compare_detects_host_dt` (the old timing caught by the objects check: the player 0.000062 units
  off, which the 0.01 comparison let through).
- **Tests:** `-L presentation` and `-L camera`: 43 of 43 passed (all M6B camera tests with the new timing).

### D4b-1 and D4b-2: skeletal animation interpolated between ticks (2026-10-27)
Details: docs/HIGH_FPS.md 17.2-17.10, docs/KNOWN_ISSUES.md K39, K44, K45.

- **How the game poses a character (D4b-1):** the tick's scene build evaluates each visible skinned model's pose
  (vf96 -> `sub_8219C8A8`) from its animation layers into a palette: one model-space 4x4 matrix per bone in the slot's
  frame memory, pointer at mesh + 856 + slot * 4. The layers advance one frame per tick in the tick's update;
  transitions are crossfades (a blend layer fading the old clip). Weapons are placed from the palette right after the
  pose; physics-driven parts (class 0x82103828) get their palette from the physics step. Correction to 13.3: the
  characters' palettes come from the scene build, not the physics step.
- **Interpolation (D4b-2):** after the game's own evaluation in the tick, the pose is captured and the interval's plan
  decided; in the tick's own frame and each render-only build the model gets a palette of its own (the game's
  allocator) holding the pose at the frame's alpha, each bone interpolated in its parent's space (slerp, lerp), and
  the tick's pointer back when the next tick into the slot starts. The game's palettes are never written.
- **Transitions:** cut on the game's state: a new model or no earlier pose, the root's own cut (D4a, D3b-3), another
  skeleton or hierarchy, a switch without a crossfade, a bone that does not decompose; crossfades interpolated; one
  backstop (a bone turning over 149 degrees held at the new tick's angle). In the fight: 2,704 clip changes with a fade
  interpolated, 111 cuts (new), 1 (the root's turn), 4 bones held in 267,768 model-ticks.
- **Isolation, a fault found and corrected:** a probe of every function that reads the palette pointer found the
  physics step's parts reading their character's palette after the tick's scene build had pointed it at the tick
  frame's written pose (8,125 of 8,366 calls): presentation reaching PhysX. Those readers (and three not seen running)
  now get the game's palettes for the call: 27,288 physics reads, 0 finding a written palette (gated by
  `pose_check.py`). Shadow Depths per-tick state still exactly equal to 30 fps with the poses written.
- **Visual:** 651,854 poses in the fight each at its frame's alpha (within 0.0001), none across a cut, lengths within
  0.0074 units; sweeps of the poses alone in even steps, ending on the tick's frame; native 4K and 32:9 the same.
  Weapons follow the interpolated hand (5,421 placements within 0.001).
- **Cost:** 75-113 us per frame for up to 55 models and 110 bones; frame rate within the run-to-run spread with the
  poses on and off; 30.00 ticks per second at every rate; memory flat.
- **Found while measuring (K44, not D4b):** the frames between ticks can lock at exactly 30 (at 60) or 90 (at 120),
  with the poses on or off; 60 fps was not reached in three runs. Left for approval.
- **Remaining:** physics-driven parts posed from the presented bones (K45, D4b-3); particles and effects; HUD.
- **New options:** `--kuf_frame_interpolation_bones` (on), `--kuf_trace_presentation_bones` (debug).
- **Tests:** 1,842 of 1,842 passed (full ctest, including `-L presentation` and `-L camera`: 47 of 47); SDK patches
  apply; the game folder and the 47 save files unchanged (sha256); `check_tracked`: clean.

### K46: crash after an in-game save at 120 fps (2026-10-27)
A 120 fps play session crashed 17 ms after saving: a render-only build replayed an index into the renderer's object
array for an object the tick had released after saving its lists. Restored indices are now checked against the object
they named (docs/HIGH_FPS.md 14.8, K46). Tests: `-L "timing|render|presentation|save"` 60 of 60; three 120 fps runs of
`save_continue` passed; 9 indices left out in the 120 fps combat run.

### K44: the frame rate no longer locks at 30 or 90 (2026-10-27)
Details: docs/HIGH_FPS.md 18, docs/KNOWN_ISSUES.md K44.

- **Cause:** the presentation clock kept its phase from tick to tick; one tick starting late by nearly a frame step
  left it where the step before every next tick was too late to build and was skipped, without anything moving the
  phase again (60 -> 30, 120 -> 90). Scheduler only: builds 1-3 ms, render thread idle, reproduced offscreen.
- **Correction:** such a step goes to the next tick's own frame and the clock restarts from that tick's start. The
  safety check before a build, the 30 Hz tick and the no-catch-up rule are unchanged; no busy wait.
- **Measured (fight, 250-350 s):** 30.0 / 60.0 / 89.8 / 119.4 fps at 30 / 60 / 90 / 120; 144 -> 120.3, 240 -> 105.4,
  uncapped 115.8 (render-thread limited on this machine); 30.00 ticks per second at every rate.
- **Tests:** `timing.k44_60`, `_120` (a held tick, `k44_check.py`), `_60_fault`, `_120_fault` (the old clock: the
  check must see the lock), `k44_long_60` (six minutes, no lock).
- **New options:** `--kuf_trace_frame_clock` (debug), `--kuf_debug_tick_stall_at` / `_us` (tests),
  `--kuf_present_clock_keep_late_phase` (fault test); run summary `frames_per_tick`, `late_ticks`, `clock_restarts`.


### D4b-3: physics-driven parts follow the interpolated pose (K45, 2026-10-28)
Details: docs/HIGH_FPS.md 19, docs/KNOWN_ISSUES.md K45.

- **How parts are posed:** a part (class 0x82103828, no skeleton) hangs from a character through a controller
  (0x821014C4); each tick the physics step allocates its palette and writes the simulated pose in the model's space.
  Render-only builds never posed it, so it stayed at the tick's pose while the character moved. Seen on the fight's
  player (a 21-bone tabard), the Shadow Depths character (four parts) and a menu-screen model (63 bones).
- **Interpolation:** each part bone is placed on the bone it moved with most rigidly over the tick (the model's bone
  it hangs from, or the previous link of a chain); its relation is interpolated (rotation, scale, offset on the arc)
  and composed with that bone as presented. Bones with no rigid anchor are interpolated in the model's space; a turn
  over 2.6 rad is held; new parts, a cut model pose and invalid pairs are not interpolated.
- **Physics isolation:** the part's palette is written into the build's frame memory and set back before the next
  tick; the physics readers still get the game's palettes (D4b-2). 0 of 23,598 physics reads in the fight found a
  written palette; the new fault-injection test (`--kuf_debug_physics_sees_presented`) must see one and does.
- **Found and corrected during D4b-3:** in held frames (freezes) a part drew from reused frame memory and vanished,
  caught by `presentation.objects_check`'s objects-only sweep; each part is set back to its tick palette before every
  build.
- **Visual:** fight at 120: 7,688 of 7,691 part-ticks interpolated, each at its alpha within 0.00004, distance from
  the anchor within 0.0143 units of the ticks'; 4K and 32:9 the same; the tabard stays on the body through the sweeps.
- **Cost:** about 10 us per frame more pose writing; 30.00 ticks per second; frame rate within run-to-run spread.
- **New option:** `--kuf_debug_physics_sees_presented` (fault test). New tests: four unit cases,
  `presentation.bones_physics_fault` and its check; `pose_check.py` checks the parts.
- **Tests:** 1,857 of 1,858 passed (full ctest); `gameplay.combat_loot_native` failed the loot rule in the full run
  and its first rerun (no gold, no items: K24's random drops; at 30 fps with no presentation code active, 0 render-only
  builds) and passed on the second rerun. `-L presentation` 33 of 33 (`combat_loot_120` passed there on its second run
  after a no-loot run), `-L "presentation|camera"` 49 of 49; SDK patches apply; the game folder and the 53 save files
  unchanged (sha256); `check_tracked`: clean.

### K46 regression test (2026-10-28)
The K46 correction had no test of its own. Scripted runs never put a released index in the list whose consumer
crashed (the scene setup list), so the test injects it: `--kuf_debug_restore_stale_every` adds a released and a
reused index to the tick's saved list every N ticks. `render.k46_released_120` runs that through the Quit save and a
new game's autosaves at 120 fps (every record left out, no crash); `render.k46_released_fault`, with the correction
off (`--kuf_debug_restore_released`), must crash in `sub_821A2FD8` as the play session did, and does. Details:
docs/HIGH_FPS.md 14.8.

### K24: the loot test made repeatable (2026-10-28)
`gameplay.combat_loot*` sometimes picked up nothing: no gold, no item. Cause: which drop a dead unit gets is decided
by `sub_8256C280` from the game thread's CRT rand() (four chance rolls, then the gold amount or the item table), and
the scripted fight is not repeatable (seeding the game's sequence before the fight was tried: the runs part 915 ticks
later), so a run could meet only no-drop seeds. No loot is a valid outcome of the game's rules, which stay unchanged.
- **Test fixture:** `--kuf_test_drop_seed` (tests only) starts each drop decision from a known seed (0x370085B5,
  recovered from an observed gold drop); the game's own rolls, chances and tables decide, and every Red Lizardman
  death takes the gold path (8 gold). The fight, the kills and the rest of the game's randomness stay real.
- **Fault test:** `--kuf_debug_skip_drop_decisions` skips the decisions; `gameplay.combat_loot_no_drops` must fail the
  loot rule and nothing else (EXP still gained), and does.
- **Validation:** 10 of 10 seeded runs passed (1280x720, 120 fps and native 2560x1440; 13-23 gold-path decisions
  each); `k24_check.py` on each; the registered tests 6 of 6 (the fault test included). Still open (K24): a
  death in the fight fails the run.

### D4b-4: HUD, particles and effects (2026-10-28; docs/HIGH_FPS.md section 20)
- **HUD (D4b-4A):** every HUD entry traced per tick (`--kuf_trace_hud_from`) through the walk's camera turn (120,
  native 4K, 32:9) and 600 ticks of the fight: no element is placed from the world (gauges change width, the minimap
  marker steps 1 pixel in seconds, a menu highlight jumps between fixed slots). Nothing in the HUD changed;
  `hud_check.py` guards it (an element moving with the world fails; its self-test shows markers at 30 or 15 Hz caught).
- **Effects:** the effect shaders' time (the frame timer's count / rate, read by four draw functions on the render
  thread) is now each frame's own moment: the rate is changed for those draws only so that the time is
  (count - lag) / 30, and put back; the count is never written, alpha 1 is the original frame. Vegetation sway and the
  ground mist move between ticks (effects-only sweeps 0.11-0.33 levels in even steps). A tick-start check counts any
  rate left changed (0 in every run; the fault test is caught).
- **Left at 30 Hz, with reasons:** GPU particles (their time is a whole frame number; drawn earlier a particle born in
  the newest tick would be shown before it existed), weapon trails, flipbooks, the per-tick radius list.
- **Found on the way:** one frame in 32,000 was placed 0.18 ms before the frame handed before it (since D3b-3, camera
  and objects too); `FrameAlpha` now never places a frame before the tick's previous one. `render.k34_check` failed once
  under parallel load: its two runs captured a few ticks apart relative to gameplay and the mist differed; the captures
  are now at fixed ticks (thresholds unchanged).
- **Performance:** no measurable cost (240: 183-185 fps on, 184-191 off in the walk segment; the fight segment varies
  101-141 run to run either way); 30.00 ticks per second, 0 rand() and 0 PhysX calls in render-only frames at every
  rate; the save chain at 120 passed with the effect clock traced (90,000 frames, 0 leaks, 0 back in time).
- **Also in this checkpoint:** the K46 regression test (14.8) and the K24 loot fixture (above).
- **Regression (2026-10-29, at c83c6d0):** 1,876 of 1,876 (`ctest -j6`); SDK patches apply; the game folder verified
  before and after; 53 save files unchanged (sha256); tracked files clean.

### D5: the frame rate in PC Settings (2026-10-29; docs/HIGH_FPS.md section 21)
- **Menu:** two new pages at the end of the game's PC Settings (in game and on the title screen), reached with Down
  past the display page: FPS (30 (Original), 60, 90, 120, 144, 165, 240, Unlimited; a custom 31-480 from the file)
  and Interpolation; Advanced: World Objects, Animation, Effects. Keyboard, controller and mouse; no external tool.
- **Storage:** `[frame_rate]` in pc_settings.toml; default 30 with interpolation on, so an existing installation is
  unchanged until the player raises FPS; invalid values fall back one by one with a warning.
- **Runtime:** every change applies within one tick without a restart (`ApplyFrameRequest`: the render thread idle,
  the presentation values restored and the history dropped, the guest vblank and the frame clock reset); 3.8-6.3 ms in
  game. The game's state per tick over 6,700 ticks of switching equals the same route without the changes.
- **Command line:** a `--kuf_frame_*` flag (or REX_ value) wins for that run, flag by flag; the file keeps its value.
- **VSync:** none offered (the host presents IMMEDIATE; the only limiter is the frame clock, K48).
- **Performance:** every limit holds; 60 everywhere incl. 4K and 32:9; above 60 the scene and GPU set the rate
  (1280x720: 127-219 fps in the camp, 75-164 in the fight); 29.9 ticks per second in every run. The title screen stays
  at about 36 (K47).
- **Tests:** 4 unit cases and 22 game tests (`settings.frame_rate_*`), including the fault run.

### D5 validation (2026-10-30; docs/HIGH_FPS.md section 22)
- **Why the D5 regression stopped:** `presentation.check` held 14 GB (all 4K captures as float64) and freeze runs
  held two copies of guest memory (9.8 GB), beside the desktop's ~15 GB. Fixed: 2.0 GB under a peak-memory guard
  (`tools/peak_rss.py`, self-tested), freeze runs 6.8 GB. One game at a time (resource lock), no orphans, no
  accumulation; every non-game test now peaks at 2.4 GB or less.
- **Regression found and fixed:** an effect-clock check race (false leaks), a physics part bone drawn through its
  anchor when its offset flipped in one tick (K52), the HUD trace's tick labels for slow frames (K53).
  `timing.k44_120_check` failed once (58 busy ticks of 848) and passed three repeats (load burst; K49 notes the clock
  phase).
- **Runs (ctest -j4, fixture-group batches, docs/HIGH_FPS.md 22.2):** round 1 (cbb6931 tree): all batches passed except
  the two faults above in the freeze re-run; round 2 (cca44ac): 1,902 of 1,904 passed (k44_120_check, hud_check_walk);
  round 3 (8c70d75): 1,783 passed, 0 failed, then stopped by the roadmap's testing policy (full regression deferred
  to feature freeze; 121 tests not run on the final D5 build). **Not a clean pass of the complete suite on 8c70d75.**
- **90 fps:** not reproduced (88.6-89.9 fps in every segment of two runs). **Title screen (K47):** the movie player
  sleeps on the render thread inside each frame (thread dumps); D6 options documented.
- **Integrity (2026-10-30):** SDK patches apply; game folder unchanged (138 files); 53 save files unchanged
  (sha256); tracked files clean.
- Commits: cbb6931, 90bf881, d0e5a5a, cca44ac, 8c70d75 and the docs.

## M8: performance and stability optimisation (branch m8/performance, from m7/feature-completion 3eb605e; 2026-10-31)

The player's M8 brief: evidence-driven performance and stability work with automated, sandboxed benchmarking and
stability testing (game launches authorised for isolated tests; no multiplayer, no network, no push or merge).
Report: docs/PERFORMANCE.md section 7.

| Item | Status |
|---|---|
| Benchmark framework (tools/bench.py, the matrix, three workloads, GPU timestamps and sections: SDK patch 0012) | IMPLEMENTED, VERIFIED (78 matrix runs and the A/B runs, none invalid) |
| Baseline (39 runs) and final measurement (39 runs) | VERIFIED |
| CPU / GPU bottlenecks | investigated: GPU (4K) render target transfers, texture loads from resolves, resolves; CPU (1440p+) the GPU command processor and the D3D ring wait |
| Render target extents (K73: execute_unclipped_draw_vs_on_cpu on) | IMPLEMENTED, VERIFIED (4K/120: 77.6 -> 90.1 fps, 1 % low 33.2 -> 43.0, p99 26.1 -> 22.3 ms; render equivalence within 2 levels) |
| Frames stepped from the tick frame's hand-off | IMPLEMENTED, VERIFIED (60 fps jitter roughly halved; timing / presentation / render tests pass) |
| Adaptive frame rate | IMPLEMENTED (opt-in, unit-tested; even 60 at 4K/120 instead of an uneven 90) |
| Register-write lookup (SDK) | tried, no measurable gain, REVERTED |
| Fast loading | NOT IMPLEMENTED: loading is paced by the game tick (10.1 s -> 3.1 s uncapped, diagnostic); needs the loading state identified |
| 120 fps at 4K | NOT REACHED (about 90; GPU-bound). At 2560x1440 / 1920x1080 a 120 limit gives 101-114 fps (CPU side) |
| Memory and resource lifetime | VERIFIED for the measured workloads (no growth); stability: 22 of 22 stability runs passed (30 minutes at 120 fps: +4 MB RSS after the first quarter; play under real memory pressure, memory.high 800 MB) |
| Co-op disconnect crash (K72) | PARTIAL: static analysis only; BLOCKED for verification (no multiplayer sessions in M8) |
| Regression | timing, presentation, render, camera, gameplay: 92 of 96 pass; 3 pre-existing failures (K74, the same with the baseline build), 1 intermittent route timing |
| Original game files and saves | VERIFIED unchanged (the game folder's 138 files match the manifest, nothing in it newer than M8's start; no save fixture, manual save or backup modified; no game content tracked) |

Commits: 5f38941 .. HEAD on m8/performance (tools, patch 0012, K73, pacing, docs).

## M9: deep GPU, Vulkan and ReXGlue optimisation (branch m9/deep-optimization, from m8/performance 61a061f; 2026-11-01)

The player's M9 brief: per-pass GPU and CPU profiling, Vulkan synchronisation, render targets, shaders, the 1440p
extent-estimate question, no visual-quality reduction, at least 5 interleaved runs for claims, the stale M8 regression
routes. Report: docs/PERFORMANCE.md section 8.

| Item | Status |
|---|---|
| Profiling: per-frame Vulkan command counts, per-pass GPU timeline, GPU idle, texture copies section, extent-estimate CPU cost (SDK patch 0012); GPU clock / temperature / power per run and interleaved runs (tools/bench.py) | IMPLEMENTED, VERIFIED (instrumented build within the M8 build's noise, 5+5) |
| Render-to-texture results loaded straight into their images (SDK patch 0013, on by default) | IMPLEMENTED, VERIFIED: 4K/120 vs M8 (5+5, own caches): 91.2 -> 95.5 fps, 1 % low 41.5 -> 46.2, p99 22.2 -> 19.8 ms, jitter 5.6 -> 3.2 ms, GPU 10.07 -> 8.83 ms; 1440p / 1080p no measurable change; gameplay captures bit-identical (`render.direct_load`) |
| Upload look-ahead, one submission per frame, submissions every 128 draws | tried, slower or no gain, REVERTED / not adopted |
| execute_unclipped_draw_vs_on_cpu at 1440p | MEASURED: no trade-off (0.008 ms CPU per frame; on is equal or better); M8's claim corrected (7.5.1, K73) |
| K74 stale regression routes | FIXED (routes only, assertions unchanged); all five pass |
| Vulkan validation | no message from the new path; pre-existing SDK messages recorded (K75) |
| Regression / stability | 115 of 116 + the reworked `render.direct_load`: pass; stability 21 of 21 |
| 120 fps at 4K | NOT REACHED (95-101 fps, GPU 8.8 ms + 0.9 ms idle per frame) |
| Game folder, saves, tracked files | VERIFIED unchanged / clean |

Commits: ca5de18 .. HEAD on m9/deep-optimization.
