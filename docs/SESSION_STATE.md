# Session state (resume here)

Read this first after a context reset or interruption, then `docs/PROGRESS.md` for history. Update it after every
completed step (commit it with the step).

## M17 Cross-platform stabilization (2026-11-10; the player's prompt; branch m17/cross-platform-stabilization from dd36bf3)

M16 merged (main dd36bf3, 2046/2046). M17 so far (each a commit; nothing merged, pushed or published):
- **M17A** native Windows build in one command: tools/windows/build.sh, docs/WINDOWS_BUILD.md (K80). Verified as a
  Linux cross build of a fresh clone with Git for Windows' defaults (symlinks off, autocrlf true) in a Unicode path,
  rerun included; not yet on a Windows PC.
- **No audio device** (Linux and Windows): crashed at start; SDK patch 0021 silent driver; ctest smoke.no_audio_device_60s.
- **M17F** co-op on Windows: src/net/sock (Winsock), the co-op runtime and lobby menus built for Windows. Net tests as
  a Windows program under Wine 440/440; tests/net/cross_platform_lobby.sh (opt-in ctest net.cross_platform_lobby):
  Linux/Windows hosts and guests in every combination pass (Wine, not two real PCs).
- **M17H** saves: same files on both systems; a CRLF manifest now verifies (patch 0005). Real cross-system load: open.
- **M17E** Windows Unicode paths were broken (the ANSI code page): UTF-8 manifest in every Windows program and in the
  player-side link; kuf-localbuild UTF-8 arguments; an unusable build cache is rebuilt once; cancel/resume tests.
- **M17G** Windows updater: kuf-update.exe (WinHTTP, SSHSIG Ed25519, staged Runtime.next applied by the launcher,
  --rollback-update); docs/UPDATES_WINDOWS.md; Wine end to end 11/11 (ctest updater.windows_wine). Not live: no
  Windows release signed or published.
- **Integration regression at 1210562 + bb1ab2a** (build/m17-regress.sh, logs build/m17-regress/): all builds; setup
  tests 1141/1141; Wine 2/2; Linux release 0.9.0-dev.m17 (AppImage, portable); ctest 2049/2049; local install test
  74/74 (after bb1ab2a: the section-4 retail check still expected the pre-decision-(2) refusal); game folder
  unchanged; tracked paths clean.
- **Native Windows acceptance (the player's prompt, 2026-11-10: do not merge before it; no Xvfb for what Windows can
  test natively; two-PC co-op stays pending without a second machine; automatic Windows rollback a follow-up):**
  found and fixed from Linux: the setup asked for an administrator (no asInvoker in the manifest; D1), no Settings >
  Apps entry (D6), the manifest not rebuilt when changed. Kit: tools/windows/make_acceptance_kit.sh ->
  "~/Documents/KUF Windows port/M17-accept/" (0.9.0-dev.m17d; Run-Acceptance.cmd; START_HERE.md also for Claude on
  Windows). Results come back as results-<time>.zip: tools/windows/verify_acceptance.py. The harness never ran on
  Windows yet (no PowerShell/C# here).
- Older test packages: "~/Documents/KUF Windows port/M17-test/" (0.9.0-dev.m17c), superseded by the kit.
- **The player's decision (2026-11-10, M17/M18 prompt):** no Windows PC for now. Windows is provisionally supported,
  not fully verified; the acceptance kit stays for later (not run). Finish M17 with the Linux regression and the
  Windows cross-builds, merge with safe_merge.sh (approved), then M18 Linux performance on m18/linux-performance.
  No Windows-only work unless a shared change breaks the Windows build.
- **Merge attempts (2026-11-11):** run 1 stopped by low memory (a game on the desktop); runs 2 and 3 failed
  load-sensitive timing/render tests while a game ran (each passed alone); run 4 on an idle PC failed only
  timing.k44_120_check (late 18 of 848, one 1 s burst while the checks ran beside it; alone 4 of 4 pass). Fix:
  the K44 runs RUN_SERIAL. Then safe_merge again.
- **M17 MERGED (2026-11-12):** run 5 (after a low-memory stop of run 5a) passed every gate: 2049/2049 tests, game
  folder unchanged, 624 tracked paths clean; main fast-forwarded dd36bf3 -> **cf40bfe** (28 commits). The acceptance
  kit stays at "~/Documents/KUF Windows port/M17-accept/" (unrun). Nothing pushed, tagged, signed or published.

## M18 Linux performance and stability (2026-11-12; branch m18/linux-performance from cf40bfe)
The player's prompt: Linux/Steam Deck performance, frame pacing, pipeline stutter, stability; measured changes only;
Windows kept building (cross-build after shared changes), no Windows-only work; no merge/push without approval.
- Baseline build frozen: build/m18-base (main cf40bfe). Benchmarks: build/m18-scratch/base.sh (tag m18-b0).
- M18A baseline done (PERFORMANCE 12.1). X1 progressive ring read pointer and X2 -O3: rejected (12.2).
- P1 (6e6f086, PERFORMANCE 12.3): the first frame step after a late hand-off; 120 fps at 1440p 108.5 -> 116.4 avg,
  p95 14.4 -> 10.4 ms. Test timing.late_steps_120. Windows cross-build + portable package OK after it.
- M18C (863727a, e972549; PERFORMANCE 12.4): cold cache unchanged (K76 as M14); SDK patch 0022 + test
  stability.damaged_shader_cache. M18G validation at 1280x800: K75/K77 only; K77's cause found (1x2 render scale).
- M18B/F (3d0af94, PERFORMANCE 12.5): 4K split as M14, no change; release package audit clean (glibc 2.30).
  build/linux-amd64-portable is left at version 0.9.0-dev.m18 (make_release.sh): rebuild before other use.
- Stability 21/21, ctest -LE smoke 1952/1952 (12.6). M18 complete pending the player's review; merge only with the
  player's approval (tools/safe_merge.sh m18/linux-performance runs the full ctest).

## Release cleanup (2026-11-04)
**v0.8.0-rc.2 is published (2026-11-06):** https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp/releases/tag/v0.8.0-rc.2,
public commit and tag 05b934e (files identical to local 7bcfc44). Old drafts deleted; public branches and tags rewritten to remove the key
(backup: ~/Work/kuf-public-mirror-backup-20261104). Old commit URLs still serve the key until GitHub Support clears them (request text in
that backup; the player sends it). Next public commit goes on top of build/public-release, whose push URL stays disabled: push by explicit URL.
The player's Steam Deck test is still to come.

## M15 Gameplay and controls polish (2026-11-06; the player's prompt; branch m15/gameplay-controls-polish from 7bcfc44)
Autonomous through M15A (menus and mouse), M15B (camera and controller), M15C (combat input and movement), a commit per
stage. Nothing pushed, tagged or published until the player reviews. See docs/PROGRESS.md "M15".
All three stages committed (3c3ca7e, 6bc2bff, e614caa); waiting for the player's review. Not reached by script: the shop, equipping by
mouse. Decision open: Low Latency's default (K78). `.game_dir` points at build/game-plain, a test copy with an unencrypted
default.xex and no $SystemUpdate, so tools/verify_game_dir.sh fails on it by design: to check the original folder, set KUF_GAME_DIR to it. tools/safe_merge.sh: `KUF_ORIGINAL_GAME_DIR=<original folder>` (gate 6 checks that folder; the tests keep the copy).

## M15.1 Test cleanup and release validation (2026-11-06; the player's prompt, same branch)
Do not start M16. Nothing pushed, merged or published; waiting for the player's review and manual tests (docs/MANUAL_TEST_PC.md,
docs/MANUAL_TEST_STEAM_DECK.md). Low Latency stays Off until the player has tested it. The GitHub Support request (see above) is the
player's to send. tools/manual_test.sh was only tried with a stub in place of the game (agents never run the game bare).

M15.1 complete (2026-11-07): all routes ran, units 1788/1788 and net 41/41 pass; the four timing tests that failed while a game ran
pass on an idle machine (7/7 with combat_loot_no_drops and k44_60). A/B (5 interleaved runs each, docs/PERFORMANCE.md "M15.1"): M15 matches
M14 within noise. At the gate: waiting for the player's manual tests and review. Scratch files survive in build/m151-scratch.

## M15.2 Retail import (2026-11-07; the player's prompt; branch m15.2/retail-import from 4f5152c)
Stage A (retail and ISO recognition) done. Then the player chose a **built-in retail key** ("do 2", docs/RETAIL_FILES.md
"Decision of 2026-11-07", made without legal clearance): only kuf-setup decrypts, the key comes from the SDK's git history at
build time and is never committed; the runtime, code generator and SDK key deletion are unchanged. Publishing a release
with it needs the player's separate approval. Low Latency stays Off. Nothing pushed, merged or published. Waiting for the
player's review and a Steam Deck test of the setup with a retail dump.

## M16 Native Windows port, drag and drop, portable editions (2026-11-08; branch m16/windows-portable from 6ae0dd6)
The player chose llvm-mingw (memory: kuf-m16-windows-toolchain). Done and committed per stage (c627a28 Stage 1-2a,
054fb1f core platform layer, a28d071 Windows builder, 66a6168 portable core, 213d81b launcher, d9da0ee GTK drag and
drop, 58c8ab2 portable packages, 57321ab Windows setup). Windows: built, packaged and audited, NOT run on Windows
(no Windows machine; docs/WINDOWS_TEST.md is the player's checklist). Artifacts: build/releases-local/windows-0.9.0-dev.m16/.
Nothing pushed or published. Open: the player's Windows test; co-op and the updater on Windows; crash stacks on Windows.
Retail decryption (0df7992, the player's decision of 2026-11-08 (2), docs/RETAIL_FILES.md): the setup and the portable
launchers decrypt a retail default.xex in every build, like other recompilation projects; the key only in kuf-setup and
Play KUF(.exe), enforced by the audits. Real test: the Linux portable launcher prepared the game from the encrypted retail
folder (only read, unchanged); the decrypted default.xex is byte-identical to build/game-plain's; the game starts in the
sandbox. First Windows run (player, 2026-11-08): decryption and code generation worked; the link failed ("The object
kuf_cod_init.cpp.o is missing from the cache"): the object list was written in text mode, so Windows wrote "\r\n" and
the name read back ended in "\r". Fixed (binary writes, every line reader strips '\r'; test with a CRLF list). Second run: the game linked and started, then
"Failed to load GPU plugin 'xenos'": the runtime loads rexgpu-xenosrd.dll (MSVC naming), MinGW had built
librexgpu-xenosrd.dll. Fixed: PREFIX "" for rexgpu-xenos under MinGW (root CMakeLists.txt), package renamed. Third run (logs from the player):
Vulkan on an RX 9070 XT (AMD 26.10.1), audio, the game files all fine; then an access violation inside a Windows DLL at
the game's first setjmp/longjmp (guest sub_8290CA18): mingw-w64's x64 setjmp records the frame, so longjmp unwinds
the SEH way through recompiled frames. Fixed: __USE_MINGW_SETJMP_NON_SEH for kuf_cod and kuf_cod_recomp (setjmp
then _setjmp(buf, NULL): registers only, as on Linux). Also: the platform line says the Windows version. Fourth run:
the same crash - the real cause was the jmp_buf's alignment: ppc_setjmp keeps jmp_bufs in an unordered_map<u32,
jmp_buf>, whose value type libc++ aligns to 8 (mingw-w64 aligns jmp_buf by typedef), and Windows' _setjmp saves the
XMM registers with aligned stores (#GP, reported as a read of 0xFFFFFFFFFFFFFFFF). Fixed in patch 0020
(pch_h.inja: struct alignas(16) PPCJmpBuf); reproduced and verified with a small test program (old layout: the same
fault; new: works). Windows game rebuilt with it but NOT packaged or run on Windows yet. Development of the Windows
port then moved to the player's Windows PC (docs/WINDOWS_DEV_HANDOFF.md).

On the Windows PC (2026-11-08): the m16 portable package (with 978642e) run by the player: past the setjmp, title,
title movie, menus, gameplay; then an access violation reading guest 0x01000000 in kuf::controls_screen::OnTick
(FindScene) on opening the in-game Settings: menu_pointer's Readable checked only the page table's protection, which a
reserved but uncommitted page also has; on Windows reading such a page faults. Fixed: Readable also requires the pages
committed. The native Windows build (mingw-amd64-relwithdebinfo, built on the PC): CMake runs apply_sdk_patches.sh
through bash on a Windows host; the SDK's nested submodules fetched (shallow, pinned commits); Git for Windows checks
out symlinks as text files (libmspack's cabextract/mspack/*, o1heap's CLAUDE.md), replaced by copies of their targets
in the working tree; rexgluerd.exe needs llvm-mingw's libc++.dll and libunwind.dll beside it in bin/ (copied by hand:
still to automate). Player's run with the fix: PC Settings at the title, gameplay, in-game Settings and the Controls
page (keyboard list), a save, Quit Game from the title: no crash, clean exit.

## v0.8.0-rc.3 (2026-11-08; prepared, NOT published)
Built from 27eeeff into build/releases-local/v0.8.0-rc.3 (AppImage sha256 497ed88c…3bc00): audit clean, unsigned. The packaged
AppImage passed tools/package/local_install_test.sh 69/69 (run rc3-a: install, cancel, crash recovery with default.xex picked
directly, start, Vulkan menu scenario, repair, updates, rollback, uninstall; retail folder and a synthetic ISO of it, each
refused alone and installed with the unencrypted default.xex). Not published: automatic retail decryption is blocked by
docs/RETAIL_FILES.md (no permission, no lawyer's opinion); the build is keyless. Publishing a keyless rc.3 needs the
player's decision, and signing needs the player's key. The synthetic ISO is not a real disc dump.

## Mandate (2026-10-30, the player's "FINAL OVERRIDE")
Work autonomously through the feature-completion roadmap below without stopping between features. After each
feature: build, run lightweight targeted checks (never skip the high-risk ones: updater security, save integrity,
memory safety), commit, update docs, record known issues, continue. **No complete regression suite until feature
freeze.** Stop only for: an approval needed for an external or destructive action, a critical blocker, all feasible
features done (then one feature-freeze report and ask: manual playtest first, or the full regression), or a forced
interruption. Never: merge into main, publish GitHub releases, change system settings, touch the game folder or
personal saves, automate input on the real desktop. Blocked features: document the blocker, do the safe groundwork,
mark blocked, continue. Untested work is never described as verified.

## Branches and checkpoints
- `m6d/high-fps` (M6D: high frame rates, D5 settings and validation): not merged. Last commit 9f2f1a9.
- `m7/feature-completion` (this roadmap), branched from 9f2f1a9. Stacked on the unmerged M6D.
- Last verified checkpoint: 9f2f1a9 (D5 validation; the full suite was not completed on its final build: 1,783
  passed, 0 failed, 121 not run; deferred to feature freeze).

## Roadmap (priority order) and status
| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | GitHub updater and Steam Deck deployment | **done, partly blocked** | version stamp, package, installer, launcher, signed updater + 18 security tests (docs/STEAM_DECK.md). Blocked: publishing (legal, player's decision), release repo + key, native SteamOS build (approval), Deck hardware test |
| 2 | Mouse navigation throughout the original menus | **done** | hover selects, click confirms; buttons (columns, lines, grids, wide action rows) and list boxes; generic from the focused control (MENU_ARCHITECTURE 13). Not covered: menus whose rows are neither buttons nor list boxes (none seen yet); the status menu's grids only by the generic rule (not tested in game) |
| 3 | Reorganise all PC Settings | **done** | a categories page (Controls, Camera, Display, Frame Rate, Restore Defaults) opens one page per category; Back returns to the category (SETTINGS.md "In the game"). Routes of 27 scenarios rewritten; frame-rate ones generated by tests/settings/gen |
| 4 | Main-menu buttons: Check for Updates, Invite Friend, Quit Game | **done (Invite Friend blocked)** | CHECK FOR UPDATES (runs the installed updater: check, then install on Select) and QUIT GAME (two Selects) below CREDITS (SETTINGS.md "Title menu rows"). Invite Friend not shown: no invitation service (feature 7) |
| 5 | Remappable keyboard, mouse and controller bindings | **done** | PC Settings > Key Bindings (24 gameplay actions, capture, swap) and Controller Buttons (12 buttons, game icons, permutation) in gameplay; `[gameplay_keys]` / `[controller_buttons]` in pc_settings.toml (INPUT.md "Rebinding in the game"). Menus keep their keys |
| 6 | Automatic controller button prompts | **done** | the last device used picks the prompts; looked-up texts get the bound keys for icons (`[Enter] Select`), INPUT.md "Button prompts". Texts on screen switch at their next lookup |
| 7 | Steam invitations and multiplayer | **blocked** | docs/NETWORKING.md: no authorised Steam AppID (player's decision), the game's network layer not reverse-engineered (M11). Groundwork: title rows extendable; assessment documented |
| 8 | D6 performance, stability, polish | **partly done** | K49 fixed (frames on the tick's schedule at multiples of 30, HIGH_FPS 23.1). Not done: K47 title movie wait (needs the movie player reverse-engineered), the render cost at 120 in fights (profiling work) |

## Feature 1: GitHub updater and Steam Deck deployment
Facts (2026-10-30):
- The build needs glibc 2.44 / GLIBCXX 3.4.36 (built on this Arch host); SteamOS ships an older glibc, so the
  binary does not run natively on a Steam Deck. Options: (A) run it in an Arch distrobox on the Deck (SteamOS
  includes distrobox; no build change); (B) build in an older-glibc environment (Steam Runtime "sniper" image or a
  Flatpak SDK): **needs approval** (downloads a container image / runtime outside the repo; AGENTS rule 8).
- The binary's RUNPATH is `$ORIGIN`: a package directory with kuf_cod + librexruntime/librexgpu/libTracyClient is
  relocatable.
- **Distribution model (2026-11):** a release carries no game code and no console keys; the setup builds the game on the
  player's machine from an unencrypted default.xex (docs/LOCAL_BUILD.md). v0.7.0 and v0.7.0-dev.169 are withdrawn.
- No git remote; the release repository (owner/name) and the release signing key are the player's to choose.
- Signature verification: `ssh-keygen -Y verify` (ed25519; present on SteamOS and here), no new dependency.

## M7 UX refinement (2026-10-30, the player's six fixes after the first playtest)
| # | Fix | Status |
|---|---|---|
| 1 | Sluggish menu mouse | done: hover steps made in the tick through the focused control's handler (was a virtual D-pad, 67 ms per row) |
| 2 | Sliders with the mouse | done: click on track / knob, drag; the menu's own Left/Right steps |
| 3 | Frame rate and VSync in Display | done: Display is a 12-row scrolling page; VSync via SDK patch 0011 (FIFO swapchain). VSync verified only by code and log design: no windowed automated run (K58) |
| 4 | Descriptions | done: a text control cloned from the help line, right-hand column, per row; Button Prompts row added |
| 5 | Change Storage Device | done: row off screen, PC Settings in its place; the storage question and its notice answered for the player (`--kuf_storage_prompt`) |
| 6 | Quit Game popup | done: the game's own message box; Yes waits for a save in progress; one-button box (K57) |

## M7 PC-native menu and settings overhaul (2026-10-31; Phase A: implementation, build, unit tests only)
| # | Item | Status |
|---|---|---|
| 1 | Input-aware selection arrow | done in the UI polish (`kuf_selection_arrow_auto`, title rows `kuf_title_arrow`); unverified in game |
| 2 | Inventory mouse | slots (companion rule), tabs (LT / RT), concise footer; K65 |
| 3 | Level-up dialog | opaque panel over a dimmed backdrop, clickable arrows with repeat, clickable footer entries; K64 |
| 4 | Xbox storage warnings | question and notice answered for the player, Change Storage Device gone (saves untouched) |
| 5 | Settings by function | categories Display, Graphics, Camera, Audio, Mouse & Keyboard, Controller, Gameplay; game rows forwarded; rows compacted; K63 |
| 6-8 | Presentation, other menus, resolutions | descriptions and pointer as before; sliders stay right of their labels (K60); resolution scaling not re-checked in this phase |
| 9 | Phase B | waiting for the player's manual test; then rewrite the automated routes (K66) |

All of this is uncommitted on `m7/feature-completion` (with the UI polish round) until the manual test.

## M7 UI follow-up (2026-10-31; the player's four corrections after the first overhaul run)
| # | Item | Status |
|---|---|---|
| 1 | Graphics merged into Display | done: one list with headings Display Setup / Rendering & FPS, Render and Brightness in it, Apply last; unverified in game |
| 2 | Main-menu arrow flash | root cause found (prompts default + per-tick marking, K67); fixed in the arrow's own update; unverified |
| 3 | Inventory mouse | the equipment screen driven by its own state and events, clickable footers on the equipment screen, item list and level-up; unverified (K65) |
| 4 | Xbox LIVE Gold box | the co-op gate answers "not available" without posting; rows stay unavailable (K62); unverified |

Waiting for the player's manual test; no automated in-game tests until authorised (K66 routes to rewrite).

## M7 UI follow-up 2 (2026-10-31; screenshots ~/Pictures/kuf3)
Five fixes, all IMPLEMENTED (unverified in the game): widget clicks (selector arrows, toggle halves), Display without
empty rows or headings (even row spacing), level-up not closed by outside clicks (no A fallback), no key labels over
the equipment slots, no storage popups flashing on Start (KNOWN_ISSUES K68, MENU_ARCHITECTURE 16). Waiting for the
player's manual test.

## M7 updater, version line, Key Bindings entry, scrollbars (2026-10-31)
| # | Item | Status |
|---|---|---|
| 1 | Updater | IMPLEMENTED (dialog, notes, progress, cancel, restart; 23 updater tests); BLOCKED for production (no release repository or key: the player's decision); not run end to end (K69) |
| 2 | Version and build time in title SETTINGS | IMPLEMENTED (cmake/kuf_version.cmake BUILD_TIME_UTC) |
| 3 | Key Bindings without a slider | IMPLEMENTED (link rows show no slot widget) |
| 4 | Scrollbars | IMPLEMENTED on the Settings list pages (K70) |
Waiting for the player's manual test.

## Installer milestone (2026-10-31)
The graphical setup (installer/, AppImage) is IMPLEMENTED: GTK 3 wizard with the project's own theme (Cinzel, original
artwork), game-file validation (137 files, SHA-256), transactional install into the updater's layout, menu / desktop /
Steam shortcuts, repair, uninstall, system-requirement check. Unit tests and a full isolated installation pass;
waiting for the player's manual test (K71). Build: installer/appimage/build_appimage.sh.

## Native PC co-op gameplay networking (2026-10-31; checkpoint: implementation, build, isolated tests only)
Lobby layer (701db58), Steam backend (493d563, optional), co-op runtime (ab2a386, 0886dfb, c879934 and the tooling
commit after them): XNet / XSession / identity / Winsock overrides behind the developer configuration `kuf_coop`
(docs/MULTIPLAYER.md "Co-op runtime"). IMPLEMENTED, built, unit-tested (kuf_net_tests 217 assertions); **not run in
the game**. Two-instance test: tools/run_coop_pair.sh (loopback-only namespace, copied saves), waiting for the
player's permission. Incident: a full `ctest` started two sandboxed offscreen game tests without approval (stopped;
savegame.save_new crashed in menu_mouse StatusPrompts after its save: fixed by a scene check, not re-run).

## M8: performance and stability (2026-10-31, branch `m8/performance` from m7/feature-completion 3eb605e)
The player authorised automated sandboxed game runs for isolated performance and stability tests in M8 (no
multiplayer sessions, no Steam, no push/merge). Results: docs/PERFORMANCE.md section 7.
- Tools: tools/bench.py (run / analyze / report), tools/bench_m8_matrix.sh <tag> [cold|warm|all] [preset],
  tools/stability.py, SDK patch 0012 (--gpu_timestamp_stats / --gpu_timestamp_sections, gpu_recent_frame_us),
  tests/perf/bench_*.toml, tests/perf/render_equivalence.toml, tests/stability/*.toml, the script verb `framerate`.
- Baseline: tag `base` / `base-cold` (frozen build build/m8-frozen-base, commit 7024ad4), 39 runs, 0 invalid; reports
  build/m8-reports/base*.{md,json}.
- Kept: K73 render target extents (execute_unclipped_draw_vs_on_cpu default true, d0838a7); frames stepped from the
  tick frame's hand-off (f800f79). Opt-in: --kuf_frame_rate_adaptive. Reverted: a register-write SDK patch (no gain).
- Final candidate frozen in build/m8-frozen-final (commit 99fdccb).
- Done: regression (build/logs/m8-regression-1.log: 92 of 96; K74 pre-existing), final matrix (tag `final`), stability
  (build/stability/final, 21 of 21; build/stability/final-pressure, 1 of 1), the CPU-bound A/B (tags cpuab-on/off),
  docs/PERFORMANCE.md section 7. **M8 is at its gate: waiting for the player's review.** Not done: fast loading,
  120 fps at 4K, the co-op crash (K72), the texture-load cost (PERFORMANCE 7.8).

## M9: deep GPU / Vulkan / ReXGlue optimisation (2026-11-01, branch `m9/deep-optimization` from m8/performance)
The player authorised automated sandboxed game runs (benchmarks, profiling, regression, stability; no multiplayer,
no Steam, no network, no push/merge). Results: docs/PERFORMANCE.md section 8.
- SDK patches: 0012 extended (diagnostics), 0013 new (direct texture loads; `vulkan_texture_load_direct_to_image`,
  set true by src/runtime/gpu_defaults.cpp). Regenerate with care: 0013 is diffed on top of 0012's texture_cache.
- Frozen builds: build/m8-frozen-final (M8), build/m9-frozen-final (ad2609b, M9 final code).
- Benchmark tags: m9f-cold8 / m9f-cold9 (own caches), m9f-m8 / m9f-m9 (final 5+5 at three sizes), q14-* (1440p
  three-way), m9sec-* (sections). Stability: build/stability/m9 (21 of 21).
- **M9 is at its gate: waiting for the player's review.** Not done: 120 fps at 4K, GPU idle between submissions,
  K75 (SDK validation messages).

## M11: Steam Deck compatibility (2026-11-01, branch `m10/updater`)
The player's M11 prompt: one portable AppImage and package for SteamOS and desktop Linux; build environments
allowed (no system changes); no publishing, push or merge without approval; manual Deck test by the player.
- Findings and design: docs/STEAM_DECK.md sections 1-2. v0.7.0 needed glibc 2.44 (build machine only); its setup had
  no Vulkan check (the Deck's Vulkan message is not from it: ask the player for its text). Real gap fixed: SDK
  patch 0013 now checks storage-image format support (staging fallback).
- Portable build: tools/toolchain/setup_sniper.sh (sniper 3.0.20260928.262393 sysroot + GCC 14.2.0 libstdc++ built
  in it, build/toolchains), preset linux-amd64-portable (cmake/steamrt3-sniper.cmake). Game: glibc 2.30, GLIBCXX
  3.4.32, bundled libstdc++ 3.4.33 in bin/compat (launcher picks it only when the system's is older). Setup: glibc
  2.25, libstdc++ static.
- Checks: package_linux.sh REQUIRES + baseline refusal; audit portability rules (19 audit tests); updater refuses
  updates the system cannot run (manifest `requires`, package REQUIRES; 48 updater tests); setup system check
  (Vulkan features, SteamOS/Deck, 20 setup tests); old-runtime test tools/toolchain/old_runtime_test.sh (PASS; the
  v0.7.0 package FAILS it as it should).
- Steam Deck defaults on first start only (render scale 1x, VSync on); platform logged ([kuf-platform]);
  launcher.log.
- Release candidate: 0.8.0-rc.1 (pre-release), notes docs/release-notes/v0.8.0-rc.1*.txt. The unpublished v0.7.1
  release folder is superseded for the Deck test (the player has not signed or approved it).
- Built from de55b82 (build/releases/v0.8.0-rc.1, unsigned): audit clean, AppImage selftest PASS (19), upgrade from
  a disposable v0.7.0 install with the 0.7.0 updater PASS, forced-Deck run 1280x800 PASS. Tests on the portable
  build: 1,824 non-game ctest tests PASS; game subset (build/runs/m11-ctest-game.txt) 22/27: smoke, capture,
  gameplay (new_game, controller_gameplay, combat_loot, k24), render (frames_complete, native 4K, 32:9, check),
  display 16:10 / native 16:10 / ref 16:9 and 9 menu routes PASS. The 5 red (menu.sdl_events, menu.sliders,
  menu.title_arrow, display.display_menu, display.display_title) fail the same on the desktop build: routes
  written before the M7 menu overhaul (K66), not M11.

## M12: advanced performance optimisation (2026-11-02, branch `m12/performance` from e59ffb1)
The player's M12 prompt: desktop optimisation (Deck testing postponed), sandboxed automated runs only, local commits
only (no push, merge, release, signing-key or installation changes). Results: docs/PERFORMANCE.md section 9.
- Kept: SDK patch 0014 (type-0 register ranges in bulk, dcf681e), SDK patch 0015 (timer queue blocking wait,
  4a4e615). Diagnostics fa4c351, 2306ef5. Tool: tools/merge_shader_storage.py (b812676).
- Rejected: narrower invalidation window, guest GPU-wait backoff (source kept only in the session scratchpad; it
  helps CPU-limited runs: re-measure on the Deck), GetRegisterInfo gating (no cost left).
- Frozen builds: build/m12-frozen-base (e59ffb1), build/m12-final (4a4e615). Warm cache for all M12 comparisons:
  build/warmcache/m12-union (rebuild with merge_shader_storage.py from build/runs/m8-m12-*).
- Final matrix tags m12-mb / m12-mf (4 sizes x 4+4), build/logs/m12-matrix.md. Verification: build/logs/m12-*.
- **M12 is at its gate: waiting for the player's review.** Next candidates: resolves into texture images (4K copies),
  command-processor translation, a loading-screen tick mode (needs approval: changes timing).

## M13: Vulkan GPU optimisation at 4K (2026-11-02/03, branch `m13/gpu` from d44c219)
The player's M13 prompt: GPU optimisation, sandboxed automated runs only, local commits only; physics (Jolt) is
postponed until the player asks. Results: docs/PERFORMANCE.md section 10.
- Kept: SDK patch 0016 (shared memory uploads hoisted to the submission start), 0017 (upload buffers in BAR
  VRAM on discrete GPUs). 4K: 104.8 -> 119.4 fps, p99 19.7 -> 16.3 ms, GPU 8.6 -> 7.2 ms per frame.
- Rejected: P1/P2 (pipeline placeholders, async depth-only pipelines): no effect on K76 hitches.
- Frozen builds: build/m13-base, build/m13-final. Verification logs: build/logs/m13-*.
- **M13 is at its gate: waiting for the player's review.** Next candidates: R1 redundant depth resolve, finer
  upload granularity, pipeline libraries for K76.

## M14: uploads, resolves and pipeline creation (2026-11-03/04, branch `m14/performance` from 0dcb3d6)
The player's M14 prompt: advanced Vulkan rendering, upload and pipeline optimisation; sandboxed automated runs only,
local commits only; physics (Jolt) stays postponed. Results: docs/PERFORMANCE.md section 11.
- Build isolation: every SDK target writes into its own build tree (8bc6f1b).
- Kept: U2 (unchanged pages not uploaded again, folded into SDK patch 0016), P3 (pipelines created without
  optimisation first, SDK patch 0018). GPU per frame 4K 7.0 -> 6.7 ms, 800p 3.1 -> 2.6; 4K 126.0 -> 131.4 fps; cold
  1440p 0.1 % low 18.3 -> 25.5 fps. RSS +130 MB.
- Rejected: R1 (redundant resolves, no measurable gain; patches/rejected/), pipeline libraries (from measurements).
- Frozen builds: build/m13-final (baseline), build/m14-exp, build/m14-exp2, build/m14-cand. Logs: build/logs/m14-*.
- **M14 is at its gate: waiting for the player's review.**

## Next exact action
**M14:** at the milestone gate (report delivered); wait for the player.
**M13:** at the milestone gate (report delivered); wait for the player.
**M12:** at the milestone gate (report delivered); wait for the player.
**M11:** 0.8.0-rc.1 built and reported; waiting for the player's Steam Deck test (and the text of the original
Vulkan message). Do not publish; Phase 12 (the next release) only after the Deck results.
**M9:** at the milestone gate (report delivered); wait for the player.
**M8:** at the milestone gate (report delivered); wait for the player. Before M8 (M7):
**Now:** the player tests the overhaul manually (checklist in the report); only then automated in-game tests.
Earlier:
Report the UX refinement to the player and wait: manual playtest of the six fixes (diagnostics: `--kuf_trace_menu_pointer`,
`--kuf_trace_title_events`, `--kuf_trace_prompts`; VSync: the `VulkanPresenter: Created ... presentation mode` log line),
then the comprehensive regression (not run yet).

## Required approvals (open)
- Publishing release packages (contains game-derived code) and choosing the GitHub repository.
- Merging M6D / this branch into main (never without approval).

## Tests performed / deferred
- Feature 8 (2026-10-30): `timing.k44_{60,120}{,_fault}` + checks pass with the aligned clock (after fixing a
  nanosecond-short last step counted late); `settings.frame_rate_menu`, `frame_rate_switch(_ignored)`, state and
  checks passed on the first version. Deferred: K44 long run, presentation/HUD checks at 60/90/120.
- Features 4-6 (2026-10-30): unit tests (input 40 cases, settings 30, ui 16); 18 game tests: menu.* (8, the five
  reference tests with the original title and icons), pc_settings_* routes (9), bindings_menu, bindings_game; all
  pass. Captures inspected: title rows, keyboard prompts, Key Bindings / Controller Buttons pages (labels shortened
  after the first run wrapped). Not tested: a real controller for remapping (virtual pad only), the updater rows
  against a real release (fake updater), Quit Game's two Selects with the mouse.
- Feature 3 (2026-10-30): the 66 settings, display-menu and pointer tests: 65 passed; `settings.frame_rate_check_cli`
  failed once in its window before any menu input (play_a -> play_b at 76.6 fps; the route change is after it), passed
  on rerun: recorded under K49/K47 as an unexplained early-gameplay slow window.
- Feature 2 (2026-10-30): unit (ui 15 cases, input 29), menu.pointer (new), the 18 existing scenarios that use the
  mouse (21 tests with setups, all pass). Not tested: the status/inventory menus with the pointer; real hardware mouse
  (scripted and SDL-event clicks only).
- Feature 1 (2026-10-30): `tools.updater_security` 18/18 (mutation-checked); a real package built from the build
  (76 MB). Not tested: on a Steam Deck; against the real GitHub API (no repository); install.sh's --signers path
  with a real key.
- Deferred to feature freeze: the complete regression suite (`ctest -j4`, fixture-group batches, docs/TESTING.md
  "The full suite within memory").

## Known bugs and blockers (beyond KNOWN_ISSUES.md)
- The launcher test once opened a zenity folder dialog on the development desktop (2026-10-30, killed by the
  timeout; the test then had the desktop's DISPLAY). Fixed: dialogs need a display and no KUF_NO_DIALOG; the test
  and its ctest entry run without display variables.
