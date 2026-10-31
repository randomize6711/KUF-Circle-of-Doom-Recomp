# High frame rates (M6D): timing investigation and implementation plan

Status:
- **D5 validated (2026-10-30):** the full regression within memory (section 22): two memory faults in the test
  harness fixed, three faults found by the regression fixed (an effect-check race, a part edge case, a trace
  label), the 90 fps result not reproduced, the title screen's limit traced to the movie player's wait. D6 not started.
- **D5 done (2026-10-29):** the frame rate (30 original to Unlimited), interpolation and its advanced parts on two new
  pages of the game's PC Settings menu, saved in pc_settings.toml (default 30), switched while the game runs, the
  command line winning per flag for the run (section 21). Not merged; awaiting approval; D6 (optimisation) not
  started.
- **D4b-4 done (2026-10-28):** the HUD checked against the world (screen-space, unchanged), the effect shaders drawn at
  each frame's moment, GPU particles and trails left at 30 Hz with the reason (section 20); K24's loot fixture and the
  K46 regression test (14.8). D4a, D4b, D4b-3 and K44 are in sections 16-19. Not merged; awaiting approval; D5
  (settings) and D6 (optimisation) not started.
- **D3b-3 done (2026-10-26):** the camera and the player character interpolated between ticks in render-only frames
  (presentation only; the game's state is unchanged), with explicit discontinuity rules and threshold backstops
  (section 15). Awaiting approval; D4 (world), D5 (settings) and D6 (optimisation) not started.
- **D3b-1 done (2026-10-25):** render-only frames between ticks, complete and isolated (`--kuf_frame_rate`, off by
  default); K38 corrected (section 14).
- **D3b-0 done (2026-10-25):** K34 fixed in the SDK (patch 0010); the K35 render-test correction; the D3b-1
  investigation and implementation proposal (section 13). Awaiting approval of the D3b-1 design.
- **D3a done (2026-10-25):** research only. The exact cause of the black frames, the render data lifetimes, the
  architecture comparison and the recommended design are in section 12. Awaiting approval before D3b.
- **D1 and D2 done** (branch `m6d/high-fps`; not merged). Final D2 regression: 1,776 of 1,776 (10.4).
- **D1:** the renderer and timing were measured (section 9).
- **D2:** the game's 30 Hz tick runs on a host clock and is the default (section 10).
- **Drawing more than 30 frames per second** works since D3b-1 (section 14); since D3b-3 the camera and the player
  move smoothly in those frames, since D4a/D4b the world's objects, skeletal poses and hanging parts, and since D4b-4
  the effect shaders' motion; GPU particles and trails still 30 times per second (K39, section 20).
- **Revised D3 plan:** section 11, refined by section 12.

Sections 0-8 are the investigation of 2026-10-24 (commit 8b479fa), kept as written. Where D1 changed the picture,
sections 9-11 say so.

Confidence marks: **[V]** verified (read the instructions or the code, or measured), **[L]** likely (inferred),
**[?]** unknown. Guest addresses refer to the v1 XEX (`build/analysis/v1/flat.dis`); SDK paths are under
`thirdparty/rexglue-sdk`.

Sources: two read-only code investigations on 2026-10-24 (guest disassembly; host/SDK code), spot-checked again
against the disassembly while writing this document, and the four probe runs `build/runs/m6d-*-tick_rate`
(section 3).

## 0. Summary

**Update after D3a (section 12):**
- **Why repeated frames are black:** the game step fills the frame's object list and the HUD's lists, which the
  frame end erases, and the SDK's default render-target path resolves black when the HUD's draws are missing (K34).
- **What works:** on the EDRAM-exact path, frames rebuilt or repeated from one tick show the correct scene.
- **Recommended:** rebuild render slots from the tick's retained outputs, with presentation-only camera data.
- **Found on the way:** effect clocks run per drawn frame, and the scene build uses the game thread's random
  numbers.

**Update after D1/D2:**
- The fixed 30 Hz tick is in: the game speed is 1.00x at any guest vblank rate and with vsync off, where the
  original ran up to 6.8 times as fast (10.2).
- Drawing a tick more than once is not possible without deep changes (9.2), so D3 starts with a research step
  (section 11).

The original summary follows.


- The game simulation is **frame-locked**. Each rendered frame is one game tick of a nominal 1/30 s, and no
  gameplay code measures elapsed time [V]. The 30 fps cap is a single D3D setting: the game presents on
  every second vblank [V].
- Raising the guest refresh rate or turning vsync off **speeds up the whole game** [V, measured]. The probes ran
  at 1.00x, 1.99x, 3.91x and 5.88x real-time speed, and the character walked exactly the same distance per frame
  in all four.
- Raising the simulation rate is not a safe option. Ticks are built into PhysX, Lua waits, script timers,
  ability tables, animation, AI and more than 100 code sites that use the constants 30.0 and 1/30.
- **Recommended:** keep the game tick at exactly 30 Hz on a host clock. Let the render thread present more
  often, drawing positions interpolated between the last two ticks: first the camera and the player, then
  the rest of the world. The game's own two-thread design, with a game thread and a render thread linked by
  two semaphores, provides the natural place to do this.
- The original 30 fps mode stays the default, and its behaviour stays exactly as it is now. High frame rates
  are a setting on the native PC Settings page.

## 1. Guest timing architecture

### 1.1 Threads and the frame loop [V]
`sub_826F10E8` creates two guest threads. The sync object at **0x82AC6658** (`sub_826F2D40(obj, 2)`) holds two
semaphores: "free" at +0 (initial count 2, maximum 2) and "filled" at +4 (initial 0, maximum 2).

```
game thread sub_826F0FB8                     render thread sub_826EB458
loop until byte 0x82AA9120:                  loop until byte 0x82AA9121:
  sub_826F2DB8  wait "free"                    sub_826F2EA0  wait "filled"
  sub_826F0D58  ONE game tick (world)          sub_826E9FC8  render + present one frame
  sub_826F2E10  release "filled"               sub_826F2E40  release "free"
```
- The game advances exactly one tick per rendered frame and can run at most 2 ticks ahead of the render thread.
- The loop has no sleep, timer or time accumulator. The render rate, set by the present interval and the
  vblank, paces everything.
- Checked in the disassembly: the calls at 0x826F1038/0x826F1040/0x826F1048 and 0x826EB49C/0x826EB4A4/0x826EB4AC.
  The `lis -32084` plus `+26200`/`+26232` operands give 0x82AC6658 and 0x82AC6678.

### 1.2 One game tick, `sub_826F0D58(world)` [V order, L names]
1. Message and event dispatch, and game-state transitions (`sub_826F0B70`).
2. Subsystem updates: `sub_826BAC68`, `sub_826B6E18`, `sub_826EA090`, `sub_826ECD78`, `sub_821C1600`,
   `sub_825D4F98`, `sub_826BFF80`, `sub_82619160` and `sub_826B20D8`.
3. **Global tick counter** `++*(0x82ABB740)`.
4. Either the paused/menu branch, or the world simulation: `(*(0x82ABA7D8)+672)->vf20/vf24` and state->vf28.
5. Scene build: `sub_821F49E8`, then `sub_821F4D00`, which also runs the physics step. Then `sub_821F4CD8` flips
   the double-buffered per-frame view slot (index 0x82AE760C) that the render side reads.
   - The render side keeps its own index at 0x82AE7610, toggled by `sub_821A3C88` once per rendered frame
     (docs/CAMERA.md).
   - This is why the render side draws one game frame behind.

### 1.3 One render frame, `sub_826E9FC8(world)` [V]
The render frame runs these steps in order:
- state->vf36, clear, `sub_821F5608` and state->vf40;
- present through `sub_821A3C98` (`sub_821E2CA8` → `sub_8216E1B0` → `VdSwap`);
- a GPU throttle that spins while the GPU is more than 2 frames behind.

Before and after the present, a millisecond timestamp (`sub_821EDB00`, which assumes a 50 MHz timebase) is
written to world+448. Nothing reads it apart from the constructor [L: diagnostic only].

### 1.4 How the 30 fps cap is requested [V]
- `sub_826EF200` calls the renderer constructor `sub_821A2690` with `li r4,1` at **0x826EF2E4**.
- `sub_821E21B8` writes PresentationInterval at this+72 (present parameters +52):
  - **2** when that argument is 1 (`stw r23,72` at 0x821E2280);
  - **1** when it is 0 (0x821E2270).
- The D3D vblank callback `sub_8216DAE8` counts vblanks (refresh rate defaults to 60) and releases a swap once
  `vblank >= lastSwap + interval`.
- The result is 30 fps on a 60 Hz signal. The renderer already has a 60 Hz code path behind the argument, but
  selecting it only doubles the game's speed.

### 1.5 Time sources [V]
- **QueryPerformanceCounter:** an inline `mftb` in `sub_826F3BD0`; the game does not import
  KeQueryPerformanceCounter. In the game's own code it is used only by PhysX profiling, an audio timer class
  (`sub_826E2898`), the D3D library and the unused timestamp above.
- **QueryPerformanceFrequency** (`sub_826F3BF0`): D3D, the PhysX profiler, audio, and `sub_825E6C60`
  ([?] probably network or session code).
- **KeQuerySystemTime:** one wall-clock wrapper (`sub_826F6978`) [L: dates in saves].
- **No measured delta time anywhere in gameplay.** All 16 raw `mftb` sites are accounted for, and none of them
  feeds a simulation step [V for the call graph; L for "none elsewhere"].
- **Per-tick constants** (value checked in `image.bin`):

  | Constant | Address | References | Purpose |
  |---|---|---|---|
  | 1/30 = 0.0333333 | 0x8211BCCC | 34 in 29 functions | ticks to seconds |
  | 30.0 | 0x8211B818, plus copies | 65 in 60 functions | seconds to ticks |
  | 1/60 | 0x8211B8F0 | 7 | PhysX defaults and a few effects |
  | 60.0 | 0x8211BB88 | 17 | UI and effects |

### 1.6 Per-system timing
| System | Mechanism | Conf. |
|---|---|---|
| Game tick | One tick per rendered frame | V |
| Lua coroutines | Clock += 1.0 per tick. `Wait(n)` waits n ticks; `WaitSec(s)` waits s×30 ticks (`sub_826D7E80`, `sub_826D7F38`, `sub_826D8230`) | V |
| Script timers (`sys.MarkTime`, `GetElapsedTick`, `GetElapsedSec`) | 20 counters at script manager +4124, +1 per tick. Seconds = ticks × 1/30 | V |
| PhysX | `simulate(1/30)` once per tick (`sub_82230E00`). `setTiming(1/120, 4, FIXED)`, i.e. four fixed substeps (`sub_82230FE8`). Rate field physics manager +60 = 30.0 | V |
| Player SP regeneration | Each tick adds a rate × 1/30 (`sub_824678F8`) | V |
| Player timers | Integer counters that count down or up by 1 per update | V (units: ticks) |
| Ability costs and cooldowns | Table columns `SPperTick`, `Cool`, `tickPerParm` (`sub_826C3F08`) | L (ticks) |
| Game camera | Per-frame smoothing (1/(n+1)), 25-30 % position and height smoothing, turn rates in rad per frame (CAMERA.md) | V |
| UI blinking and phases | `((n-1) % 30) × 1/30` from a frame count (`sub_825D71F0`) | V |
| Animation, IK and aim | Per-frame rates; `sub_8255F908` uses 30.0; `SET_ANI_SPEED` script parameters | L per tick |
| AI, combat and spawns | No time source exists that could scale them; their data is authored in ticks | L per tick |
| Particles and effects | CPU side per tick (1/30 and 1/15 constants). GPU particles get `Time`/`deltaTime` shader constants whose source was not traced | L / ? |
| Audio (XACT, 3D) | Its own QPC timer; playback runs in real time | L |
| WMV movies | Movie player (`sub_826E90C0` play, `sub_826E91F0` stop). Pacing not traced | ? |

## 2. Host and SDK timing architecture

### 2.1 Vblank and presentation [V]
- **"GPU VSync" thread** (`src/graphics/graphics_system.cpp:149-176`):
  - each interval of 50 MHz / refresh it calls `MarkVblank()`, which bumps the CP counter and runs the game's
    interrupt callback on that thread;
  - it then sleeps 1 ms, and fires any missed vblanks back to back.
- **Guest refresh rate:** the cvar `video_mode_refresh_rate` (default 60, clamped to 24-240,
  `xboxkrnl_video.cpp:77`). Our code never sets it.
- **`--vsync`** (default true; `command_processor.cpp:42`): false gives **1000 vblanks/s** and makes the
  `WAIT_REG_MEM` loop yield instead of sleeping. It does not change the host present mode.
- **No other frame limiter** exists in the SDK or in `src/`.
- **Swap path:**
  - `VdSwap` writes `PM4_XE_SWAP`;
  - `VulkanCommandProcessor::IssueSwap` hands the frame to `Presenter::RefreshGuestOutput`, which keeps a
    3-slot mailbox where the newest frame wins;
  - by default the frame is presented immediately from the CP thread.
- **Host present mode:** IMMEDIATE > MAILBOX > FIFO_RELAXED > FIFO (`vulkan_presenter.cpp:1384-1402`). Host
  presents are therefore 1:1 with guest swaps and are not tied to the monitor's refresh rate (VRR-friendly).
- **The presenter can re-present the last image** (UI-thread repaint), but it only duplicates frames.

### 2.2 Guest clocks [V]
- The guest timebase is 50 MHz with a scale of 1.0. `mftb` maps to `Clock::QueryGuestTickCount()`.
- `KeQueryPerformanceFrequency`, `KeQuerySystemTime`, delays (1 ms granularity), timers and waits all go
  through the SDK clock.
- **`Clock::set_guest_time_scalar` exists**, but no cvar exposes it and our code does not call it. It would
  speed up or slow down the whole guest, including the vblank thread. It does not decouple rendering from
  the simulation.

### 2.3 GPU command processor [V/L]
- **Pacing fence:** one `WAIT_REG_MEM` per frame on a value the game writes.
  - At 0x100 or above it sleeps wait/0x100 ms (vsync on) or yields (vsync off).
  - [L] the fence is advanced by the game's vblank callback.
- **Limits at higher rates:**
  - 1 ms sleep granularity;
  - a single CP thread, which reaches 91-94 % of a core when uncapped (PERFORMANCE.md sections 4 and 6.2);
  - present calls run on the CP thread.

### 2.4 Audio [V]
- The SDL audio callback releases the guest's XAudio render callback once for each consumed frame
  (256 samples at 48 kHz), so mixing is clocked by the sound device and does not depend on the frame rate.
- `--kuf_audio_pace` does the same for the sandbox's null sink (test only). Its `SDLAudioP` thread spins at one
  core in sandbox runs (PERFORMANCE.md).

### 2.5 Input [V]
- The game polls `XamInputGetState` once per tick (`sub_825EAD58`).
- `KeyboardPad` has per-poll semantics:
  - taps are latched until the next poll;
  - the mouse wheel sends one pulse and then one gap poll;
  - in right-stick mode, mouse look becomes one poll's stick deflection.
- The modern camera's direct mouse path (`TakeLook`) uses raw pixels and does not depend on the poll rate.
- Input scripts and captures are anchored to **presented guest frames** (`OnFramePresented`, the `VdSwap` hook).

### 2.6 Our runtime code
- **`modern_camera.cpp` already uses real time.** A `steady_clock` dt drives player turns
  (stick × rad/s × dt), the distance ease, recentring and `LimitUpward`. (Since K43, section 17.1, with the fixed
  tick that dt is one tick per update: the host clock made runs differ.)
- **Some parts mirror the game's per-frame behaviour:** eye/target smoothing at 1/n per frame, height-smoothing
  undo, `kQuietFrames` = 10. These must keep running once per game tick.
- **Telemetry, frame captures and the profiler count guest presents.**

## 3. The four timing probes (2026-10-24, measured)

### 3.1 Setup [V]
- **Build:** `m6d/high-fps` at d3734d2, with no code changes; RelWithDebInfo tree; sandbox.
- **Display:** offscreen 1280x720, rendered at native resolution (1x); RX 9070 XT. Warm shader cache after the
  first run.
- **Game:** save `build/m6-saves/celine-shadows-depths-1`. Continue, then gameplay in Shadow Depths with no fight.
- **Script:** anchored to presented frames:
  - hold W (walk) for 180 frames;
  - hold L (camera turn right) for 25 frames;
  - stay idle;
  - capture at gameplay+300/390/480/600/900/1500.
- **Variants:**
  - `--video_mode_refresh_rate` 60 / 120 / 240 (`m6d-hz60`, `m6d-hz120`, `m6d-hz240`);
  - `--vsync=false` (`m6d-novsync`);
  - all with `--kuf_profile=true`.
- **Results:** all four exited with 0, applied 16/16 script items and passed every check. The scenario is in
  appendix A.

### 3.2 Rates and speed (gameplay window = the 1,200 frames between the first and last capture) [V]
| Probe | Vblanks/s | Presented fps | Frame median / p99 (ms) | Wall time for 1,200 frames | Game speed vs real time | Title to gameplay |
|---|---|---|---|---|---|---|
| hz60 (as shipped) | 60 | 29.97 | 33.28 / 37.09 | 40.04 s | **1.00x** | 63.6 s |
| hz120 | 120 | 59.54 | 16.77 / 19.81 | 20.15 s | **1.99x** | 47.2 s |
| hz240 | 240 | 117.05 | 8.28 / 11.22 | 10.25 s | **3.91x** | 39.5 s |
| vsync off | 1000 | 176.28 | 5.24 / 8.72 | 6.81 s | **5.88x** | 34.1 s |

- **Ticks = presented frames.** Fps is exactly half the vblank rate (present interval 2) until the hardware runs
  out of capacity: the uncapped run reaches 176 fps, not 500. Game speed is fps / 30 because the scripted
  actions are anchored to frames, and the game advanced the same amount per frame in every run (3.3).
- **The guest clock was not scaled.** The measured guest timebase was 49.99-50.00 MHz in every run. The speed-up
  comes only from more ticks, not from a faster clock.
- **Menus and loading also ran faster.** The time from title to gameplay fell from 63.6 s to 34.1 s, because
  menu waits and loading sequences count ticks too.
- **Capacity in this scene** (profile_report over the window, % of one core):

  | Probe | Guest threads | GPU command thread | GPU busy |
  |---|---|---|---|
  | hz60 | 112 % | 18 % | 38 % |
  | hz240 | 140 % | 61 % | 83 % |
  | vsync off | 167 % | **93 %** | 86 % |

  - Uncapped, the single GPU command thread is the limit, as in M6A/M6C.
  - PERFORMANCE.md 6.2 reached 306 fps uncapped at 1x on a different workload, the exploration loop.
  - `SDLAudioP` at about 100 % is the sandbox's silent-audio thread; there is no such cost with a real sound
    device.

### 3.3 What changed in the picture [V measured, L where noted]
Mean absolute RGB difference per pixel (0-255) between captures at the same frame, compared with hz60. For
scale: the walk changes the picture by about 12.7 between two captures 90 frames apart in the same run.

| Capture | hz120 | hz240 | vsync off | Meaning |
|---|---|---|---|---|
| walk_90 | 1.4 | 3.6 | 2.5 | |
| walk_180 (end of the walk) | **0.6** | **1.3** | **1.4** | Same position after 180 frames: **movement is per tick** |
| turned / idle | 8.1-9.1 | 8.2-8.4 | 7.9-8.1 | Different camera heading |

- **Movement, the character's walk animation and the game's camera trailing are per tick** [V]. The same number
  of frames gave the same picture at 2x, 3.9x and 5.9x real time.
- **Our camera turn is per second** [V measured; the cause is L, consistent with 2.6]. The L key turns the camera
  through `modern_camera.cpp` at stick × rad/s × real dt. Holding it for 25 frames lasted 0.83 s at hz60 but
  only 0.14 s with vsync off, so the camera turned through a different angle. The game and our camera
  therefore already run on two different clocks: they agree only while the game runs at 30 ticks/s. Any
  high-fps design must keep the game at 30 ticks per real second.

### 3.4 Measured vs inferred
- **Measured:**
  - presented fps;
  - game ticks per real second (speed-up = fps / 30) for movement, walk animation and camera trailing;
  - menu and loading speed-up;
  - the guest clock rate;
  - CPU/GPU load;
  - the real-time camera turn.
- **Inferred from the code, not exercised by the probes:**
  - combat, AI, ability cooldowns and SP regeneration;
  - PhysX, Lua `Wait`/`WaitSec` cutscene timing;
  - particles, audio and voice sync (the sandbox has no sound device);
  - movie playback.
  
  All of these count ticks or fixed 1/30 steps (section 1.6). At any present rate above 30 the code predicts
  the same speed-up for them. That needs no further proof for the decision: the recommended design never runs
  the game above 30 ticks/s.

## 4. Recommended strategy

### 4.1 Options considered
| Option | Verdict |
|---|---|
| Raise the vblank rate or turn vsync off | **Rejected:** the whole game speeds up (measured) |
| Run the simulation at 60+ Hz by rescaling every per-tick quantity | **Rejected:** 100+ constants, tick-authored ability tables, Lua scripts and data files, PhysX substep ratios, per-frame camera and animation smoothing. It cannot be validated against the original and would risk combat balance and scripted events |
| Variable delta time | **Rejected:** no dt path exists to build on |
| Host-side frame generation (image interpolation in the presenter) | **Not now:** the game provides no motion vectors. It would cause HUD and edge artifacts and add a frame of latency. Possible later as an optional extra |
| **Fixed 30 Hz simulation, independently scheduled rendering, interpolated transforms** | **Recommended:** gameplay stays bit-for-bit original, and the engine's own split between the game and render threads is the seam |
| Camera only at a higher rate (game world at 30 Hz) | Useful as the **first visible stage**. On its own the camera would follow a player who moves only at 30 Hz and judders, so the player's transform must be interpolated in the same stage |

### 4.2 Design
1. **Host-owned simulation clock.** Hook the game thread's wait (`sub_826F2DB8`) so that it also waits for a
   host 30 Hz tick, using an absolute schedule with catch-up limits. The game then runs exactly 30 ticks per
   real second whatever the render rate. Input polling, Lua, PhysX, AI, saves and audio are untouched.
2. **Render thread no longer bound one-to-one to ticks.**
   - The render thread waits for its pacing (the frame limit or the present) instead of only for "filled".
   - When no new tick is ready, it renders the latest completed tick again.
   - The guest present interval becomes 1 (the game's existing code path), with
     `video_mode_refresh_rate` = target, or vsync off plus a host frame limiter for targets the guest clamp
     cannot express (above 240, or unlimited).
3. **Interpolation at render time.**
   - Interpolate the inputs to the render frame between ticks N-1 and N with α = time since tick N / 33.3 ms:
     camera eye, target and FOV, then object transforms.
   - The **approved M6B camera keeps running once per tick with unchanged math.** Only its outputs are
     blended, so at every tick boundary the view is identical to today's.
   - Discontinuities (camera cuts, teleports, level loads, menu/pause transitions) snap instead of blending.
4. **Option in the native PC Settings page** (`[framerate]` in `pc_settings.toml`):
   - Frame rate: **30 (original)** / 60 / 120 / 144 / 240 / Unlimited;
   - VSync;
   - Interpolation (on by default above 30).

   The default is 30 (original), which keeps the current behaviour and all 1,768 tests unchanged.

### 4.3 Open questions for phase 1 [?] (answered by D1, section 9)
- Is a render frame (`sub_826E9FC8`) a pure consumer of tick data? Its state->vf36/vf40 calls, the UI,
  the movie player and the D3D throttle may advance state.
- Where are the transforms in the data the render thread consumes?
  - Are object, bone and particle matrices baked into a render list during the scene build (game thread,
    1.2 step 5)? Then interpolation would need to patch that list per render.
  - Or does the render side read them from objects? Then it would need to read blended copies.
- The render-side view index (0x82AE7610) toggles on every rendered frame. Rendering a tick twice must not
  toggle it, or the second frame reads the wrong slot.
- The game can run 2 ticks ahead, but the view data is double-buffered. How many ticks of render data are
  available at once? Interpolation needs ticks N-1 and N together.
- Movie pacing, and the GPU particles' `Time`/`deltaTime` constants (are they per tick or per render?).

## 5. Proposed implementation phases
Each phase ends with tests and a report. The default (30, original) must stay bit-identical throughout.

| Phase | Content | Exit criteria |
|---|---|---|
| **D1: Render seam research** (diagnostics only, opt-in cvars) | Answer 4.3: a write trace on render frames, the render-data layout, the location of player/camera/object/bone transforms, whether a tick can be rendered twice, movie and particle timing. Inventory of what can be interpolated, by category | A documented map with addresses; go/no-go for D2 |
| **D2: Fixed-step pacing** | Host 30 Hz tick clock; render thread repeats the last tick; present interval 1; frame limiter 30-240/unlimited; tick-anchored test scripts | Tick rate 30.00 ±1 % at every target; at 120 the simulation state equals the 30 fps run at the same tick (captures and saves); present rate meets the target or the measured capacity |
| **D3: Camera and player interpolation** | Blend the camera outputs and the player character (with attachments) between ticks; snap rules | M6B camera tests unchanged at 30; at 120 the camera trace at tick boundaries equals the 30 run; smooth camera motion; the player character does not judder against the camera |
| **D4: World interpolation** | Enemies, NPCs, skinned bones, props, physics bodies, projectiles; CPU particles where feasible; anything else stays at 30 Hz | Per-category tests; no frame shows an object going backwards; teleports snap |
| **D5: Native PC Settings** | Frame rate, VSync and Interpolation rows; persistence; next-start/apply rules as in M6C | Menu and persistence tests |
| **D6: Performance and verification** | Command-processor and present cost at 144/240; performance tables at 1x/1080p/1440p/4K/ultrawide; full regression; build for the player's manual test | All tests pass; manual approval |

Suggested first approval: **D1 + D2** together. D2 has no visible benefit on its own (repeated frames), but it
is the foundation for everything after it and it proves that gameplay speed is correct.

As implemented (section 10), D2 keeps one draw per tick instead of repeating ticks (D1 found that a tick cannot be
drawn twice), so there is no frame limiter yet. The tests check the game's speed at different guest vblank rates
instead of frame-rate targets.

## 6. Risks and mitigations
| Risk | Mitigation |
|---|---|
| A render frame changes simulation state, so rendering a tick twice alters gameplay | D1 write trace; hook only what is proven pure; a D2 equivalence test (same tick, same state) |
| Transforms baked into render lists on the game thread | Capture the tick N-1 and N matrices at upload, keyed by object, and blend per render. Fall back to camera + player only (D3) if the world proves impractical |
| Render-side double-buffer index (0x82AE7610) and the 2-tick lead | Hook the toggle; hold two completed ticks; add 1 tick of display latency only when interpolating (measured, documented) |
| Interpolation latency (up to 33 ms for world objects) | Use the newest tick for the camera's own input-driven rotation if the player's test asks for it. Keep it optional, because the M6B feel is approved as is |
| Frame pacing (1 ms sleeps, vblank thread jitter) | High-resolution host limiter; pacing statistics in the tests (p99 frame time) |
| The single-threaded GPU command processor limits uncapped and 240 Hz | Measure in D6. Expected 240 at 1x, not at 4K on this GPU (PERFORMANCE.md 6.2: 99 fps at 4K uncapped) |
| Tests anchored to presented frames | Tick anchors for high-fps runs; the existing suite stays at 30 and unchanged |
| Movies, cutscenes and audio sync | Cutscenes stay at 30 ticks/s, so audio sync is unchanged; movie pacing checked in D1/D2 |
| Our runtime's per-frame hooks (camera smoothing mirror, captures, telemetry) | Keep them per tick; only render-time blending is new |
| Saves | Untouched: the simulation is unchanged. The save chain also runs at a high frame rate |

## 7. Proposed validation
**Automated:**
- `tick_rate` scenarios at 30/60/120/144/240/unlimited:
  - game ticks per real second (tick counter 0x82ABB740 against wall time) = 30.0 ±1 %;
  - presented fps;
  - frame-time p99.
- **Equivalence:** the same scripted route at 30 and 120 with interpolation off gives identical tick-anchored
  captures and identical save files.
- **Gameplay timing at 120 and unlimited:**
  - walk distance per second;
  - a Lua `WaitSec` cutscene in wall time;
  - `combat_loot` and the save chain;
  - menu and loading durations equal to 30 fps.
- **Camera:** the existing M6B tests at 30; the camera trace at 120 equals the 30 trace at tick boundaries; FOV
  and distance unchanged.
- **Interpolation:** no motion backwards; snaps at cuts and teleports; HUD unaffected.
- **M6C:** display tests at 120 (native resolution, ultrawide, colour comparison).
- **Settings:** menu rows and persistence.
- **Full regression** before the report.

**Manual (the player's session):** 60/120/144 and unlimited on the real display, VRR on and off, exploration,
combat, a cutscene, the intro movies, menus, saving and loading, and camera feel compared with 30.

## 8. Expected limitations
- Game logic and input stay at 30 Hz, so the response time of actions (attack, dodge) is the original's.
  Only what is drawn becomes smoother.
- Anything not interpolated (some effects, GPU particles, 2D UI animations) still moves at 30 Hz.
- Interpolating world objects adds up to one tick of display latency.
- Uncapped and 240 fps are limited by the single GPU command thread and by the GPU at high resolutions.
  Measured uncapped on this machine: 176-306 fps at 1x, 99 fps at 4K.
- 144 Hz does not divide 30 Hz evenly. Interpolation handles this; without interpolation, frames repeat unevenly.

## 9. D1 results: the renderer and timing, measured (2026-10-24)

Approved together with D2. Tools (opt-in, `src/runtime/frame_pacing.cpp`; none of them changes the default game):
- `--kuf_render_repeat=<fps>`: the render thread draws the latest tick again up to that rate (the deferral
  described in 9.2 below; not for play).
- The input script's `freeze <seconds> <label>` (needs `--kuf_render_repeat`):
  1. holds the game thread when the next tick is taken;
  2. snapshots all readable guest memory (3.2 GB: virtual 0x00000000-0x9FFFFFFF and the 512 MB of physical memory)
     before the tick's first frame, after it, and after the repeated frames;
  3. then repeats the measurement for a control period of the same length with nothing drawn;
  4. writes `freeze-<label>.txt`: changed bytes per region, the changed virtual words, and the vectors the first frame
     emptied (an end pointer reset to its begin), each named after its owning object (nearest vtable or renderer
     field);
  5. takes three labelled captures during the repeats.
- `--kuf_trace_render_lists=<tick>`: the sizes of both slots' draw lists and of each view's per-slot list, before and
  after 96 frames from that tick.
- Log lines name the game and render threads (`[kuf-pacing] game thread` / `render thread`), so the thread tag of
  any other log line (camera traces, input contexts, movies) shows where it runs.

Runs: `build/runs/d1*-120-*` (probe route at `--kuf_render_repeat=120`; freezes at the title, standing, walking and
in the status menu).

### 9.1 Answers
| # | Question | Answer | Evidence |
|---|---|---|---|
| 1 | Can the renderer draw several frames without advancing the game? | **No, not as built.** A frame consumes the tick it draws. In the repeat mode all 14 gameplay and status-menu captures were black (mean 0.0), with every consumer found so far deferred. The same route drawing each tick once has normal captures | 9.2 |
| 2 | Do rendering operations modify gameplay state? | Rendering writes about 20 KB of heap and 0.5 MB of physical memory for a tick's first frame (render lists, frame-memory pools, D3D state, command data). Among them is a list inside a PhysX callback object (vtable 0x8210376C, "Phys_Callback", +88) that the first frame empties. With the game held and nothing drawn, gameplay memory is still (43 bytes in 2 s). So **all simulation runs in the tick on the game thread**, and the renderer touches physics-owned data only to consume it | freeze-idle (control 43 bytes; first frame 20,041 heap bytes) |
| 3 | Do animations or effects advance during rendering? | Not found in gameplay: the first frame computes per-tick data (e.g. 16 KB of float positions at 0x400B6000-0x400B9FFF, written by the first frame only, never by the repeats), which looks like lazily built, cached per-tick data. The title movie advances in **real time**, independent of ticks and frames: with the game held, 3.0 MB of heap changed in the control period without any drawing, and the three repeated title captures differ | freeze-title (control 3,044,497 bytes), captures title_1-3 |
| 4 | Does the renderer depend on a frame counter that advances with the simulation? | Yes: the two slot indices. The game flips its slot (0x82AE760C) at the end of each tick, and the render flips its own (0x82AE7610) at the end of each frame. They stay in step only while every tick is drawn exactly once | 1.2, 9.2 |
| 5 | Can command buffers, render lists or GPU resources be reused? | Render lists: no, they are emptied by the draw (9.2). Frame-memory pools are freed and rebuilt per frame. GPU resources: deletions are deferred per slot (renderer + 29444 + slot * 16) and collected when the GPU is done (sub_821C8710), so a kept slot keeps its resources alive. Re-running the frame-end step is safe only once per slot | 9.2 |
| 6 | Can game-generated render commands be replayed? | **Not attempted, and not useful for D3.** A PM4 replay would have to filter the stream's fence writes and interrupts and would repeat baked transforms, so it could not show interpolated positions | analysis |
| 7 | Can the M6B camera stay unchanged at the simulation level? | Yes: the camera runs inside the tick on the game thread (9.3). Its own turn rate uses real elapsed time, which is 1/30 s per tick with the fixed tick, as before (since K43 exactly one tick per update, 17.1) | 9.3 |
| 8 | Do audio, menus, cutscenes and loading have special timing dependencies? | Audio is clocked by the sound device (2.4). Menus, script waits and cutscene camera paths count ticks (1.6). The movie player (title loop, attract movie) decodes against real time (Q3). While loading, the **game thread presents frames itself** (`[kuf-pacing] a frame was presented outside the render thread's loop`, logged from the game thread at start-up), so loading screens are drawn outside the two-thread loop | logs, 9.3 |

### 9.2 What a frame consumes
Found with the repeat mode, the list trace and the freeze reports, in the order they were found:
1. **The end of the frame** (`sub_821A3BA0`, after drawing):
   - the device's end-of-frame step (`sub_821E3F48`);
   - **clears the slot's draw list** (`sub_8220FB00` = vector erase, renderer[(slot + 6782) * 4]);
   - runs the slot's **deferred resource commands** (`sub_821AA7A0`: 28-byte records of 18 kinds at renderer + 29444 +
     slot * 16; mostly deletions through destructors);
   - **frees and rebuilds the slot's three frame-memory pools** (`sub_821B07E0` at 0x82AE81FC + slot * 48,
     `sub_821B03D8` and `sub_821B0648` at renderer + slot * 48 + 8 / + 29308);
   - clears the slot's byte at renderer + 29656 + slot;
   - **releases the objects the slot kept alive** (`sub_821F0BB8`, renderer + 420);
   - flips 0x82AE7610 and collects released GPU resources (`sub_821C8710`).

   The repeat mode runs only the device step and the collection per frame, and the rest once when it moves on. That
   fixed the empty slot list (`--kuf_trace_render_lists`: 45 entries kept across repeats), but frames stayed black.
2. **The draw itself** (`sub_821F5608`) clears each view's three per-slot lists (`sub_821EA3A0`: view + 1124 /
   1156 / 1188 + slot * 16; views are the renderer objects whose indices are listed at renderer + 26548). The trace
   showed the main view's list going from 3 entries to 0 in the first frame. Deferring that too kept the lists, but
   frames stayed black.
3. **More lists** emptied by the first frame (freeze reports, "vectors emptied"):
   - the game's render command queue for the slot (renderer + 29412 + slot * 16, filled by the tick through
     `sub_821AD2B0`, run by `sub_821A6FA0` / `sub_821A6EA0` at the start of the draw);
   - lists inside objects with vtables 0x8210109C (+152), 0x8210376C ("Phys_Callback", +88) and 0x820306B0 (+100);
   - a 412-byte list in a renderer object;
   - in one run, a 22 KB list near 0x400E4C14.

   These are reached through virtual calls (each view object's `vf124` draws it), not through direct calls, so a
   static search does not find them.

Conclusion: the renderer is built to draw every tick exactly once, and the consumption is spread over at least
seven places, some in physics-owned objects. Making a tick drawable twice means finding and deferring all of
them. That is reverse engineering for D3, not a safe D2 mechanism. Following the D2 instruction ("stop and explain
the blocker rather than implementing an unsafe workaround"), **D2 keeps one draw per tick** (section 10), and the
repeat mode stays a diagnostic.

### 9.3 Threads
Log thread tags (run `d1-threads-short`, `--kuf_trace_modern_camera`, `--kuf_trace_projection`):
- **Game thread** (t37, "[kuf-pacing] game thread"):
  - all 427 modern-camera view updates;
  - the game's own projection builds;
  - the input contexts (the game's input poll);
  - the movie start and stop;
  - 28 of 29 file opens.
- **Render thread** (t38): only the render side's projections (`sub_821E9ED0`, 144 lines).

So the approved M6B camera runs entirely inside the tick. With the fixed tick it is updated 30 times per real second
with a regular 1/30 s between updates, as before.

## 10. D2: the fixed 30 Hz tick (2026-10-24)

### 10.1 What changed
`--kuf_fixed_tick` (default **on**, `src/runtime/frame_pacing.cpp`, `src/runtime/tick_schedule.h`):
- **Ticks run on a host clock.** The game thread's wait for "free" (`sub_826F2DB8`) is wrapped. After the game's
  own wait, the thread sleeps until the next tick time on a host clock: 1/30 s steps from an absolute schedule, where
  30 ticks take exactly one second. A late tick starts at once, so the game catches up by up to two ticks. A larger
  backlog (loading, a stall) is dropped, so afterwards the game never runs faster than real time.
- **Nothing else in the loop changes.**
  - The render thread still draws each tick exactly once (9.2).
  - The console's present interval still aligns frames with the guest vblank, so the default settings present
    as before.
  - Sleeping is the only waiting: no spin, no busy loop.
- **Counting.** The tick function (`sub_826F0D58`) and the release of "filled" (`sub_826F2E10`) are counted.
  `run-summary.json` gains:
  - `pacing` (mode, ticks, presents, backlog drops, tick-interval statistics);
  - `sim_tick_timeline` and `game_counter_timeline` (the runtime's count and the game's own counter, 0x82ABB740, per
    10 s);
  - `tick_frames_presented`.

  A `[kuf-pacing]` progress line logs ticks and frames per second. Input scripts, captures and the game-context
  log count frames that show a new tick, which is every frame unless the D1 repeat mode is on, so all existing
  scenarios keep their timing.
- **What it guarantees.** The guest vblank rate (`--video_mode_refresh_rate`), `--vsync=false`, the display and a
  fast GPU can no longer make the game faster than 30 ticks per real second. Under load the game slows exactly as
  the original did, but catches up after short hitches.
- **What it does not do.** It draws no more than 30 frames per second. Drawing a tick more than once is the blocker
  in 9.2, and D3 has to solve it differently (11).
- **The original pacing** stays available as `--kuf_fixed_tick=false`, for comparisons and the control test.

### 10.2 Measured (2026-10-24)
Setup:
- **Route:** the probe route of section 3 (`tests/timing/tick_route.txt`): CONTINUE in Shadow Depths, 180 ticks of
  walking, a camera turn, standing.
- **Machine:** RX 9070 XT, offscreen 1280x720.
- **Order:** one run at a time (`build/runs/d2s-*`).

| Run | Guest vblank | Game ticks per real second | Game speed | Frames per second | Frame time p50 / p99 | Tick interval p50 / p99 | Title to gameplay |
|---|---|---|---|---|---|---|---|
| original pacing | 60 Hz | 29.97 | 1.00x | 29.97 | 33.3 / 37.1 ms | 33.58 / 37.88 ms | 63.5 s |
| original pacing | vsync off (1000/s) | **203.67** | **6.79x** | 203.67 | 4.4 / 8.6 ms | 4.68 / 34.71 ms | 34.3 s |
| fixed tick (default) | 60 Hz | 29.98 | 1.00x | 29.98 | 33.3 / 37.5 ms | 33.33 / 36.92 ms | 63.5 s |
| fixed tick | 144 Hz | 30.00 | 1.00x | 30.00 | 33.3 / 36.6 ms | 33.33 / 33.38 ms | 63.7 s |
| fixed tick | 240 Hz | 30.00 | 1.00x | 30.00 | 33.3 / 38.1 ms | 33.33 / 33.38 ms | 63.2 s |
| fixed tick | vsync off (1000/s) | 30.00 | 1.00x | 30.00 | 33.2 / 37.7 ms | 33.33 / 33.38 ms | 63.6 s |

How the columns are measured:
- **Game ticks per real second:** 1,200 ticks between two tick-anchored captures, divided by their wall-clock times.
- **Frames per second:** the profiler's presented frames over the same window.
- **Tick interval:** the runtime's tick start times. The game's own tick counter (0x82ABB740) advanced with them in
  every run.

Findings:
- **The game speed no longer depends on the vblank.** It is 1.00x at every guest vblank rate, and loading and menus
  take as long as in the original. The original pacing at an unlimited vblank ran 6.8 times as fast, and its
  menus and loading too.
- **Ticks are regular.** With a vblank faster than 60 Hz the tick interval's p99 is 33.38 ms: the host clock's own
  regularity. At 60 Hz it is 36.9 ms, because the render thread consumes a tick only at the next even vblank and the
  game waits for a free slot, as in the original.
- **Backlog drops** (8-10 per run, at start-up and loading) are stalls longer than two ticks. After them the game
  continues from that moment instead of hurrying.
- **CPU and GPU, default settings:** the fixed tick costs the same as the original. Profile report over the same
  window: guest threads 111 %, GPU command thread 18.6 %, frame time median 33.31 ms, against 111 % / 19.0 % /
  33.28 ms.
- **CPU, faster guest vblanks:** the guest threads use less, 37 % with vsync off. At a 60 Hz vblank the render
  thread spins in the game's D3D library while it waits for the frame cap (KNOWN_ISSUES K10, `sub_82160C90`,
  measured in M6A). That spin is in the original, and M6D does not add one.
- **Same ticks, same picture:**
  - Captures at the same tick match the original run (whole-frame mean luma difference; for scale, 90 ticks of
    walking change about 12): before the walk 0.6-1.1, standing after the turn 2.0-2.3.
  - At the walk's end, the fixed tick at 60 Hz differs by 0.4. With faster vblanks the difference is 4.8 in all three
    runs, because the camera is still trailing the character.
  - The cause is the test harness, measured: the game's tick counter at a presented frame is level with the frame
    count at a 60 Hz vblank (4,342 of 4,391 frames). It is one tick behind without the vblank wait (4,195 of 4,392),
    so a scripted key, applied at a present, reaches the game one tick earlier. The character stops at the same
    place in every run. Real input is read by the game at the start of a tick and is not affected.

### 10.3 Tests
- **`timing.*`** (`tests/timing/`, `[scenario.timing]` in `tools/menu_test.py`): ticks per real second between
  tick-anchored captures, the pacing mode, one presented frame per tick, and the game's own tick counter against the
  counted ticks.

  | Test | Run | Pass condition | Result |
  |---|---|---|---|
  | `tick_rate_fixed` | fixed tick, default settings | 24-30.6 ticks per real second | 29.98 |
  | `tick_rate_fixed_240hz` | fixed tick, 240 Hz guest vblank | same range; the walk's end within 6.0 of `tick_rate_fixed` | 30.00 |
  | `tick_rate_fixed_vsync_off` | fixed tick, vsync off | as `tick_rate_fixed_240hz` | 30.00 |
  | `tick_rate_original_vsync_off` | original pacing, vsync off (control) | at least 45 ticks per real second | 205.71 (6.86x) |

  The control proves that the check sees an acceleration. If acceleration ever returns, the fixed-tick tests fail.
- **`pacing.*`** (unit, `tests/pacing/test_tick_schedule.cpp`): 30 ticks take exactly one second, a late tick
  catches up by at most two ticks, a stall drops the backlog, and a restart continues from now.
- **`input.*`**: the `freeze` verb's parser cases.
- **The whole existing suite** runs with the fixed tick, now the default (10.4).

### 10.4 Regression
**Final D2 regression (2026-10-25, HEAD e57c39e, build current):** `ctest -j8`, log
build/logs/m6d-d2-ctest-final.log.
- **1,776 of 1,776 passed**: no failures, nothing skipped or not run, 12,816 s.
- Every camera test (16, M6B) and display test (31, M6C) passed.
- Game folder unchanged (`tools/verify_game_dir.sh`: 138 files). The 41 save files have the same SHA-256 as before
  D2. `tools/check_tracked.py` clean; no game process left over.

First full run, before the trace fix: full suite (`ctest -j8`, fixed tick on by default, log build/logs/m6d-d2-ctest-1.log): 1,771 of 1,776 passed in
3 h 34 min. Four camera runs failed one check each, and `camera.checks_fault_injection`, which reads their logs, was not
run: `camera_original`, `camera_fov_min`, `camera_fov_max` and `camera_fov_max_native`, "handover: the frame written
before the switch drawn with its own angle: no value in the window". The cause was the trace, not the camera. The
render-side projection trace starts at the write that changes the angle. With the original pacing the render thread
was still drawing the previous tick when that write happened, so the earlier frame was logged after it. With the fixed
tick the game thread waits for its scheduled start, so the render thread has drawn that frame before the change is
written. The trace now also logs the last render-side projection drawn, marked "drawn before the change", right after
the change (src/runtime/modern_camera.cpp). The check's pattern and bounds are unchanged. After rebuilding, the four
runs, the other camera runs they depend on, and the fault injection passed (10/10,
build/logs/m6d-d2-camera-rerun.log). In all four logs every render-side projection was drawn with the angle written
to its slot.

### 10.5 Limitations and risks
- **No more than 30 frames per second.** D2 decouples the game's speed from the display, VSync and the vblank. It
  does not yet draw more frames: a tick can be drawn only once (9.2). Higher frame rates need D3 (section 11).
- **At the default 60 Hz vblank** frames are paced exactly as before, including the original's occasional
  long frames (p99 37 ms).
- **Under heavy load** (several games at once, a slow GPU) the game still slows down as the original does, because a
  tick waits for the frame before it to be drawn. Unlike the original it catches up after short stalls (up to two
  ticks), never faster than real time.
- **Test harness:** with a faster vblank, scripted input lands one tick earlier relative to the game (10.2). The
  timing tests allow for it, and existing scenarios run at the default vblank.
- **Diagnostics:** the D1 repeat mode and freeze are diagnostics with known black frames. They are off by default,
  `--kuf_render_repeat` logs a warning, and `freeze` refuses to run without it.


## 11. Revised plan for D3 (after D1)

The plan in sections 4-5 assumed the render thread could draw a tick again with blended transforms. D1 shows it
cannot without deep changes: the engine's contract is **one render slot, drawn once**. Two ways forward, to be
decided by a research step before any interpolation code:

| Option | Idea | For | Against |
|---|---|---|---|
| **A. One slot per displayed frame** (recommended to investigate first) | Between two ticks, build further render slots from blended state (camera, then objects), each drawn once by the unchanged renderer | Keeps the renderer's consume-once contract; the draw, the frame end and the GPU resource handling stay exactly as the game wrote them | The tick's scene build (`sub_821F49E8`, `sub_821F4D00`) also runs the physics step and other per-tick work: it has to be separated into a render-only build that can run without simulating. Unknown whether that split is clean [?] |
| **B. Make a slot drawable more than once** | Defer every consumer found in 9.2 (and the ones not yet found), then draw with blended transforms | The render thread alone; the game thread unchanged | At least seven consumers, some in physics-owned objects; each found one at a time with the freeze tools; a missed one gives black or wrong frames, a wrong deferral leaks or frees too late |

**D3a result (section 12):**
- **The scene build is not the game step.** A render-only build kept the game at 30.00 ticks/s, and the game's
  tick counter did not move. Not every word it changes could be attributed (12.6), so "it runs the physics step"
  is not supported, but a full gameplay-state audit is still a D3b test.
- **What it needs:** the game step's draw submission and UI lists. It also calls the CRT `rand()`.
- **Recommendation:** a hybrid of A and B (12.8).

Proposed next phase (done, section 12): **D3a, research (diagnostics only)**:
- Map the scene build: what it reads (object transforms, bones, camera, particles) and what it writes into the slot.
- Find what besides the render data it advances (physics, animation, effects).
- Test whether a render-only build can run twice between ticks without changing the game's state. Use the freeze
  memory diff, applied to the game thread.

Go or no-go for option A, else B, before any player-visible interpolation (D3b: camera and player). The rest of
the plan (D4 world, D5 settings, D6 performance) is unchanged. The fixed tick from D2 is the base for either option.

## 12. D3a: render frame data lifetime (2026-10-25, research only)

Approved as research only: no D3b, no interpolation, no higher frame rates. Marks as above: **[V]** verified (code
read, or measured in the runs named), **[L]** likely, **[?]** unknown. Everything here was measured on the D2 build
(fixed tick), offscreen at 1280x720, one game run at a time.

### 12.1 Tools and runs
New diagnostics, opt-in, all in `src/runtime/frame_pacing.cpp`. Nothing changes by default:
- `--kuf_render_builds=N`: after each tick the game thread builds N more render slots from the same game state,
  without a game step, and hands each to the render thread, which draws it once with its unchanged code.
  - A build is the tick's own slot build: `sub_821A2E70` (slot marked as a world frame), `sub_821F49E8`,
    `sub_821F4D00` and `sub_821F4CD8` (game slot index flip).
  - It runs while the game thread holds both "free" slots, so the render thread is idle.
  - With the input script's `freeze` it holds the ticks, keeps building, captures the last tick's frame
    (`<label>_tick`) and three build frames, and diffs guest memory against a control period.
  - `--kuf_render_build_same_slot` (default true) rebuilds the slot the last tick wrote; `=false` builds the other
    slot.
  - `--kuf_render_build_submit=true` also runs the game state's draw submission (`state->vf28`) in each build.
- `--kuf_trace_render_lists=<tick>` (with builds) logs:
  - the slot containers at the slot build and at the frame end;
  - the guest call chains of everything that adds to the frame packet in that tick: LR, then the r1 back chain.
- `--kuf_trace_slot_writes=<tick>` writes, for 4 ticks, every guest word changed by the game step, by the slot
  build and by the frame in between, with the render thread idle during each tick (`slot-writes-<tick>.txt`).
- A `rand()` counter: CRT `rand` is `sub_827FACD0`, the MSVC LCG on the thread's own seed. The counter separates
  calls made inside builds from calls made in ticks.
- SDK patch 0009 (GPU trace) now also logs, for each draw with a pixel shader:
  - its depth, stencil, blend and colour-control registers;
  - the float constants it reads.

Runs (`build/runs/d3a-*`):

| Run | What | Result |
|---|---|---|
| `d3a-same-`, `d3a-other-` | builds, same slot / other slot; freezes at the title, standing, walking, status menu | every gameplay build frame black (mean 0.0, HUD included); title frames normal |
| `d3a-flag-` | + slot-valid counters | all 180 builds marked as world frames (the draw's gate is open) |
| `d3a-gpu-` | GPU trace, 40 swaps | tick 524 draws / 13 resolves; build 139 / 12: no world geometry, no HUD |
| `d3a-lists-`, `d3a-writers-` | list sizes, packet writers | the packet's object list (52) is filled in the game step and erased by the frame end |
| `d3a-submit-`, `d3a-submitgpu-` | builds + `state->vf28` | 505 draws (the world is back), only the 19 HUD draws missing; image still black |
| `d3a-dump-`, `d3a-const-` | resolve dumps, shader constants, render state | every resolve has content except the last (front buffer), which is all zeros |
| `d3a-fsi-` | builds + vf28, `--render_target_path_vulkan=fsi` | **build frames show the correct scene** (no HUD) |
| `d3a-repeatfsi-` | D1 repeat mode (`--kuf_render_repeat=120`), FSI | **repeated frames show the correct scene** |
| `d3a-slotw-` | slot write trace, 4 ticks of play | per-slot data written by the game step (12.4) |
| `d3a-hud-`, `d3a-nosub-` | 2D list sizes, `rand()` counts | 4,050 `rand()` calls in 180 builds, with or without vf28 |
| `d3a-timing-` | the timing route with one build per tick | 1,200 ticks in 40.00 s (30.00/s, 1.00x); 1.996 frames per tick |
| `d3a-fsi3-`, `d3a-repeatfsi30-` | three builds per tick; repeat at 30 | 72 build frames/s reached; two captures per run cannot time effects |
| `d3a-anim120-`, `d3a-anim30-` | GPU trace with constants, repeat at ~120 vs normal 30 | effect clocks step 1/30 s per drawn frame (12.6a) |

### 12.2 Data flow of one tick and one frame [V unless marked]
```
GAME THREAD, one tick (sub_826F0D58)                        RENDER THREAD, one frame (sub_826E9FC8)
 wait "free" (sub_826F2DB8) + host clock (D2)
 simulation (subsystems, physics, AI, scripts, M6B camera)
 sub_821A2E70 ............. slot-valid byte = 1 ----------.
 sub_825BE6B0(ui) ......... UI update + 2D submission ----+--> ui + 136 / + 168 + slot*16   (HUD items)
 state->vf24 (update) ..... + 1 camera-attached object ---+--> FRAME PACKET renderer + 26584 + slot*272
 state->vf28 (submission) . world near the camera, units -+      +4 object list (sub_821A2CA0: ~52 entries)
                             (sub_8268E5F8, sub_8250F528)  |      per-object per-slot data (obj->vf52, vf16)
 sub_821F49E8 (scene build 1) consumes renderer + 29276;   |
                             obj->vf200 per packet entry  |
 sub_821F4D00 (scene build 2) views, matrices, lights,  --+--> view + 1124/1156/1188 + slot*16 (per-view lists)
                             shadows; ~22 rand() calls     |    packet buckets: 16 x 16-byte records (~200)
 sub_821F4CD8 ............. game slot index flip           |
 release "filled" (sub_826F2E10) ----------------------->  wait "filled" (sub_826F2EA0)
                                                            state->vf36 (UI/HUD side, render thread)
                                                            clear; draw sub_821F5608:
                                                              gated by the slot-valid byte
                                                              writes PM4 into guest memory
                                                              clears the view lists (sub_821EA3A0)
                                                            state->vf40 (2D list renderer + 28976, unused here)
                                                            frame end sub_821A3BA0:
                                                              erase the packet's object list
                                                              run deferred commands (renderer + 29444)
                                                              reset 3 frame-memory pools
                                                              valid byte = 0; keep-alive releases (renderer + 420)
                                                              flip the render index; collect GPU resources
                                                            present sub_821A3C98 (3 front buffers, fence throttle)
                            <-----------------------------  release "free" (sub_826F2E40)
GPU: the SDK's command processor reads the PM4 in guest memory in place, executes draws and resolves into EDRAM,
     host render targets and guest memory, and on the swap hands the front buffer to the presenter (3-slot mailbox).
```

### 12.3 The ten questions
| # | Question | Answer | Mark, evidence |
|---|---|---|---|
| 1 | How does the simulation construct render commands? | In two stages. **(a) Game step:** the game state's **submission pass** `state->vf28` (`sub_825D7678`) adds the visible objects to the slot's **frame packet**: the world near the camera (`sub_826A5CA8` → `sub_8268E5F8`) and the units (`sub_8250F528` → `sub_8245D9A0`). Each object goes in through `sub_821A2CA0(renderer, index, matrix)`, which also calls the object's vf52 and vf16(1). The update `state->vf24` adds one camera-attached object (`sub_826EAF40`). The UI manager (`sub_825BE6B0`) adds the visible UI elements to its own per-slot lists. **(b) Scene build** at the tick's end: `sub_821F49E8` and `sub_821F4D00` turn the packet into per-view lists and 16 buckets of draw records, and set up views, matrices, lights and shadows. The GPU commands themselves (PM4) are written by the render thread while it draws | [V] call chains at tick 1700 (`d3a-writers-`): 52 packet objects (30 units, 20 world, 1 from the scene's own vf28, 1 camera-attached), ~200 bucket records, all bucket records from `sub_821F4D00` |
| 2 | How do they reach the render thread? | Through the two-slot handoff. Every per-slot container exists twice in the renderer. The game builds slot `0x82AE760C`, flips it at the tick's end and releases "filled"; the render thread draws slot `0x82AE7610`. Nothing is copied: both threads address the same arrays | [V] code; list trace |
| 3 | Which buffers are reused or overwritten? | All per-slot containers, every other tick. The three frame-memory pools (dynamic vertex and constant data) are reset at each frame end. The device cycles three front buffers (device + 260 + 4i, i = frame mod 3) when triple buffering is on (device + 272). EDRAM is rewritten every frame | [V] code (`sub_821E2CA8`, `sub_821A3BA0`); 12.4 |
| 4 | Which commands consume or clear input data? | The frame end erases the packet's object list. The draw clears the per-view lists. The frame end also runs and empties the deferred commands, resets the pools, releases the keep-alives and clears the slot-valid byte. The HUD's per-slot UI data is consumed too: build frames have no HUD although `vf36` runs every frame. The draw also empties the D1 lists (render command queue, Phys_Callback +88, 0x8210109C +152, 0x820306B0 +100) | [V] list trace, GPU trace; [L] where the UI data is cleared (consumer not located) |
| 5 | Which GPU resources persist? | Persistent: textures, the game's render targets and resolve targets (rewritten each frame at the same addresses), vertex and index buffers in guest memory, and the SDK's shared memory, texture, render-target and pipeline caches. Per frame: the frame-memory pools. Per slot: deferred deletions, executed at that slot's frame end, and GPU resources released once the GPU is done (`sub_821C8710`) | [V] code; SDK report |
| 6 | What are the synchronisation signals? | The semaphores "free" (count 2) and "filled" (sync object 0x82AC6658), and the two slot indices. The present waits while more than two frames are in flight (device fences +10908 vs *+10896). The GPU writes fences and memory through EVENT_WRITE / MEM_WRITE packets. The SDK swap goes through a 3-slot mailbox to the presenter | [V] code |
| 7 | How do frame indices advance? | Game slot index: at the tick's end (`sub_821F4CD8`). Render slot index: at the frame end. Draw counter renderer + 29640: per draw. 0x82AE7630: per slot marked as a world frame. 0x82ABA780: per world tick. ui + 200: per UI update. Tick counter 0x82ABB740: per tick. Front buffer: mod 3 per present | [V] code |
| 8 | Side effects beyond rendering? | (a) **CRT `rand()`**: the scene build calls it about 22 times per build on the game thread, so an extra build shifts the game thread's random sequence. (b) The submission pass writes per-object data: obj vf52 (matrix) and vf16(1); 36 objects with vtable 0x821010E0 change 6 words per slot. (c) The UI update advances a counter and runs `sub_825BC8A0` in the same function as the 2D submission. (d) Visual effects (ground mist, fireflies, grass) change between frames drawn from one frozen game state, in builds and in render-thread-only repeats. (e) The Phys_Callback list is emptied by the draw (D1). With the game held and nothing built or drawn, gameplay memory is still (25-44 bytes in 3 s) | [V] (a) 4,050 calls in 180 builds, with or without vf28; [V] (b) writes, [L] meaning; [V] (c) code; [V] (d) captures, rate dependence in 12.6; [V] (e) D1 |
| 9 | Can commands be captured or reconstructed safely? | **GPU capture or replay: no.** The PM4 is read in place, contains fence, interrupt and memory-write side effects, there is no trace player, and replay cannot move anything. **Engine-level reconstruction: yes, for the scene build.** Running `sub_821F49E8` and `sub_821F4D00` again from the same game-step outputs gives the same frame structure (505 of 524 draws, only the HUD missing), and the image is correct on the FSI path. It needs the game step's outputs (packet object list, UI lists) present, either re-submitted or retained | [V] SDK report; GPU trace; FSI captures |
| 10 | Can independent render-frame state exist without modifying simulation state? | **Yes, within the two slots.** The scene build writes only renderer data plus the game thread's CRT seed (restorable). More than two slots is not practical: 227 functions compute slot addresses from the two indices. A render-only frame is therefore built into one of the two slots on the game thread between ticks, while the render thread is idle, as the D3a builds do. That keeps the game at exactly 30.00 ticks/s (1.00x) at one build per tick. Re-running the submission pass instead also writes per-object data | [V] static count (D1), `d3a-timing-`; [L] "only renderer data": not every changed word could be attributed (12.6) |

### 12.4 Resource lifetimes [V unless marked]
| Data | Where | Written by | Consumed or cleared by | Lifetime |
|---|---|---|---|---|
| Slot-valid byte | renderer + 29656 + slot | `sub_821A2E70` (tick) | frame end | one frame; the draw skips the scene when 0 |
| Frame packet: object list | renderer + 26584 + slot*272, +4 | game step: vf28, vf24 (`sub_821A2CA0`); +2 by `sub_821F4D00` | frame end (`sub_8220FB00` erase) | game step to frame end |
| Frame packet: 16 buckets | same, +16(k+1) | `sub_821F4D00` (`sub_821BC580` → `sub_821A3390`) | the draw and frame end [L] | scene build to frame end |
| Per-view lists | view + 1124/1156/1188 + slot*16 | `sub_821F4D00` | the draw (`sub_821EA3A0`) | scene build to draw |
| UI per-slot lists | ui + 136 / + 168 + slot*16 | `sub_825BE6B0` (tick) | render side (vf36 path) [L] | tick to draw [L] |
| Queued work | renderer + 29276 + slot*16 | the game step | `sub_821F49E8` | within the tick |
| Deferred commands (28 B) | renderer + 29444 + slot*16 | the game step (deletions) | frame end (`sub_821AA7A0`) | until the slot's frame end |
| Frame-memory pools | 0x82AE81FC + slot*48; renderer + 8 / + 29308 + slot*48 | the draw | frame end (reset) | one frame |
| Keep-alives | renderer + 420 | scene build (`sub_821F00D8` / `sub_821F0948`) | frame end (`sub_821F0BB8`) | until the slot's frame end |
| Per-object per-slot words | 36 objects (vtable 0x821010E0) +76..84 / +140..148; renderer object[10] (19 words); two heap blocks (82 words each) | game step | overwritten two ticks later | two ticks (double-buffered) [L] |
| Front buffers | device + 260 + 4i | present (resolve) | presenter (SDK swap) | three presents |
| Textures, render targets, caches | guest memory / SDK | load / every frame | never (deleted via deferred commands) | level |

### 12.5 Exact cause of the black frames [V]
A repeated or rebuilt frame is black because three separate things are missing. Each was confirmed by removing the
one before it:
1. **No world.** The frame packet's object list is filled only by the game step, through `state->vf28` / `vf24`.
   The frame end of the tick's own frame has already erased it, so a rebuild finds it empty.
   - Effect: 139 instead of 524 draws; shadows, sky and post-processing still run.
   - Confirmed: with `vf28` replayed in each build, the list is back (51 of 52) and so are the draws (505).
   - The D1 repeat mode skipped this erase, which is why its list stayed at 45.
2. **No HUD.** The HUD's 19 draws (vertex shader 55F0A5216126584F) come from the UI manager's per-slot lists. Those
   are filled only in the game step (`sub_825BE6B0`), and are missing in every build.
3. **The black resolve: an SDK render-target-cache effect, not the game.**
   - In the tick's frame, after the final composite, the HUD draws bind the 1280-pitch colour surface (14000500).
     Then come 24 small draws with colour writes off, and the resolve to the front buffer.
   - Without the HUD, those 24 draws keep the 640-pitch 4x MSAA surface (0A020280) of the depth-only draws before
     them. The SDK's default host render-target path (`fbo`) then resolves zeros, so the whole frame is black.
   - Everything else is identical in the two frames: shaders, constants, textures, depth, stencil, blend and
     colour-control registers. Every earlier resolve has content.
   - On the EDRAM-exact path (`--render_target_path_vulkan=fsi`) the same build frames show the correct scene. So do
     the D1 repeat frames, which were black on `fbo` for this reason.
   - The exact mechanism inside the render-target cache (ownership transfer between the two layouts at EDRAM base
     0) is **[L]** and is filed as K34.

The first D1 conclusion ("the renderer consumes the tick") is true for (1) and (2). The total blackness, HUD included,
was (3).

### 12.6 Side effects of a render-only build [V unless marked]
- **Game speed:** unaffected. One build per tick on the timing route: 1,200 ticks in 40.00 s, the game's counter
  equal to the counted ticks, 1.996 presented frames per tick.
- **Random numbers:** 22.5 `rand()` calls per build, from the scene build itself: 4,050 in 180 builds with or without
  `vf28`. Builds run on the game thread, so they advance the seed that gameplay randomness uses (drops, AI, combat:
  K24) **[L]**. A rebuild has to save and restore the game thread's seed (tiddata + 20; `sub_827FACD0` reads it via
  `sub_827FE1A8`).
- **Submission pass (`vf28`):** writes per-object data through obj vf52 / vf16. It is a separate pass from the update
  (`vf24`), but re-running it is not proven harmless **[?]**. A design that retains its output avoids running it again.
- **Memory with the game held:** builds changed 1.5-1.8 MB per 3 s (mostly physical memory: GPU data and pools),
  against 25-44 bytes in the control period. Most of the changed words are renderer and heap data whose owners the
  diff cannot name. A run-independent audit (game state hashed per tick, with and without builds) is a D3b
  validation item, not done here.
- **Effects that move between frames of one frozen state:** ground mist, fireflies and grass, in builds (mean
  difference 2.0-3.8 between captures 1 s apart) and in render-thread repeats (1.8). Their timing is in 12.6a.
- **Throughput of the diagnostic:** with three builds per tick the frozen game reached 215 build frames in 3 s
  (72/s, not 120). A build plus its draw on the FSI path takes about 14 ms. The diagnostic waits for each draw;
  this is not yet the cost of one build on its own.

### 12.6a Effect timing [V]
- **Method:** the GPU trace (`d3a-anim120-`, `d3a-anim30-`) logs every pixel shader's constants for 12 consecutive
  swaps in idle gameplay: once with the D1 repeat mode at about 120 frames per second (FSI path), once in the normal
  30 frames per second.
- **Effect clocks advance per drawn frame:** two constants (c0 of pixel shader 55429B55..., c6 of 67F20911...) are
  effect clocks. They grow by **0.033 (1/30 s) per drawn frame** at both rates: every 8 ms at 120, every 33 ms at
  30. So they are frame counters scaled by 1/30, not real time.
- **They are advanced by the render thread:** the repeat mode runs no scene build, so the step is in the draw
  path.
- **Effect:** at 120 frames per second these effects (the ground mist among them [L]) would run four times too
  fast.
- **Tick-driven constants:** camera and light constants of the same shader (c13-c15) change only when a new tick is
  drawn, every frame at 30 and twice in 11 frames at 120.
- **Consequence for D3b:**
  - the render-side effect clock must advance by the real frame interval, i.e. 1/30 s times the frame's fraction
    of a tick, instead of 1/30 per drawn frame;
  - or it must advance only on a tick's first frame, at the cost of smoothness.
  - Where the step is written is not yet located [?].

### 12.7 Architecture comparison
- **A: independent render slots.** Per presented frame, re-run the submission and the scene build from presentation
  state.
- **B: preserve and defer.** Keep the tick's built slot, defer every consumer, and redraw it.
- **C: SDK-level capture and replay.**

| Criterion | A | B | C |
|---|---|---|---|
| Correctness | Scene correct (`d3a-fsi-`). Needs the UI submission too, which is mixed with the UI update. Re-running vf28 writes per-object data [?]; the scene build moves the `rand()` seed (fixable) | Scene correct (`d3a-repeatfsi-`). Every consumer must be deferred, HUD included; one missed gives missing or black content; each deferral risks a leak or a late free | Exact repeat only. Fences, interrupts and memory writes in the stream must be filtered |
| Complexity | Medium-high: separating the UI's submission from its update; scheduling builds | Medium: about ten consumers, some in physics-owned objects (D1) | High, for little gain |
| CPU overhead | One scene build per presented frame on the game thread (cost not yet measured; `sub_821F4D00` is the heavy part) | Near zero | Low |
| GPU overhead | Full frame per present (same for all) | same | same |
| Memory | None extra | None extra | One command stream copy per frame |
| Synchronisation risks | Builds share the game thread with the tick and need both slots idle; proven at one per tick | The render index has to stay on a slot the game may want back; the game waits for "free" | Replayed fences confuse the game's throttle |
| Native resolution, ultrawide (M6C) | Kept: each build runs the view setup, where the M6C projection lives [L] | Kept: same view, drawn again | Kept (same image) |
| Camera interpolation | **Natural**: the view setup in the build takes a presentation camera (view matrices), leaving the simulation camera alone | **Not possible** without rebuilding view-dependent data (culling, shadow cascades), which is the scene build, i.e. A | No |
| Object interpolation | Possible: interpolated transforms at submission or at the per-object per-slot data [?] | Only by patching drawn data in place [?] | No |
| Gameplay timing | Unchanged (30.00/s measured) | Unchanged | Unchanged |
| Effect clocks (12.6a) | Must be made time-based | Must be made time-based | Repeats frames, no change |

### 12.8 Recommendation
**A hybrid: "A over retained game-step outputs"** (rebuilt render slots, without re-running the game's submission):
1. The game step runs once per tick, unchanged, M6B camera included.
2. **Snapshot after the tick:** the outputs of that game step that the renderer consumes. These are the frame
   packet's object list, the UI per-slot lists, and the per-object per-slot data written during submission.
3. **One build per presented frame:** for each extra frame before the next tick, the game thread restores that
   snapshot into the slot and runs only the scene build (`sub_821F49E8`, `sub_821F4D00`, `sub_821F4CD8`) with
   presentation inputs. It saves and restores the CRT seed around the build. The render thread draws and consumes
   the slot once, with its own unchanged code, as in D3a.
4. **Interpolation** then lives in presentation data only:
   - the camera, as view matrices handed to the view setup in the build, interpolated between the last two ticks'
     M6B camera outputs (the simulation camera is never written);
   - later the objects, as interpolated transforms in the retained per-object data.

Why not the others:
- **B** cannot move the camera without rebuilding view-dependent data, so it shows the same picture at a higher
  rate. No smoothness for the camera, characters, world or enemies, which the requirements ask for.
- **C** is ruled out by the SDK analysis.
- **Plain A** (re-running `vf28` and the UI submission per frame) runs game code with per-object side effects at the
  render rate. Retaining its output gives the same frames without running it.

Why the hybrid:
- It keeps the renderer's own consume-once contract: frame end, deferred deletes and keep-alives run once per drawn
  slot exactly as written. So none of B's lifetime risks apply.
- Its one new mechanism, the render-only build, was measured to keep the game at 1.00x.

Required first, whatever the approach: the black resolve (K34) has to be fixed in the SDK's `fbo` path, or avoided,
because any frame without the HUD's draws turns black there.

### 12.9 Implementation phases (proposal; each stops for approval)
| Phase | Content | Done when |
|---|---|---|
| D3b-0 | SDK: reproduce and fix the `fbo` black resolve (K34) as an SDK patch; or keep the HUD in every frame | build frames without HUD correct on `fbo`; display and render-compare tests unchanged |
| D3b-1 | Retained game-step outputs + render-only builds at the tick's own camera, paced to the display (60/120/144/240/uncapped = 1/3/~3.8/7/as many as fit per tick, by host clock); CRT seed preserved; the render-side effect clocks stepped by real frame time (12.6a) | build frames equal the tick frame (HUD included) within capture noise; 30.00 ticks/s; gameplay state hash identical with and without builds; no leaks (pool and GPU memory stable over 30 min) |
| D3b-2 | Camera interpolation on presentation data: view matrices from the last two ticks' camera, blended at present time | camera tests unchanged at 30; smooth camera pans at 60-240; M6B camera code untouched |
| D3b-3 | Player and objects: identify the per-object transform data (vf52 matrix, the 6 per-slot words), bones and skinning; interpolate | characters, world and enemies smooth; animations correct |
| D4-D6 | as planned: world and effects, the PC Settings frame-rate option (30 stays the default), performance | |

### 12.10 Risks
- **Scene build cost [?]:** at 240 Hz, 7 builds per tick on the game thread. If one build is too slow, the frame
  rate is limited, but never the game speed: builds are skipped when late, as in D3a.
- **Snapshot completeness [L]:** a missed game-step output gives missing objects in extra frames, which shows as
  flicker. Mitigation: compare build frames with tick frames at the same camera (D3b-1 test).
- **Per-frame effect clocks [V, 12.6a]:** effect clocks advance 1/30 s per drawn frame, so at higher frame rates
  those effects run proportionally faster. The step has to be found and made time-based (D3b-1). Others may exist:
  the test is a frozen game at two frame rates.
- **Random numbers:** solved by saving and restoring the seed, but other hidden per-build state (LOD hysteresis,
  occlusion queries) may exist **[?]**.
- **Slots and the game thread:** builds hold both slots. A slow build delays the next tick's start, by design at
  most until the tick's scheduled time (late builds are skipped).
- **K34 on `fbo`:** until fixed, any frame without the HUD draws can go black. That includes the game's own
  HUD-less moments, if the same surface sequence occurs there **[?]**; no such frame was seen in the regression.

### 12.11 Validation tests (for D3b)
- Build frame vs tick frame at the same camera (FSI and `fbo`): mean difference within the frozen-state effect
  noise; HUD present; no black frame.
- Gameplay determinism:
  - the same scripted route with and without builds, with the game's state (player position, HP, EXP, gold, and
    the `rand()` seed) logged per tick;
  - identical sequences required.
- Game speed: `timing.*` at 60, 144 and 240 Hz with builds: 30 ticks/s, the game counter equal to the counted
  ticks.
- Lifetime:
  - 30 minutes of play with builds at 240;
  - frame-memory pools, GPU resource counts and process memory flat;
  - every deferred command run exactly once (counter).
- Camera (D3b-2): all `camera.*` tests at 30 unchanged; at 144, the drawn camera between the two ticks' cameras
  (trace).
- Display: `display.*` render compares at native resolution and ultrawide with builds on.
- Effect speed: with the game frozen, effect clocks (shader constants, 12.6a) must advance by elapsed real time at
  30, 60 and 144 frames per second.
- Trace anchoring: GPU traces used by tests should be anchored to a game tick, not a swap number. Since D2, swap
  numbers shift with loading time, and builds add swaps (K35).

### 12.12 Existing limitations, reviewed
| Limitation | Affects the architecture? |
|---|---|
| One-tick presentation and input alignment at higher refresh rates (10.2: scripted input lands one tick earlier without the vblank wait) | **Test harness only.** Real input is read at the tick's start. Interpolation adds up to one tick of display delay (8), the same in any approach. Scripts should keep counting tick frames, which builds already do (build frames are not counted) |
| Uneven pacing at 60 Hz (p99 36.9 ms: a tick waits for the next even vblank) | **Yes, a design input:** extra frames must be placed by host clock (D3a's builds are evenly spaced from the tick's start), not by vblank parity. At 144 Hz the frames per tick alternate (4/5), which interpolation hides and repetition does not |
| K32 (no more than 30 fps) | **Explained by 12.5.** The consumed packet and UI lists make a redraw empty; the SDK's `fbo` resolve makes it black. Not a hard limit of the engine |
| K33 (one unexplained "game folder content changed") | **No.** Test tooling only; the script already prints the failing file on a real mismatch. No change |

### 12.13 Verified vs hypotheses (summary)
- **Verified:**
  - the cause chain in 12.5;
  - the packet writers and their call chains;
  - the slot-valid gate;
  - correct scenes from builds and repeats on FSI;
  - `rand()` use by the scene build;
  - 30.00 ticks/s with builds;
  - effect clocks stepping 1/30 s per drawn frame on the render thread;
  - the lifetimes in 12.4 marked [V].
- **Likely:**
  - the K34 mechanism inside the render-target cache;
  - the meaning of the per-object per-slot words;
  - where the UI lists are consumed;
  - the M6C projection applying unchanged in builds.
- **Unknown:**
  - the cost of one scene build;
  - other per-build state;
  - side effects of re-running `vf28`;
  - bones and skinning data for D3b-3.

## 13. D3b-0, K35 and the D3b-1 investigation (2026-10-25)

Approved: D3b-0 (fix K34 at its root), the K35 test correction, the README, and D3b-1 as an investigation with an
implementation proposal. Not done, as instructed: the D3b-1 architecture itself, D3b-2, interpolation. Marks as in
section 12.

### 13.1 D3b-0: K34, the black resolve [V]
**Reproduced** with the D3a render-only builds plus the submission pass (`--kuf_render_builds=1
--kuf_render_build_submit=true`) on the default `fbo` path: every build frame all zeros, the tick's frame normal
(runs `k34-diag1-`). A temporary log of the SDK's ownership map at each draw and resolve of the traced frames gave the
mechanism:

1. Draws 477 and 480, the game's depth-only passes (`RB_MODECONTROL` 5), bind a 640-pitch 4x MSAA depth buffer at
   EDRAM tile 720 with a scissor of 8192x8192. The SDK estimates a render target's extent from the draw and claims it
   in its ownership map; here the claim covers all 2048 tiles, wrapping past tile 2047 over the frame's 1280x720
   colour at tile 0. The ownership change transfers the colour into the depth buffer (`xfer0 [0,720) from C0`).
2. In a tick's frame the HUD's draws bind the colour again and transfer it back before the final resolve. Without
   them the final resolve finds tiles 0-719 owned by the depth buffer (`[0,2048)=D720/p16/m2`) and dumps them from it.
3. The dump (`DumpRenderTargets`, rectangles from `GetResolveCopyRectanglesToDump`) addresses the source as
   (dump base + dispatch offset + tile) - render target base, with offsets relative to the dump area. A render target
   owns the tiles from its base to its base + 2048, so for tiles below its base, owned through wrapping, that index
   was negative: outside the image, read as zeros. Only a wrapping *dump area* got the one-period correction; a render
   target reaching the dumped tiles by wrapping did not, and a wrapping dump area owned by a render target that does
   not wrap was read one period too far.

**Fix, SDK patch 0010:** each owner's range is split at the render target's base, and each part's rectangle carries
the period correction (`source_period_offset_tiles`, added to every dispatch offset by `GetDispatches`), so the
dump base plus the offset is the tile's index in the render target's own period. The EDRAM buffer is written at the
same tiles as before (the shader wraps that index); ranges needing no correction dump exactly as before. No copy, no
clear, nothing visual: the resolve now reads what the EDRAM holds, as on the EDRAM-exact path. Shared code, so the
D3D12 backend gets it too (not built or tested here).

**Results:** build frames on `fbo` show the scene (no HUD: K32) and match the `fsi` path within 0.07-0.20 levels mean,
the same as the two paths' difference on a normal tick frame. The tick frame vs its build frames: 16x16 block median
0.3-2.1 (the HUD and moving effects).

**Normal 30 fps rendering:** a temporary counter of corrected dumps over ten ordinary scenarios (menus, new game,
fight, camera, ultrawide, native 21:9 and 4K, movies, the display menu, the original camera) found the correction in
every run, always the same case: about 1,150 per run (3,216 in the movie run), on title, menu and saving-screen
frames, a full-frame copy of the colour at 0 into the texture at 0x1EC10000 while the depth buffer owns those tiles.
A GPU trace of a title frame shows that texture overwritten by a later resolve (before draw 75) before any draw reads
it (draw 78). Captures: title and menu bit-identical with the pre-fix run; gameplay captures differ from a pre-fix run
exactly as two pre-fix runs differ from each other (e.g. 0.247 vs 0.329, 0.119 vs 0.119 mean). So the fix changes
data that normal frames never show.

**Tests:**
- `gpu.*` (`kuf_gpu_tests`, tests/gpu/test_resolve_dump_wrap.cpp, no GPU needed): the dump shader's addressing
  emulated for every dumped tile: the K34 frame, a range across the owner's base, a wrapping dump area of a
  non-wrapping owner, ordinary ranges unchanged, 20,000 random geometries. With the period correction forced to 0 (the
  old behaviour) 4 of the 5 cases fail.
- `render.k34_build_frames_fbo` / `_fsi` and `render.k34_check` (tests/render/k34_check.py): the build frames on
  `fbo` are not black, match the tick frame outside the HUD, and match the `fsi` run (mean within 1.0, 8x8 blocks
  within 0.6; measured 0.08 / 0.05). On the original SDK all three checks fail (black frames, median 23.9).

K32 is separate and unchanged: the rebuilt frames still lack what the tick's own frame consumed (13.3).

### 13.2 K35: render comparisons at a game moment [V]
**Measured cause:** the render tests traced `--gpu_trace_frame=1840`, a swap number. Each guest `VdSwap` (the game's
only call site, counted in telemetry.cpp) becomes one PM4 swap packet that the SDK's command processor executes once,
in order, so swap numbers are exact, but the swap reached at a game moment depends on loading: in the stored runs
swap 1840 was gameplay +339 to +344. The 32:9 bloom difference depends on the moment (K36): the native 4x2 value is
constant (green 0.2535-0.2543 in every run since M6C), the 1x reference's is 0.2598-0.2605 at +339..+341 (2.4 %
apart, passes the 3 % limit) and 0.2616-0.2638 at +342..+344 (3.2-3.8 %, fails). Run on its own the comparison
failed 4 of 4 times today; in the full suites the trace had happened to land on +339..+341.

**Fix (tests only, nothing in the game):**
- Input script `trace`: GPU-trace the next frame showing a new tick. When the render thread starts that frame, the app
  sets the SDK cvar `gpu_trace_frame` to the number of frames presented so far, which is that frame's swap number, and
  logs `[kuf-trace] GPU trace of tick frame F (requested at frame R, game counter C): swap S`.
- The eight render scenarios enable the trace without a frame (`--gpu_trace_frame=2000000000`, so the cube map dumps
  at level load are kept) and trace the tick frame after `@gameplay+340`: gameplay +341, the moment the tests were
  validated at in M6C.
- render_compare.py `anchor`: in each run the traced frame is the requested one (F = R + 1, the trace's swap is S),
  and both runs traced the same number of tick frames after entering gameplay.
- `resolves`: the game's bloom alternates between two buffers by frame parity (0x1E1BE000 / 0x1E236000), which a
  fixed swap number used to keep equal in both runs. The sequences may now differ by exchanging exactly that one pair
  (both used by the reference, same sizes and formats); the report names it. Any other difference still fails.
- No tolerance changed; the HDR, cube map and colour checks are as before.

**Validation:** all five pairs pass at +341 (green in the bloom: 1080p -0.8 %, 1440p -0.9 %, 4K -1.2 %, 21:9 -0.1 %, 32:9
-2.5 %). `render_compare_32_9` run alone: 2 of 2 pass. **Fault injection:** with SDK patch 0008 reversed (K31's red
cast at render scales above 1) the same comparison fails on the cube maps (every texel), the HDR resolves and the
colours; the patch was then restored and the tree rebuilt (patch check clean).

Found on the way: K36 (the bloom's moment-dependent difference at scale, not a regression) and K37 (one run in 55
entered gameplay 31 frames late through the intro skip, with a brighter scene at every anchored moment).

### 13.3 D3b-1 investigation: generating frames between ticks [V unless marked]
Diagnostics added (opt-in, frame_pacing.cpp): `--kuf_render_build_restore` (render-only builds restore the game step's
consumed outputs instead of re-running the submission) with build-cost logging; the input script's `trace` used to
trace a tick frame and its builds. Runs `build/runs/d3b1-*` (1 and 3 builds per tick, idle and walking in the Shadow
Depths save) plus static reading of the guest code.

| Area | Finding |
|---|---|
| World object lists | The frame packet's list (renderer[(slot + 6782) * 4]) holds **renderer object indices**, stable IDs into the object array at renderer + 29272 (247 objects in the save's level; classes 0x821010E0 x156, 0x821013D0 x56, 0x82101540 x27, 0x82103828 x4, 0x821034C8 x2, object 0, and the view). `sub_821A2CA0(renderer, index, matrix)` hands the matrix to the object (vf52), lists the index (except index 10) and calls vf16(1), which sets a sticky "enabled" byte at +168 (cleared only by constructors) |
| Object transforms | vf52 (0x8219A748, every class except 0x821034C8) copies the 4x4 world matrix to **object + 28 + slot * 64**: double-buffered per game slot. The previous tick's matrix stays in the other slot until the tick after next |
| Camera | Object 10 is the view (class 0x82102A90; `view_index 10`). The camera manager (`sub_825246D0`) builds the camera matrix from its rows and hands it to the view through the same `sub_821A2CA0`, so it lands in view + 28 + slot * 64; the field of view goes to view + (slot + 191) * 4 (`sub_821EA080`). The M6B simulation camera (manager +176, the camera object) is separate and is never needed by a build |
| HUD and menu lists | The UI update (`sub_825BE6B0`) walks its elements and fills per-slot pointer lists at ui + 104 / 136 / 168 + slot * 16 (and ui + 88 for elements flagged at +6). The elements are persistent objects; the lists only reference them. Restoring these lists does **not** bring the HUD back (next row): the render-side HUD path (state->vf36) needs something else [?] |
| Other per-frame lists | Restoring the packet list, the 2D list (renderer + 28976 + slot * 16) and `sub_821A2D48`'s list (renderer + 27136) gives 485 of the tick's 524 draws: missing are the HUD's 19 and 20 draws of ten objects drawn in two passes (depth 3BDD3A54..., colour CE03BE93...). Re-running the submission brings those ten back (505), so they come from the submission's other per-frame lists, which the draw empties (D1: the physics callback object +88, objects of vtables 0x8210109C +152 and 0x820306B0 +100, renderer object 228 + 3612) [L] |
| Render command generation | Unchanged from 12.2: the scene build (`sub_821F49E8`, `sub_821F4D00`) turns the packet into per-view lists and buckets on the game thread; the render thread writes the PM4 while drawing. A frame consumes its lists by setting each vector's end back to its begin; storage and capacity stay, so a retained list is restored in place without guest allocation (0 failures in 32,400 restores) |
| GPU resource lifetimes | Deferred deletions and keep-alives are per slot and run at the slot's frame end (12.4). The tick's own frame runs first, so the tick's deletions happen once; a build re-adds keep-alives in its scene build and releases them at its frame end, balanced. Objects in the retained list were submitted by that tick, so they are alive [L] |
| Frame-buffer ownership | Three front buffers (device + 260 + 4i, mod 3) and the present's fence throttle (more than two frames in flight wait) work per present, independent of ticks. The EDRAM ownership case of K34 occurs in every frame without the HUD and is now correct |
| Frame indices | Same-slot builds set both slot indices back to the tick's slot and flip the game index after the build (`sub_821F4CD8`), so the tick's per-slot data stays current and the previous tick's stays in the other slot. Counters advancing per drawn frame on the render thread: the draw counter (renderer + 29640) and the effect clocks (12.6a; location still [?]) |
| Synchronisation | Builds run on the game thread between ticks, while it holds both "free" slots, so the render thread only draws what it is handed and never races the game step. The scene build reads live game objects, so it cannot run on the render thread while the next tick simulates [L]. Each build waits for its draw (serial) |
| Costs | Render-only scene build: 2.6 ms mean, p95 3.3 ms in play (the tick's own: 2.7 ms); the whole tick 3.0 ms mean. With 3 builds per tick the game presented **119.8 frames per second at exactly 30.00 ticks per second** (1280x720, `fbo`, serial builds). GPU per frame (M6C, PERFORMANCE.md): 3.3 ms at 1x, about 10 ms at native 4K or 32:9 |
| Random numbers | Builds still call the game thread's CRT `rand()` (about 12 per build in play): its seed must be saved and restored around each build (12.6) |
| Visibility draws | Each frame ends with 24 small (16x16) depth-tested draws without colour writes (vertex shader C049A8C9...), probably occlusion queries for flares [?]; extra frames would issue them again. To check what reads their results |

### 13.4 Architecture comparison and recommendation
| Criterion | A. Independent render slots rebuilt from saved simulation data | B. Reusable render data presented several times | C. Capture and replay of the SDK's graphics commands |
|---|---|---|---|
| Correctness | The scene correct on both paths now (13.1); 485 of 524 draws with retained lists so far, the rest identified (13.3) | A built slot drawn again: every consumer must be deferred, some in physics-owned objects (D1); a miss is a missing or black part, a late free a leak | Exact repeat of one frame only; fences, interrupts and memory writes in the PM4 must be filtered (12.3 q9) |
| Interpolation | **Natural:** each build's view setup takes a presentation camera written into the view's slot matrix (view + 28 + slot * 64), so culling, shadows and lists follow it; objects through their per-slot matrices; the simulation untouched | Camera or object motion needs view-dependent data rebuilt (culling, shadow cascades, lists): that is A | None: the commands contain baked transforms |
| Performance | One scene build (2.6 ms) per extra frame on the game thread, plus its draw; 120 fps measured with serial builds at 1x | Near zero CPU | Low CPU, a stream copy per frame |
| Synchronisation | Builds between ticks, both slots held; late builds skipped, so frame rate can drop but never game speed | The render index must stay on a slot the game wants back | Replayed fences confuse the game's throttle |
| Simulation | Untouched (30.00 ticks/s measured with builds); seed restore needed | Untouched | Untouched |

**Recommended: A, as "render-only builds from retained game-step outputs"** (the hybrid of 12.8, now with the data
inventory). B and C produce extra frames that cannot carry camera or object interpolation, so they would have to be
replaced later; A is the one that supports interpolation, keeps the engine's own consume-once renderer, and has
measured headroom.

**Proposed D3b-1 implementation (for approval):**
1. Snapshot at the tick's scene build (game thread): the packet list, the 2D list, renderer + 27136, the UI lists,
   and the submission's other per-frame lists still to be pinned down (the ten objects); find the HUD's render-side
   input and include it. Restore before each build; never re-run the submission or the UI update.
2. Builds paced by host clock between ticks: n = refresh / 30 - 1 extra frames (60 Hz: 1, 120: 3, 144: about 3.8,
   240: 7), placed evenly from the tick's start; skipped when the next tick is due. The 30 fps default does no builds,
   so it stays exactly as now.
3. Around each build: save and restore the game thread's CRT seed; leave every simulation structure untouched.
4. Effect clocks: locate the per-drawn-frame step (12.6a) and advance it by real time.
5. No interpolation yet: builds at the tick's own camera (D3b-2 then writes the presentation camera into the view's
   slot matrix, and D3b-3 the objects' matrices from their two per-slot copies).

Done when: build frames equal the tick's frame (HUD included) within the moving-effect noise on `fbo` and `fsi`;
30.00 ticks/s at 60-240 Hz; the gameplay state per tick identical with and without builds (route log of position, HP,
EXP, gold and the seed); 30 minutes at 240 Hz with flat memory and pools; every deferred deletion run once.

### 13.5 Performance and synchronisation risks
- **Game-thread budget:** 7 builds per tick at 240 Hz are 18 ms of scene builds per 33 ms tick, plus the tick itself
  (3 ms) and the serial waits for each draw. At 1x the render thread's draw is short; at native 4K the GPU's 10 ms per
  frame limits the rate to about 100 fps whatever the CPU does. Mitigation: skip late builds (the game speed is never
  affected); later, overlap a build with the previous build's draw by building into the other slot, which needs the
  tick's per-slot data copied there and the previous tick's kept on the host (a complete inventory first).
- **Retained-set completeness:** a missed list shows as objects or HUD flickering between tick and build frames; the
  tick-vs-build capture check catches it (as it shows the 39 missing draws today).
- **Hidden per-frame state:** effect clocks (faster effects at high rates), the visibility draws, the rand seed, LOD or
  animation state advanced by the scene build [?]. Test: a frozen game at 30, 60 and 144 fps with the effect constants
  compared (12.11).
- **Pacing:** builds placed by host clock, not by vblank parity (10.2: a tick waits for an even vblank at 60 Hz);
  frames per tick alternate at 144 Hz (4/5), which interpolation hides and plain repetition does not.
- **Input latency:** interpolating between the last two ticks shows the game up to one tick (33 ms) behind; input is
  still read at the tick's start, as now.
- **Memory:** builds reuse the slot's pools and lists; the D3a and D3b-1 runs showed no growth from builds (their RSS
  includes the freeze diagnostic's 3.2 GB snapshots). A 30-minute soak is part of D3b-1's validation.
- **K34 class of problems:** any frame without the HUD exercised the SDK's EDRAM wrapping case; fixed, and covered by
  `gpu.*` and `render.k34_*`.

## 14. D3b-1: render-only frames (2026-10-25)

Approved: option A (13.4) and the K38 correction. Not done, as instructed: camera and object interpolation (D3b-3,
D4). M6D is not merged. Marks as in section 12.

### 14.1 Checkpoint A: K38, scripted input at the game's tick [V]
**Cause:** the input script applied its items after presented frames (the present hook, render side); since D2 the
game thread ticks on its own clock and reads input at its tick's start, so which tick saw an item depended on how far
the game thread was ahead of the render thread. That lead varied between runs (`tick_lead_at_present`: 44 to 2,888 of
4,400 frames at -1). In the failing frames one tick saw two scripted looks and the next none (pitch 19.05 -> 19.05 ->
21.05 degrees, eye step 0.07 then 38.2). A second race for controller items: the SDK's SDL driver only sees a virtual
pad change after the UI thread's next asynchronous pump (`QueueControllerUpdate`, sdl_input_driver.cpp).

**The unusual failure (2026-10-22, stick segment):** not K38. The camera stopped at 40 units per frame when the stick
was released while it slid in along the ground (a correct stop), against the bound of that day (15); 61bc429
(2026-10-23) raised that bound to the segment's largest step (60) for exactly this case. Under today's bound it
passes.

**Fix (input_script.cpp, game_input_hooks.cpp; no camera code, no bound changed):** script time is game ticks. An input
item at N is applied on the game thread when tick N + 1 starts (`OnGameTick`, from the tick hook), before the tick
reads input; the virtual pad's joysticks are updated there (`SDL_UpdateJoysticks`), so its events reach the driver in
that tick. capture, trace and freeze run after the frame showing tick N (every tick is drawn once). Anchors count from
the tick in which the game entered the context (`context: gameplay (tick T, frame F)`). `sdl` items still go through the
window's queue (they test the desktop event path).

**Validation:** new check `input_ticks` (menu_test.py) in the seven camera tests with a per-tick trace: between items
at ticks N and M the trace has exactly M - N lines. 408 of 408 items one tick each on the new build; the two stored
pre-fix runs that failed show 105 and 109 items wrong; fault injection (test_camera_checks.py) catches a look and a
stick change that arrive a tick late. Camera input at 54, 101 and 195 presented frames per second (D1 repeats): 408 of
408 each. All 14 game-run camera tests and the fault injection pass; the render comparisons trace the same moment
(gameplay + 341) with the tick anchor.

### 14.2 What a render-only frame needed (Checkpoint B) [V]
A render-only build (13.3) restored the frame packet's list and two other lists: 485 of 524 draws. The rest, and what
else a frame consumed or advanced, were found with freeze memory diffs per build phase (a page-set tracer marking every
callee of the scene build and the draw), GPU traces of a tick's frame and its build's frame (shader constants, then a
temporary hash of the vertex shader constants), hardware watchpoints under gdb in the sandbox, and the disassembly.
Each finding is handled in `src/runtime/render_frames.cpp`:

| What | Found | Handling in a render-only frame |
|---|---|---|
| Frame packet object list, renderer + 27136 | object indices, emptied by the frame (13.3) | restored before the build (values) |
| Two-pass object draw records (renderer + 14520's object + 156 + slot * 16) | 68-byte records (object index and matrix) written by the submission (`sub_821A6A68`); the draw (`sub_82224C60`) empties them, keeping the storage: the **ten objects' missing draws** | restored before the build |
| HUD (19 draws) | not the 2D list: the HUD is drawn from the 2D draw-list object at renderer + 30156 (vtable 0x8210376C), lists at + 88 / + 120 / + 152 + slot * 16 of **heap entries submitted each tick**; the HUD's draw ends with `sub_8222B2C8(lists, slot)`, which deletes them | the render thread defers that release; the game thread runs it when the next tick into the slot starts (all of the slot's frames drawn, nothing written yet) |
| 2D list (renderer + 28976 + slot * 16) | heap blocks of per-tick animation deltas; state->vf36 (`sub_821ABF50`) applies them **and frees them**. The D3b-1 diagnostic's restore of this list (13.3) wrote back freed pointers: unsafe, removed | consumed once, by the tick's frame |
| Frame memory (three arenas: 0x82AE81FC + slot * 48, renderer + 8 / + 29308 + slot * 48) | growable arenas (block + 36, used + 40, overflow list + 4 / + 20) freed at every frame end (`sub_821B07E0`, `sub_821B03D8`, `sub_821B0648`); they hold the tick's per-frame allocations | the tick frame's end keeps them (mark); a build frame's end frees back to the mark; the full release at the next tick into the slot |
| Bone palettes of skinned meshes | written by the tick's **physics step** (the physics manager's pre-simulate updates, `sub_821C39E8` -> `sub_8219A9B8`) into frame memory, pointers at mesh + 856 / + 864 + slot * 4; the scene build's first part clears them (mesh vf200, `sub_8219E278`). Without them a build drew the character in its bind pose | the pointers kept across the build's clear (the memory is kept above) |
| Physics | scene build 2 runs the physics manager (renderer + 30160, vtable 0x82103790): callbacks (vf28) and the step (vf24: pre-simulate updates, PhysX simulate 1/30 s, fetchResults). **Every build ran a physics step** (1,989 simulate calls in builds against 1,798 in ticks), and all ~22 rand() calls per build came from it | neither runs in a build (0 simulate, 0 rand() in builds) |
| CRT rand() seed (game thread) | the build's calls would shift the game's sequence (12.6) | saved and put back around each build (now no calls remain) |
| World frame count 0x82AE7620 | +1 in `sub_821A2E70` (marks the slot as a world frame); units animate by it (unit + 716 = start + count - start count) | the slot is marked, the count put back |
| Renderer draw count (renderer + 29640) | +1 per scene draw; meshes stamp their previous-frame matrices with it | the build's frame counts as the tick's frame (one back before its draw) |
| Frame timer (renderer + 28864: + 12 frames) | the effect shaders' time is frames / 30 (`sub_821D24F0`; c0 of PS 55429B55, c6 of 67F20911: 12.6a) | one back when the build's frame starts: same time as the tick's frame |
| An effect system (renderer + 444, vtable 0x821032F0) | + 80 counts scene builds (`sub_8221EEB0`) | put back after the build |
| Bloom (post-processing renderer + 412, vtable 0x82102A6C) | blends with the previous frame's bloom; the two buffers alternate by the selector at + 84 (the K35 parity) | the build's frame uses the tick frame's buffers |
| Two-pass draw accumulation (+ 1600: + 160 / + 164) | swapped every frame (`sub_82223A70`) | the build's frame starts from the tick frame's choice |
| Previous-frame matrices (mesh + 228..+ 292, `sub_82235EC0`) | each mesh's motion since the frame before | the build's frame repeats the tick frame's (no flicker between frames with and without motion) |

**Result:** the build's frame equals the tick's: 540 of 540 draws, every draw identical in the GPU trace except vertex
fetch addresses (fresh per-frame buffers) and shadow-projection constants within 2.3e-4; the captured images differ by
a mean of 0.006 levels (largest 16x16 block 0.53, no pixel over 32) at 1280x720, 0.006 / 0.55 at native 4K and
0.008 / 0.64 at 32:9; the effect clocks equal (63.9 in both frames); three render-only frames 0.6-2.7 s apart differ
by at most 1 level (the effects no longer move while the tick is held).

### 14.3 Simulation isolation (Checkpoint C) [V]
- **Physics and random numbers:** 0 PhysX simulate and 0 CRT rand() calls in render-only builds at every rate
  (run-summary `physx_in_builds`, `rand_in_builds`; tests `timing.tick_rate_frames_*` require 0).
- **Memory with the game held:** 180 builds changed 250 KB (237 KB physical: GPU data and command buffers; 12 KB of
  heap), against 1.85 MB before D3b-1 (the physics step's writes) and 12 bytes in the control period.
- **The game state per tick** (`--kuf_trace_game_state`: the game's tick counter, the game thread's seed, every
  submitted object's position, at each tick's slot build) along the timing route, against a 30 fps run: the same
  objects and positions within 0.07-0.16 units (the camera's view object; two 30 fps runs differ by up to 2.27) at
  60, 144, 240 and as many frames as fit; the seed diverges in every pair, the 30 fps pairs included, at gameplay
  + 617-660 (run-to-run nondeterminism of the original game, K24). One 120 run of two entered gameplay with two other
  distant units submitted; its repeat matches the 30 fps run.
- **The game's speed:** exactly 30.00 ticks per second at every rate; the game's own tick counter advances with the
  ticks; the walk ends where the default run's does (tests).
- **By construction:** a build runs only the two scene build functions and the flip; the game step (AI, combat,
  scripts, timers, inventory, saves, the M6B camera) never runs in one. Frames, never ticks, are dropped when time
  runs out.

### 14.4 Implementation (Checkpoint D) [V]
- `--kuf_frame_rate` (default 0: one frame per tick, the original): 31-480, or -1 for as many as fit. Needs the fixed
  tick (turned on). `--kuf_render_builds=n` stays the diagnostic (n per tick, the freeze). Removed:
  `--kuf_render_build_submit`, `--kuf_render_build_same_slot`, `--kuf_render_build_restore` (superseded); new
  `--kuf_render_build_hud=false` keeps the K34 case (frames without the HUD) for its tests.
- **Scheduling (frame_pacing.cpp `AfterTick`):** a presentation clock in steps of 1 / rate; the tick's own frame takes
  the first step from the tick's start, render-only frames the following ones before the next tick is due. Each frame
  only if the render thread has finished the one before (waited for up to one step, no spinning: a condition variable
  signalled when the render thread releases a slot) and the build (its measured cost plus 0.5 ms) ends before the next
  tick is due; otherwise the frame is dropped (`builds_late`, `builds_busy`). The game thread does not wait for a
  build's frame to be drawn.
- **The 30 fps default is unchanged:** with `--kuf_frame_rate=0` every new hook passes straight through.

### 14.5 Measurements [V]
Timing route (Shadow Depths save, walk, turn, stand), 1280x720, RX 9070 XT, gameplay 60-120 s of each run:

| --kuf_frame_rate | frames/s | ticks/s | CPU | GPU busy | builds dropped (late / render busy) | build cost |
|---|---|---|---|---|---|---|
| 0 (30) | 30.0 | 30.00 | 238 % | 30 % | - | - |
| 60 | 59.9 | 30.00 | 204 % | 46 % | 251 / 775 | 0.92 ms (p95 1.05) |
| 120 | 119.6 | 30.00 | 268 % | 78 % | 36 / 2,517 | 0.86 ms (p95 0.99) |
| 144 | 137.0 | 30.00 | 289 % | 82 % | 653 / 3,092 | 0.87 ms (p95 1.08) |
| 240 | 158.2 | 30.00 | 309 % | 85 % | 2,174 / 9,880 | 0.87 ms (p95 1.07) |
| -1 | 159.0 | 30.00 | 310 % | 85 % | - | 0.87 ms (p95 1.08) |

- 144 and 240 are not reached: the render thread and GPU limit the rate to about 160 frames per second at 1280x720
  (each frame is the full scene, shadows and post-processing). The achieved rate is reported, not the target.
- A render-only build costs 0.87 ms on the game thread (the tick's own scene build 2.55 ms: the physics step).
- Tick intervals: p50 33.33 ms, p99 34.8-35.0 ms at every rate (35.0 at 30 fps too).
- Presentation pacing in gameplay (profile frame times, 60-120 s): 60: p50 16.68 ms, p99 19.35; 120: p50 8.35,
  p99 10.64; 144: p50 6.95, p99 11.65; 240: p50 6.49, p99 8.82; as many as fit: p50 6.53, p99 8.98. One stall of
  about 76 ms per minute at every rate, 30 fps included (the game's own).
- **Soak, 30 minutes at --kuf_frame_rate=240** (walking and turning every 1,000 ticks): 56,903 ticks, 366,719 frames
  (about 239 per second in the last minutes), 309,815 render-only builds, 0 rand() / PhysX calls in them, 0 restore
  failures, no error lines; resident memory 1,378 MB after 5 minutes, 1,380 after 10, 1,382 after 20 and 1,389 after
  30 (+11 MB over 25 minutes); tick intervals p50 33.333 ms, p99 33.372 ms.
- **Native resolutions (GPU-bound):** at 3840x2160 and 5120x1440 (Render Native) --kuf_frame_rate=240 gives 68.0 and
  69.1 frames per second with the GPU 100 % busy (30 fps: 64-65 %); the ticks stay at 30.00 per second (2,051-2,600
  frames dropped for lack of time, 18,000-19,100 because the render thread was still drawing: the game is never held
  back).
- **Input to presentation:** from the start of a tick (where it reads input) to the present of its first frame, over the
  whole run: 30 fps p50 47.3 ms (mean 49.7, p99 72.4); 120 p50 8.8 ms (mean 17.5); 4K / 32:9 at 240 p50 16.0 / 16.2 ms.
  Render-only frames shorten it: the game thread hands the next tick to an idle render thread (it waits for the
  render thread before each build), where the default pipeline draws a tick while the next is simulated. Input is still
  read once per tick; without interpolation what is shown changes 30 times per second.

### 14.6 Synchronisation [V unless marked]
- Builds run on the game thread between ticks, only while it holds both "free" slots (the render thread has finished
  every slot and waits); the render thread draws what it is handed, in order (the slot kind queue). A build never
  overlaps the game step or a draw.
- Releases deferred from a tick's frame (HUD entries, frame memory) run on the game thread when the next tick into the
  slot starts: the game's own two-slot hand-over guarantees the render thread is done with that slot then, and the
  tick has not yet written into it.
- The render thread's idle state is counted (slots handed, slots released), with a condition variable for the game
  thread's bounded wait; no polling, no busy wait.
- 0 restore failures in 309,815 builds (soak); no error lines in any run.
- **Risks:** the deferral keeps a slot's frame memory one tick longer (memory flat in the soak); a game path that
  releases a slot outside the frame end (level unload) runs as before, the deferred release then finds nothing or
  repeats a release the game expects to be idempotent (checked once: `gameplay.new_game` at 120, a new game through the level load into gameplay and back to the menu four times, 10,191 render-only builds, 0 restore failures, no error lines; other level loads and unloads are not covered [?]); the addresses are v1's (each checked
  against its vtable where it is used).

### 14.7 Ready for interpolation? [V unless marked]
Yes, in this design:
- each render-only frame runs the tick's own scene build (views, culling, shadows, lists) from retained per-slot data,
  so a camera written into the view's slot matrix (view + 28 + slot * 64, field of view at view + (slot + 191) * 4)
  before the build is used by everything downstream (D3b-3);
- objects keep their previous tick's matrix in the other slot (object + 28 + slot * 64), the inputs for object
  interpolation (D4); the retained lists are by object index;
- the clocks a frame advances are now explicit (world frame count, draw count, frame timer, effect counters, the bloom
  and accumulation alternation, previous-frame matrices): interpolation can give each frame a fractional time instead
  of the tick's;
- remaining work for D3b-3: the presentation camera (from the two last ticks, without changing the simulation
  camera), the previous-frame motion per presented frame, and the skinned meshes' palettes (they come from the tick's
  physics step: interpolating bones needs both ticks' palettes, now kept only for the current one) [?].

### 14.8 K46: an object released after the tick's lists were saved [V] (2026-10-27)
A 120 fps play session crashed 17 ms after an in-game save (`sub_821A2FD8` reading object 0 at its second entry, on
the game thread during a render-only build). Two of the lists a build restores, the renderer's slot list and
`sub_821A2D48`'s list (renderer + 27136; the scene setup `sub_821F4D00` calls `sub_821A2FD8`, which calls vf184 and
vf24 on each listed object and empties the list), hold indices into the renderer's object array (renderer + 29272,
count + 29268). The tick saves them at its slot build; when the tick released a listed object after that, the build
replayed an index whose entry was empty. Correction (render_frames.cpp): with each saved index the object it named is
kept, and a restored index whose object is gone or another is left out (`restore_dropped` in the run summary and the
900-build log line). The object pass's records (`sub_821A6A68`) index something else (indices 2 to 4 with no
renderer object) and are restored as before. Measured: 9 indices left out in `gameplay.120-combat_loot` (37,328 builds),
1 in `presentation.bones_120`, 0 in three 120 fps runs of `save_continue`; the session's exact save was not reproduced
by script (the scripted Sleep needs an Idol). Tests: `-L "timing|render|presentation|save"` 60 of 60.

**Regression test (M6D K46 checkpoint, 2026-10-28).** The session's drop was in the scene setup list; in scripted
runs the builds left out indices only in the slot list (1-3 per fight at 120, all objects released to 0), whose
consumer tolerates them: three fight runs with the correction off (`--kuf_debug_restore_released`) kept those indices
and did not crash, and two Quit saves at 120 (the fixture and a copy of the session's own saves) left out none. So the
test adds the case itself: `--kuf_debug_restore_stale_every=N` gives every N-th tick's saved scene setup list an
index whose renderer object entry is empty (an object listed, then released) and an index now holding another object
(reused); the logged line per left-out index names its list. `render.k46_released_120` (every 30 ticks through a
walk, the Quit save, the title and a new game's autosaves): 252 records added, all left out (640 records left out by
the builds of their ticks), 0 restore failures, each of the 3 committed saves followed by records within 2 s, no
crash. `render.k46_released_fault` (the correction off): the run crashes as the session did (`fault_guest
0x00000000` in `sub_821A2FD8`, called from `sub_821F4D00`), required by `render.k46_fault_check`.

## 15. D3b-3: camera and player interpolation (2026-10-26)

Approved: presentation-only interpolation of the camera and the player character between ticks. Not done, as
instructed: world, enemy, particle and bone interpolation (D4), settings integration (D5), optimisation (D6). M6D is
not merged. Marks as in section 12.

### 15.1 What a frame shows: the timeline [V]
- **Ticks:** the game runs at 30 Hz on the host clock (D2). Tick N starts at t_N, reads input there (K38: scripted
  input applied at the tick's start), runs the game step (AI, combat, the M6B camera, physics) and builds its render
  slot. Nothing in this section changes any of that.
- **Snapshots:** at each tick's scene build (the `sub_821F49E8` hook, game thread, after the game step) the
  presentation code reads what the tick handed to the renderer: the gameplay view's camera matrix and field of view
  for the slot (view + 28 + slot * 64, view + (slot + 191) * 4; the view is renderer object 10, vtable 0x82102A90),
  the M6B camera's record of the tick (`kuf::camera::LatestTickView`: which camera placed the view, its pivot, the
  attach point, whether it snapped, turned whole or was pulled in) and the player model's world matrix for the slot.
  Two snapshots are kept: the previous tick (A) and this one (B).
- **Presentation time:** each frame gets alpha = (time from t_N to when the frame reaches the render thread) / (1/30 s),
  clamped to 0..1, where the frame's scene build cost (the tick's own about 2.6 ms, a render-only build's about 0.9 ms,
  each a running estimate) is added to "now". The frame shows game time N - 1 + alpha: the state between ticks N - 1
  and N. The tick's own frame is interpolated too (alpha about 0.08); render-only frames follow at the presentation
  clock's steps (14.4).
- **Bounded delay, no extrapolation:** a frame never shows beyond the newest completed tick (alpha at most 1). If tick
  N + 1 is late, the frames hold at tick N (alpha 1; run-summary `presentation.held`). Missed or dropped frames do not
  change alpha: each frame computes it from the clock, so irregular frame rates show the right moment, just fewer of
  them.
- **Writing:** before the slot's scene build runs (both for the tick's own frame and for each render-only build) the
  camera matrix, field of view and player matrix for that frame are written into the slot. The scene build then
  derives everything from them: view and projection, culling (the view's lists), the shadow fit, the skinning (the
  render thread multiplies each bone by the object's slot matrix and the current view's matrix, `sub_8219E030`), the
  character's parts (they copy the parent's slot matrix). So culling, shadows and the projection follow the displayed
  camera by construction (15.6).
- **The game's own values come back:** the game writes the slot at its next tick into that slot (two ticks later).
  If it does not (no view handed on in menus and while loading, the player not moved), the slot still holds the
  presented value: this is detected (the stored value unchanged) and the game's own value taken back, so such a tick
  shows and hands on exactly what the game had there (run-summary `presentation.restored`: 1,153 ticks in the new
  game and 802 in the fight, almost all in boot, menus and loads).

### 15.2 Camera [V]
- **Simulation untouched:** the M6B camera computes its view at 30 Hz as before, with its own smoothing, collision,
  distance, field of view, recentring and sensitivity; `modern_camera.cpp` only records per tick what it did
  (`TickView`: the camera object, event or modern, pivot, attach point, target height, the turn taken whole, snapped,
  pulled in). No camera state is changed for the presentation and no extra smoothing is added.
- **Interpolation around the pivot (the modern camera):** the M6B view is an exact look-at (rows right, up, forward,
  eye; right = normalize(up_world x forward), no roll) aimed at its pivot on the character. Between two ticks the pivot
  (taken as the point on each view's line of sight nearest the camera's pivot, so the end points are exactly the
  ticks' cameras) is interpolated linearly, and the eye's offset from it by azimuth, elevation and distance; the view
  is the look-at from there at the pivot. A straight line between the two eyes (a chord) would cut inside the orbit
  and swing the aim off the character: around the pivot every frame stays aimed at it (check: 0.001 degrees).
- **Angle wrap:** the azimuth goes the shorter way round (across +-180 degrees correctly), except when the camera
  reports the turn it took whole in that tick (a mouse flick of more than half a circle): then the reported turn is
  used, so the frames turn the way the player turned. M6B's position smoothing moves the eye a fraction of the
  shorter way round the target each tick (`SmoothAround`); in those ticks no turn is reported and the shorter way is
  right. (Found by the flick test: assuming the full input turn spun the view 337 degrees the wrong way; fixed.)
- **Distance and collision:** the distance is interpolated linearly, except after a collision pull-in (the camera was
  pulled in at once because something came between it and the character): the frames take the new distance at once,
  as the game does, and only the direction moves on; if the direction jumped by more than 0.45 rad in that tick as
  well (in the fight a pull-in from 880 to 211 units tilted the view 55 degrees down) the camera was put elsewhere,
  and that is a cut. The game's own camera (M6B off) pulled in is a cut.
- **Field of view:** interpolated between the ticks' values (event cameras zoom); when the PC Settings field of view
  changed between the ticks it is taken at once.
- **Other cameras** (the game's own camera with M6B off, event and scripted cameras, any view not a look-at): rigid
  interpolation of the matrix (position linear, rotation by quaternion slerp the shorter way), with the cut rules of
  15.4.

### 15.3 Player [V]
- **What the player's transform is:** the character is a skinned model (renderer object class 0x821010E0, 69 bones
  in the Shadow Depths save; its weapon and other parts are child objects of class 0x82103828 that copy the parent's
  slot matrix in the scene build). Its bone palette (mesh + 856 + slot * 4) is in model space, written by the tick's
  physics step; the render thread skins with invBind x bone x (object slot matrix x view). So the character's world
  placement is one rigid transform, its slot matrix, and its pose is separate model-space data.
- **What is interpolated:** only the slot matrix (position linear, rotation slerp, scale linear), from the two ticks'
  matrices as the game wrote them. The bones keep the tick's pose (the animation advances 30 times per second until
  D4); the parts follow the parent's matrix, so the weapon stays in the hand. Not interpolated independently: the
  bones (hierarchical, model space, produced by the physics step) and the parts (derived from the parent).
- **Which model is the player:** the skinned model in the slot's draw list standing at the gameplay camera's attach
  point (the game's camera target before its smoothing, minus the camera key's target height), within 2 units
  (measured: within 0.12 when walking). Once found, the same model is kept while it stays within 60 units of the
  attach point: stopping from a run its root is up to 24 units from the attach point (measured in the fight), and in
  a fight another character came closer to the attach point for two ticks (it took the player's place once before
  this rule; now none in the fight). In the new game's opening, where the camera does not follow the character,
  nothing is found and nothing is interpolated.
- **Gameplay untouched:** movement, collision, combat, dodge timing, hitboxes and damage are computed by the game step
  from the game's own state; the slot matrix is presentation data the game thread does not read before rewriting it
  (15.7). The camera follows the character the game simulated; both are interpolated with the same alpha, so the
  character stays where the camera expects it (check: the camera aimed at its pivot on the character in every frame).

### 15.4 Discontinuities [V]
Each tick interval is either interpolated or cut; across a cut the frames show tick B as the game wrote it. Explicit
signals come first; distance and angle thresholds only catch what the game does not signal.

| Rule | Signal | Covers |
|---|---|---|
| first, gap | no previous tick, or a tick without a frame | start, loading hitches |
| not world | the tick did not mark its slot as a world frame (`sub_821A2E70`) | menus, pause, loading screens |
| view | another renderer or view object | level transitions |
| no view | the game handed the renderer no new view in this tick (`sub_825246D0` did not run) | loading, menus, scripted holds |
| event | an event camera placed the view (manager + 893, `sub_82524530`), or stopped | cut scenes start and end |
| camera | another camera object | camera changes |
| handover | the game's camera took over from the modern camera, or back | the new game's opening (1,615 units in a tick) |
| snapped | the game moved its camera at once: its own turn taken (right-stick centring, a camera change), or its position smoothing off | recentring, resets |
| pulled in | the game's camera: its collision correction grew by more than 0.5 units in a tick; the modern camera: pulled in and the view turned more than 0.45 rad in the tick | collision corrections |
| jump | the eye moved more than 2,500 units in a tick | a teleported camera without another signal |
| shot | event cameras: more than 250 units or 0.45 rad in a tick | a cut between shots |
| shot (game's own and event cameras) | the eye's step changed by more than 150 units from one tick to the next | the new game's opening changing shot (0.7, then 485, then 205 units back; its title flight slows by 15 units per tick, the original camera steps at most 28) | |
| player: none, object | no player model found, or another model | respawns, possession, cut scenes |
| player: event | an event camera | scripted movement in cut scenes |
| player: teleport | more than 400 units in a tick (the fight's largest step 77, p99 38) | teleports, respawns, level starts |
| player: turn | more than 2.6 rad (149 degrees) in a tick (the fight's largest 63 degrees, p99 15) | instant turns |
| player: matrix | not a rotation (reflected or degenerate) | |

- **Knockback and dodges** are motion the game simulates per tick (in the fight at most 77 units per tick): they are
  interpolated like any motion, the backstops are well above them.
- **Collision corrections:** the modern camera's pull-in is shown at once (15.2); the game camera's is a cut.
- **Measured in game runs at 120** (`gameplay.new_game_120`, `gameplay.combat_loot_120` with the trace;
  `presentation.discontinuities_play`): no frame interpolated across a cut in either run; the
  largest interpolated eye step 203.9 units (new game) and 159.1 (fight), the largest interpolated player step 23.7 and
  53.7; the player model never changed in gameplay; the opening's cuts caught (new game: handover 2, shot 2; fight:
  handover 2, shot 5, snapped 37). Before the fixes below the same check found 1,615 and 840 units.
- **Found and fixed while measuring:** the new game's opening hands the view from the game's camera to the modern
  camera and back with a jump of 1,615 units that no rule caught (a one-tick sweep across the level; now the handover
  rule); the opening's change of shot (485 then 205 units, the shot rule); a collision pull-in in the fight that also
  tilted the view 55 degrees, swept through in one tick (the pull-in rule); another character standing closer to the
  attach point for two ticks in the fight took the player's place (now the previous player is kept); the player was
  lost for 55 ticks while stopping from a run (the keep distance).

### 15.5 Timing, latency and the lower-latency alternative [V]
Measured on the timing route (Shadow Depths save; walk, turn, stand), 1280x720, the whole run (run-summary
`latency`, `pacing.tick_to_present_ms`), medians (p99):

| | 30 (original) | 120, interpolation off | 120, interpolation on | 240 on | as many as fit, on |
|---|---|---|---|---|---|
| tick start to its first frame (`tick_to_present`) | 57.3 (71.9) | 9.2 (69.1) | 8.8 (68.9) | 8.9 (68.9) | 8.7 (69.0) |
| tick start to the first frame showing the tick completely (`full_response`) | 57.3 (71.9) | 9.2 (69.1) | 42.2 (102.9) | 42.2 (102.8) | 42.0 (103.2) |
| age of the shown state at its present (`state_age`) | 57.1 | 21.7 | 39.0 | 38.7 | 37.0 |

(ms. `state_age`: from the real moment of the game time shown, between the starts of the ticks around it, to the
present; `full_response`: from a tick's start, where it reads input, to the first present showing game time N or
later.) Interpolation delays what is shown by one tick: input read at tick N starts to show in the first frame after
tick N (that frame is between N - 1 and N, so it already moves towards N's result) and is shown completely from tick
N + 1's first frame, 33 ms later. At 120 frames per second that is 42 ms to the full response against 57 ms in the
original 30 fps mode, and 9 ms with render-only frames without interpolation (which show each tick at once but move
only 30 times per second). The p99 values include menus and loading (30 frames per second there).

**Alternative considered: extrapolation from the last two ticks** (show N + alpha from N - 1 and N, no extra delay).
Measured offline on the 120 route trace (every tick as written; the error is against what the next tick then
showed): the camera's eye off by up to 0.86 units walking, 11 turning, 43 starting and stopping the orbit and 193 units
(16.9 degrees) after the 270-degree flick; over the whole route up to 426 units and 33.7 degrees (p99 142 units,
5.0 degrees); the player up to 3.4 units when the walk stops (15.3 over the route). Every change of motion (a turn
starting or stopping, the flick, a walk stopping) would be shown wrong and then corrected, a visible overshoot and
snap back, and a collision or a stop the game has not yet simulated
would be shown as passed. Interpolation was chosen: what is shown is always what the game simulated; the cost is the
bounded delay above.

**Irregular frame rates and missed frames:** alpha is computed per frame from the clock (15.1); a dropped frame
leaves a longer step, never a wrong moment. A late tick holds at alpha 1 (no extrapolation).

**Observation (state comparison, harmless):** in some runs with render-only frames the level load numbers 27 distant
scenery objects (class 0x82101540) one renderer object index higher than in another run of the same build: the same
objects at the same positions (D3b-1 saw it once, 14.3). The state comparison pairs objects by class and position.

### 15.6 Visual correctness [V]
- **Intermediate frames differ, step by step (the sweeps):** each freeze of the route draws its held interval at alpha
  0, 0.25, 0.5, 0.75 and 1 and captures them. At 1280x720 the four steps of the walk are 1.99-2.00 levels mean each,
  the turn 4.03-4.11, looking up 3.27-3.54 (native 4K and 32:9 alike, 2.32-4.50); the middle frame lies between the
  two ticks' images (2.68 / 2.69 levels to each end for the walk); render-only frames drawn while held equal the alpha
  1 frame within 0.008 levels. The captures were viewed: across the walk's sweep the character stays at the same
  place on screen with its sword in its hand while the scene moves behind it (camera and character interpolated
  together), the HUD in place. (Shadows are too faint in this forest to judge by eye; they follow from 15.1.)
- **On the path the game simulated (every frame traced, `presentation.check`):** the character within 0.013 units of
  the straight path between its two ticks at the frame's alpha; the camera within 0.014 units of its distance from
  the pivot and 0.005 degrees of its direction; turning and looking up within 0.005 degrees; every interpolated
  modern-camera frame aimed at the interpolated pivot within 0.001 degrees; no frame repeating the one before; the
  shown game time never going back; the flick shown along the path the game's camera took (22.5 degrees a tick, the
  shorter way: M6B's smoothing), never a spin.
- **Culling, shadows, projection:** written before the scene build, the presented camera is the one the build culls,
  fits the shadows to and projects with (15.1). The isolation run shows it: every frame drawn from a camera 400 units
  above the game's shows a complete scene from there (no objects missing at the edges), with the
  game's own culling. The HUD is drawn in screen space and does not move.
- **Native resolution and ultrawide:** the same checks pass at 3840x2160 and 5120x1440 at 240 (GPU-bound at about 68
  frames per second there, 14.5).

### 15.7 Isolation [V]
- **Isolation test (`presentation.isolation`):** every presented camera raised by 400 units (the player by 300) and
  its field of view widened by 0.1 rad, at the ticks too (`--kuf_presentation_test_offset=400`), against the same
  route without: the rand() seed equal at every tick and every submitted object equal (0.0000 units difference) for the
  first 600 ticks of gameplay; the simulation camera within 0.1 units (the M6B camera's own run-to-run variation: it
  eased by real elapsed time; two runs without the offset differed as much). Since K43 (17.1) the camera is timed by
  the tick and the comparison is exact: objects at the trace's 6 decimals and the camera equal at every tick. A presented camera that leaked into the
  game would be 400 units off. `presentation.state_compare_detects` shows that the comparison catches a game only one
  tick apart (objects 14 units, the camera 473).
- **Interpolation off and the original 30 fps mode:** the same comparison between the route at 120 with
  interpolation, at 120 with it off (`--kuf_frame_interpolation=false`) and in the original 30 fps mode: seed and
  objects equal at every tick, the
  camera within 0.1 units, in all pairs (since K43 all exactly equal, 17.1).
- **Render-only builds:** 0 rand() and 0 PhysX calls, 0 restore failures in every run (run-summary).
- **Readers of the slot matrices:** access watchpoints on the view's slot matrices under gdb found only the scene
  build (`sub_821F4D00` callees) and the render thread; the game step reads the camera's own state, not the view.
- **By construction:** the presentation code runs on the game thread after the game step (the tick's frame) or
  between ticks (render-only builds) and writes only the two slot matrices and the field of view of the frame it
  builds; it calls no game function, no physics, no rand().
- **Not changed:** the 30 Hz tick, the input timing (K38), timers, HUD, particles, enemies and world animation,
  transparency, bloom (the frames are drawn as in D3b-1, 14.2). With `--kuf_frame_rate=0` (the default) nothing of
  this runs.

### 15.8 Frame rates, pacing and cost [V]
Timing route, 1280x720, RX 9070 XT, gameplay 60-120 s (profile frame times), interpolation on unless marked:

| --kuf_frame_rate | frames/s | ticks/s | frame time p50 / p95 / p99 (ms) | CPU | GPU busy | resident memory |
|---|---|---|---|---|---|---|
| 0 (30, the original) | 30.0 | 30.00 | 33.30 / 34.57 / 35.42 | 240 % | 31 % | 1,396 MB |
| 60 | 59.9 | 30.00 | 16.65 / 19.27 / 20.05 | 202 % | 48 % | 1,392 MB |
| 120 | 119.6 | 30.00 | 8.35 / 9.43 / 10.56 | 268 % | 82 % | 1,394 MB |
| 120, off | 119.6 | 30.00 | 8.34 / 11.15 / 11.61 | 268 % | 81 % | 1,396 MB |
| 144 | 137.3 | 30.00 | 6.92 / 10.27 / 11.30 | 287 % | 83 % | 1,394 MB |
| 240 | 158.6 | 30.00 | 6.43 / 7.91 / 9.03 | 308 % | 85 % | 1,396 MB |
| 240, off | 159.5 | 30.00 | 6.38 / 8.03 / 8.92 | 308 % | 85 % | 1,401 MB |
| -1 (as many as fit) | 159.5 | 30.00 | 6.42 / 7.92 / 8.76 | 308 % | 85 % | 1,395 MB |
| -1, off | 158.6 | 30.00 | 6.45 / 7.90 / 9.19 | 310 % | 85 % | 1,395 MB |

- Requested and achieved differ above about 160: the render thread and GPU limit (14.5, K39). Interpolation costs no
  measurable frame rate, CPU or GPU (it reads and writes a few matrices per frame).
- Every rate: 0 rand() and 0 PhysX calls in builds, tick intervals p50 33.33 ms, p99 34.9 ms; 4 frames over 40 ms
  per minute at every rate, 30 included (the game's own stall).
- **Alpha:** at 120 the frames of a tick fall at alpha about 0.08 (the tick's own), 0.33, 0.58 and 0.83 (run-summary
  `presentation.alpha_bins`); at 240 and uncapped spread over 0..1 by the achieved rate; no frame held at alpha 1 in
  these runs (`held` 0: no tick was late).
- **Memory:** resident memory flat within each run and equal with interpolation off; the presentation keeps two
  snapshots and a per-slot record (a few kilobytes).

### 15.9 Tests
| Test | What |
|---|---|
| `presentation.*` unit cases (kuf_presentation_tests) | rigid interpolation and its wrap, the game's look-at, the orbit around the pivot (end points exact, aimed, no chord), the turn hint, pull-in, rolled cameras, alpha, every cut rule and backstop |
| `presentation.presentation_120`, `_native_4k`, `_32_9` + `presentation.check` | the route (walk, mouse turn, a 270-degree flick, looking up, recentring) at 120 / 240 at native 4K and 32:9, every frame traced and alpha sweeps captured: on the path between the ticks, aimed, no spin, sweeps step evenly, held frames equal the tick |
| `presentation.isolation`, `interpolated_matches_30`, `disabled_matches_30`, `state_compare_detects` | the game's state per tick (seed, every object, the simulation camera) equal with the presented camera far off, at 120, off and at 30; the comparison catches a game one tick off |
| `gameplay.new_game_120`, `gameplay.combat_loot_120` + `presentation.discontinuities_play` | a new game (the opening's cameras, level loads, the menu and back) and the scripted fight at 120 with interpolation: their own checks, and no frame interpolated across a cut, no uncaught jump, the player kept |
| `camera.*_120` | the seven M6B camera tests at 120 with interpolation: their per-tick checks unchanged |
| `render.frames_complete*` + `frames_check` | (D3b-1) run with interpolation off: a render-only frame redraws its tick exactly |

### 15.10 Remaining issues
- **The rest of the world still moves 30 times per second** (K39): enemies, other characters, objects, particles, the
  character's animation (its bones) and the effect clocks. Next to a smooth camera, other moving characters step at
  30 Hz (D4). Since D4a (section 16) the world's object transforms are interpolated; bones, particles, effects and
  clocks remain (D4b).
- **One tick of delay** for what is shown (15.5): the price of never extrapolating. Full response at 120 is 42 ms
  (57 at 30 fps, 9 without interpolation).
- **Heuristics where the game gives no signal** (K41): the player is the skinned model at the camera's attach point;
  the game's own and event cameras are cut on a broken step (150 units) or, for event cameras, a large move or turn
  (250 units, 0.45 rad); teleports and instant turns of the player by distance and angle. Measured margins: the
  fight's largest player step 77 units (limit 400), turn 63 degrees (limit 149), the original camera's step 28 (limit
  150 change). A shot change that moves the eye less than that, or a teleport shorter than 400 units, would be shown
  as one tick of motion.
- **Previous-frame matrices** (mesh + 228..+ 292, 14.2): a render-only frame repeats its tick frame's, now also for the
  interpolated character; whether the game uses them for anything visible (a motion effect) is not known [?]. No
  difference was seen in the sweeps.
- **Level transitions and loads** were covered by the new game (its level load, the opening's cameras, the menu and
  back) and the fight; other levels' scripted cameras are not [?].
- **`gameplay.controller_gameplay` intermittent** (K42, resolved after D3b-3): a test-script issue, not the game
  or this code (docs/KNOWN_ISSUES.md).
- **Off by default:** `--kuf_frame_rate` is 0 (30 frames per second, the original); interpolation (default on) only
  applies when it is set. Settings integration is D5.

## 16. D4a: world object interpolation (2026-10-26)

Approved: presentation-only interpolation of the world's object transforms (enemies, other characters, moving objects)
between ticks, after K42. Not done, as instructed: skeletal animation, particles, effects and clocks (D4b), settings
integration (D5), optimisation (D6). M6D is not merged. Marks as in section 12.

### 16.1 What the world's objects are [V]
- **The draw list:** each tick lists the renderer objects it draws in its slot's frame packet (renderer + (6782 + slot)
  * 4: object indices into the object array at renderer + 29272). `sub_821A2CA0(renderer, index, matrix)` hands an
  object its world matrix for the slot (vf52: object + 28 + slot * 64) and lists it. The scene build and the render
  thread read that matrix: culling, shadows, skinning (bone x slot matrix x view), the parts' copies.
- **Classes in the tutorial fight** (probe at 30 fps with the state trace, 17,800 ticks; ticks counted while listed):

| Class | What | Indices | Moving | Largest step per tick |
|---|---|---|---|---|
| 0x821010E0 | skinned models: the player, the lizardmen, the other characters | 81 | all | 101 units (a knock-back; p99 15) |
| 0x82101540 | static and dropped models (loot flying out and settling) | 45 | 10 | 46 (p99 44) |
| 0x821013D0 | static and tumbling models (scenery, a thrown object) | 14 | 5 | 23 |
| 0x82103828 | a character's parts (weapon, shield): copy the parent's slot matrix in the scene build; not listed | | | |
| 0x821034C8 | listed without a world matrix of its own (no vf52) | 2 | none | |
| 0x82102A90 | the view (the camera, section 15) | 1 | | |

  The class at an index never changed while the index stayed listed. In the Shadow Depths route (section 15) only the
  player moves.
- **Object lifetime:** a creator (`sub_821A44C8`, `sub_821A4610`, `sub_821A4768`, `sub_821A4970`, `sub_821A4B08`,
  `sub_821A4CA0`) takes the lowest free index (10 and up; `sub_821A4B08` 0-9), constructs the object, stores it in the
  array and writes the index to object + 20. `sub_821A4F90(renderer, index)` releases it: on the render thread at once
  (the object's vf44), on the game thread queued in a per-slot list (renderer + 29276 + slot * 16) until that slot's
  frame has been drawn, then freed on the render thread. So an index is reused, and the allocator often returns the
  same memory: in the fight 46 of 111 reused indices held the new object at the same address and class.

### 16.2 Identity [V]
- **Generation per index:** a hook on `sub_821A4F90` counts the releases of each index (any thread, before the release
  runs). An object is the same across two ticks only with the same index, release count, address and class
  (`ObjectTick`, `ObjectCut` "object"); a renderer-list position is never used.
- **Why it suffices:** a released index can only be reused after its release (the creators look for a free entry;
  the entry is freed after the release). A release between two ticks cuts the object even when the new one has the
  same address and class (the pooled case above). A cross-check counts any address or class change at an index
  without a release (`presentation.objects.unexplained`, a warning per case): **0** in every run (the fight at 60, 120,
  144, 240 and uncapped, the isolation run, the traced runs; 283-640 releases per run, 111 and 115 indices reused in
  the two traced runs).
- **Spawns and despawns:** an object not listed in the tick before (a spawn, a reused index, or an object back from
  being culled out of the list) is shown as written in its first tick ("new": 274-457 per fight run); a despawned
  object is simply no longer listed. An object released while still listed is cut by its release count.
- **The player** keeps D3b-3's rules (15.3): the model D3b-3 last found (same index, release count and address) is
  never a world object here, also in event cameras where D3b-3 does not interpolate it.

### 16.3 Interpolation and discontinuities [V]
- **Per tick (at its scene build):** every listed object except the view, parts, class 0x821034C8 and the player is
  recorded (index, release count, address, class, slot matrix). Each object of tick B is compared with the same index
  in tick A:

| Rule | Signal | Covers |
|---|---|---|
| first, gap, not world, view | as the camera (15.4): no previous tick, a tick without a frame, menus and loads, another renderer | starts, loads, level changes |
| new | not listed in tick A | spawns, index reuse, objects back from culling |
| object | another index, release count, address or class | pool-slot reuse, released objects |
| teleport | more than 400 units in a tick (the fight's largest step 101) | teleports, respawns, scripted placement |
| turn | more than 2.6 rad (149 degrees) in a tick (the fight's largest interpolated turn 45 degrees; one cut in each fight run) | instant turns |
| matrix | not a rotation with per-row scale and position (sheared, reflected, projective, not finite: Compose(Decompose(m)) differs by more than 1e-3 of its scale) | degenerate matrices |

- **Interpolated:** the slot matrix like the player's (position and scale linear, rotation slerp the shorter way), at
  the same alpha as the camera and the player in the same write, so the whole frame shows one moment (N - 1 + alpha).
  Matrices are decomposed once per tick; each frame does one slerp and compose per moving object. An object whose
  matrix did not change is not written (most of the scenery: 410,124 of 578,687 object-ticks in the traced fight).
- **Writing and restoring:** before each scene build (the tick's own and every render-only build) the moving objects'
  matrices for that frame are written into the slot. At the slot's next tick, before anything reads it (before the
  scene build, the game-state trace and the capture), every matrix written two ticks before that the game has not
  written since (still the stored value, same object) gets the game's own value back (`objects.restored`: 3 in the
  traced fight, 125 in the isolation run, which writes every listed object). Nothing is written into the game's
  simulation state: the slot matrices are presentation data the scene build reads.
- **Parent and child:** a character's parts (weapon, shield) copy their parent's slot matrix in the scene build, so
  they follow the interpolated parent. Other attachments between listed objects (one object carried by another) were
  not searched for [?]: each listed object has its own world matrix from the game, and both are interpolated from
  their own ticks at the same alpha, so a rigid attachment stays rigid unless only one of them is cut.
- **Large rotations:** the shorter way, up to the 149-degree backstop; the unit tests cover the wrap at +-180.
- **Physics:** dropped items (rigid bodies) are interpolated like any object from the matrices the game wrote; the
  physics state, collision bodies, AI, combat, damage, pathfinding and scripted events are not touched.

### 16.4 Visual correctness [V]
- **Paths (every object of every frame, the traced fight at 120, `presentation.objects_check`):** 612,484 object
  frames, each on the straight path between its two ticks at the frame's alpha within 0.0154 units (the trace's
  precision), headings within 0.006 degrees for the 99.9 % whose forward is level (characters); 0 frames interpolated
  across a cut or a gap; 0 frames going back along a path.
- **Alpha sweeps in the fight:** two freezes during the lizardmen's attack, each drawn at alpha 0, 0.25, 0.5, 0.75 and 1
  twice: everything moving, then only the world objects (camera and player at alpha 1). Objects alone: 1.735 and
  1.183 levels mean from alpha 0 to 1 in even steps (0.77 0.78 0.78 0.76; 0.53 0.53 0.52 0.50), the middle frame
  between the ends, alpha 1 equal to the full sweep's alpha 1 (0.006 and 0.034 levels mean).
- **Rendering follows by construction:** the matrices are in the slot before the scene build, which derives culling,
  shadow casting and the shadow fit, lights, transparency and depth order, and the parts from them; the render thread
  skins and projects from them. Native 4K and 32:9 runs at 240: 16.8.

### 16.5 Isolation [V]
- **The isolation run (`presentation.objects_isolation` + `_check`):** the fight route with every presented object
  raised by 750 units (the camera by 1,000, the player by 750; `--kuf_presentation_test_offset=1000`, which since D4a
  also writes every listed object every frame): the game's own object positions per tick (the state trace, as the game
  wrote them) never stepped vertically by more than 43.7 units in 697,782 object steps (a taken-in raise would be 750);
  the route played through to its end (155 of 155 script items).
- **Per-tick state against 30 fps on the fight route:** the rand() seed equal at every tick and every paired object's
  position equal (0.0000 units) over the first 600 ticks of gameplay between the traced run at 120, the isolation run
  and the 30 fps run. Not usable as a strict test on this route: the list differs for single ticks by culling (two
  characters for one tick, also between two runs at 120), the new game's opening camera eases by real time (two runs
  at 30 fps differ by 3.4 units, 120 against 30 by 2.8, the isolation run by 15.9, all in the opening's first 250
  ticks and only in pitch, distance and eye, the pivot equal), and the gameplay entry tick can differ (1,864 and 1,877
  in two runs). The strict per-tick tests stay on the Shadow Depths route (15.7), where D4a now also writes every listed
  object with the offset: `presentation.isolation`, `interpolated_matches_30` and `disabled_matches_30` (16.9).
- **Render-only builds:** 0 rand() and 0 PhysX calls in every run. The presentation code calls no game function; its
  one hook (`sub_821A4F90`) only counts before the original runs.

### 16.6 Frame rates and cost [V]
The fight route from a new game, 1280x720, RX 9070 XT, the fight's 100 s (250-350 s of the run), `--kuf_profile`:

| --kuf_frame_rate | objects | frames/s | ticks/s | present interval p50 / p99 (ms) | CPU | GPU busy | resident memory | object write per frame |
|---|---|---|---|---|---|---|---|---|
| 0 (30, the original) | (off) | 30.0 | 30.00 | 33.39 / 36.60 | 243 % | 26 % | 1,377-1,381 MB | |
| 60 | on | 60.0 | 30.00 | 20.64 / 35.05 | 213 % | 38 % | 1,389-1,392 MB | 0.91 us |
| 120 | on | 112.7 | 30.00 | 8.45 / 34.20 | 280 % | 61 % | 1,382-1,385 MB | 0.87 us |
| 120 | off | 115.3 | 30.00 | 8.39 / 34.30 | 287 % | 61 % | 1,387-1,390 MB | |
| 144 | on | 131.8 | 30.00 | 7.54 / 34.09 | 288 % | 68 % | 1,396 MB | 0.77 us |
| 240 | on (two runs) | 115.5, 128.8 | 30.00 | 7.91 / 34.00, 7.29 / 33.99 | 302 %, 301 % | 63 %, 68 % | 1,389-1,391 MB | 0.70, 0.67 us |
| 240 | off (two runs) | 127.1, 116.1 | 30.00 | 7.38 / 34.06, 7.94 / 33.98 | 301 %, 305 % | 68 %, 64 % | 1,382-1,398 MB | |
| -1 (as many as fit) | on | 123.7 | 30.00 | 3.64 / 33.61 | 296 % | 64 % | 1,394-1,395 MB | 0.62 us |

- The fight is heavier than the Shadow Depths route (15.8: 159 frames per second uncapped there): about 115-130 here,
  limited by the render thread and the GPU as before. Runs of the same setting differ by up to 10 % (the fight is not
  the same twice, K24): at 240, objects on gave 115.5 and 128.8, objects off 127.1 and 116.1. Object interpolation
  costs no measurable frame rate, CPU or GPU (`--kuf_frame_interpolation_objects=false` for the comparison).
- The p99 present interval of about 34 ms is there at 30 too (36.6 ms): the game's own stalls (15.8), not the frames
  between ticks.

- Every rate: 30.00 ticks per second, tick interval p50 33.33 ms; 0 rand() and 0 PhysX calls in render-only builds.
- **Cost of the objects:** 0.6-0.9 us per frame for up to 106 listed objects (decide once per tick, one slerp and
  compose per moving object per frame); the per-object trace makes it 15 us. Memory: two tables per object index
  (release counts and the object last listed, 384 KB in all) and per tick about 700 bytes per listed object (two
  snapshots, the plan, the restore records of both slots): about 75 KB at 106 objects. Resident memory flat (1,377 to
  1,398 MB across all runs).
- **Synchronisation:** camera, player and objects are written in the same call with the same alpha; the frame shows
  one game moment. No busy-wait and no extra simulation step were added.

### 16.7 What still moves at 30 Hz (D4b) [V unless marked]
- **Skeletal animation:** the bone palettes (mesh + 856 + slot * 4, model space) are the tick's pose: a walking or
  attacking character glides smoothly but its limbs pose 30 times per second.
- **Particles and effects:** sparks, blood, magic, dust, the effect clocks (63.9 in every frame of a tick) and UV or
  texture animation advance per tick. *(D4b-4, section 20: the effect shaders timed by the frame timer are now drawn at
  the frame's moment; GPU particles, trails and flipbooks remain per tick.)*
- **HUD elements tied to the world** (the 2D list, renderer + 28976 + slot * 16): enemy health bars, item labels and
  markers are placed by the tick from the tick's positions and camera [?]; next to interpolated objects they may lag by
  up to a tick. *(D4b-4A, 20.1: none found in the measured scenes; the HUD is screen-space.)*
- **Objects not in the draw list as objects** (the scene's other buckets, `sub_821A3390` records) are not interpolated
  [?]: whether any moving geometry is drawn that way is not known.
- **Heuristic backstops** (K41): a teleport shorter than 400 units or an instant turn below 149 degrees without
  another signal is shown as one tick of motion.

### 16.8 Native resolution and ultrawide [V]
The traced fight with both alpha sweeps at 240 at native 4K (3840x2160) and 32:9 (5120x1440) (scratch runs, as
`objects_120` with `--kuf_display_size`): every object check passed. 4K: 394,126 object frames on their paths within
0.0150 units, 0 across a cut; objects-only sweeps 1.781 and 1.380 levels mean in even steps, alpha 1 equal to the full
sweep (0.008, 0.022). 32:9: 441,994 object frames within 0.0158 units, 0 across a cut; sweeps 0.894 and 0.427 in even
steps (0.032, 0.044). Identity unexplained 0 in both (370 and 395 releases).

### 16.9 Tests
| Test | What |
|---|---|
| `presentation.*` unit cases (kuf_presentation_tests) | D4a: the same object moving is interpolated (end points, the middle, the wrap), another object at the same index never (release count, address, class, index), teleports, turns, sheared, reflected, projective and non-finite matrices cut |
| `presentation.objects_120` + `presentation.objects_check` | the tutorial fight at 120 from a new game, every object of every tick and frame traced: identity explained, no frame across a cut, every object on its path at its alpha, no step over the backstop, no motion back; two freezes with alpha sweeps of everything and of the objects alone |
| `presentation.objects_isolation` + `presentation.objects_isolation_check` | the same route with every presented object raised by 750 units: the game's own object positions never take it |
| `presentation.isolation`, `interpolated_matches_30`, `disabled_matches_30` | (15.9) now with D4a writing every listed object: the game's state per tick equal |
| `gameplay.combat_loot_120`, `gameplay.new_game_120` | (15.9) the fight and the new game at 120 with the objects interpolated: their own checks |

Shown to catch faults (scratch): the path check on a copy of the traced run with each frame's alpha moved by 0.1 (9.9
units off the path: FAIL); the isolation check on a copy with one object's game position raised by 750 units for one
tick (FAIL).

## 17. D4b: K43 and skeletal animation (2026-10-27)

### 17.1 K43: the camera timed by the host clock made runs differ [V]
- **What failed:** once, in the D4a full regression, `interpolated_matches_30` and `disabled_matches_30`: the 30 fps
  reference run's player one 0.01 log step off at 6 of 600 ticks.
- **Source:** `ElapsedSeconds()` in `modern_camera.cpp`, the M6B camera's dt: the host clock (`steady_clock`) between
  two updates of the free camera (`sub_825232B8`, once per game tick in gameplay: 2,821 of 2,821 ticks). With the fixed
  tick the ticks keep their schedule on average, but each tick's start moves with the host's load: measured 5 to 111 ms
  between updates with four runs at once. The dt drives the stick turn, the mouse fed to the game's own cameras as a
  stick deflection, `LimitUpward`, the idle timer (recentring, the height-follow bypass), the distance ease and the
  obstruction release. The camera is part of the simulation (the player walks relative to its heading), so the
  jitter reached the game: four runs of the same build at once, compared at 6 decimals, all differed: the view by up to
  0.29 units from 7 ticks into gameplay, the player by up to 0.00018 units from about 320 ticks (below the old trace's
  0.01, which it crossed only by rounding, hence once in many runs). It affected gameplay, not only the comparison,
  though by a fraction of a millimetre over 600 ticks. Nothing else timed by the host reached the traced state on this
  route: with the camera's dt fixed the runs were identical (below).
- **Experiment:** the same build with the camera's dt fixed at 1/30 s: four runs at once identical at 6 decimals
  (every object, the view and the camera trace) over 600 ticks.
- **Correction:** with the fixed tick (`--kuf_fixed_tick`, the default) the camera's dt is one tick (1/30 s): what
  the update stands for, as the game's own state advances. The M6B camera's rates and easing are per second as before
  (30 ticks a second on average); only the per-tick jitter is gone. The original pacing (`--kuf_fixed_tick=false`,
  one tick per presented frame) keeps the host clock. `--kuf_camera_host_dt` (debug) restores the old timing.
- **Comparison:** the state trace now logs positions to 6 decimals and `state_compare.py` requires them exactly equal,
  the camera too (it allowed 0.25 units). After the correction: the route at 30 fps twice, at 120 with and without
  interpolation and with the isolation offset, six runs at once, identical in every pair. The new
  `presentation.state_compare_detects_host_dt` runs the route with the old timing and requires the objects check to
  fail (the player 0.000062 units off, at 367 ticks): a difference the old 0.01 comparison let through.

### 17.2 Skeletal animation: how the game poses a character [V unless marked]
Runtime probes in the fight and on the walk route (scratch, removed) and the recompiled code:

| What | Where |
|---|---|
| Skinned models | renderer objects of class 0x821010E0 (the player, the lizardmen, the other characters; up to 54 at once in the fight, 21 to 110 bones). Skeleton at mesh + 700 (+ 12 bone count, + 20 the bones; a bone's + 892 is its parent's index relative to its own, 0 for a root); bone count also at mesh + 696 |
| The animation's state | per model: base layers (count mesh + 708, clip + 712, time + 732) and up to 5 blend layers (count + 772, clip + 776, time + 796, weight + 816). Each layer's time advances one frame per tick (30 a second) in the tick's update; a looping clip wraps (80 to 0, 54 to 0, 19 to 0) |
| Transitions | a crossfade: the old clip moves to a blend layer whose weight falls by 1/7 per tick (6/7 ... 2/7) while the base layer plays the new one from frame 0 or 1. The base clips change without a blend layer only with a fade already running (bookkeeping), never alone in the routes traced |
| Where the final bone matrices are made | in the tick's **scene build**: culling (`sub_821A30D8`, from `sub_821F4D00` and `sub_821F49E8` -> `sub_821F5270`) calls each visible model's vf96 (`sub_8219CCB8`), which evaluates its pose (`sub_8219C8A8(mesh, flag)`) from the layers at their current times into the **palette**: one 4x4 float matrix per bone (64 bytes), **model space**, root motion masked (mesh + 872). Game thread, once per tick per model drawn |
| Storage | the palette is a block of the slot's frame memory (arena 0x82AE81FC + slot * 48, `sub_8219A9B8`: pure allocation, bone count x 64 bytes), pointer at mesh + 856 + slot * 4. A pose cache shares one palette between models with the same clip and frame (5,149 pointers shared in the fight). The scene build's first part clears the pointers (vf200, `sub_8219E278`) |
| Skinning | on the render thread: invBind x palette bone x (object slot matrix x view); its output is mesh + 864 + slot * 4. Shadow passes skin from + 856 again per pass |
| Root transform | the object's slot matrix (mesh + 28 + slot * 64): D4a interpolates it (16.3) |
| Weapons and equipment ("sockets") | mesh + 624 (12-byte entries: bone, the child's object index, an optional offset matrix): vf96 places each child right after the pose, child matrix = [offset x] palette[bone] x the parent's slot matrix, then evaluates the child. Children of class 0x821013D0 (rigid meshes, weapons): 5,421 socket placements checked |
| Physics-driven parts | mesh + 1028 (object indices): each gets the parent's slot matrix, then its own vf96. In the fight one character (61 bones) has one: class 0x82103828, 21 bones, its palette made by the physics step (the physics manager's pre-simulate updates `sub_821C39E8` / `sub_821C45C0`, which read the parent's palette for the bones it hangs from) [L for the physics side] |
| Who reads the palettes | the recompiled code indexes mesh + 856 by a slot in 16 functions besides the allocator: on the game thread vf96 and `sub_8219BAD8` (called from vf96: the attachments), the bone queries (vf172 `sub_8219D648`, messages 520 by name and 526 by index), the physics step's parts (`sub_821C39E8`, `sub_821C45C0`, `sub_821CA680`) and three not seen running (`sub_821B5470`, `sub_821B9C98`, `sub_82567788`); on the render thread, by the render slot, `sub_8219B020`, `sub_8219E030`, `sub_82234608` (the skinning and the shadow passes). Runtime counts in 17.6 |
| Ragdolls | [?] none identified: no character's palette was written outside the scene build in the traced routes (17.6); whether a death elsewhere hands a character to physics is not known |
| Reuse and release | a model's palette pointer changes every tick (new frame memory or the cache's); models are released through `sub_821A4F90` (the object index's release count, 16.2) |

Section 13.3's table (and a comment in `render_frames.cpp`, now corrected) said the palettes are written by the
tick's physics step. That holds for the physics-driven parts only; the characters' palettes are evaluated in the scene
build. D3b-1's handling is unaffected (a build keeps the tick's palettes either way).

### 17.3 Interpolation [V]
- **When:** the tick's scene build evaluates each model's pose (the game's own code, untouched); right after
  `sub_8219C8A8` returns to vf96, before vf96 places the weapons, the pose is captured (palette, layers, parents) and
  the frames' plan decided once per tick. In the tick's own frame and in each render-only build vf96 runs again (the
  build re-evaluates at the same layer times: the same pose), and the hook then gives the model **a palette of its
  own** from the game's allocator (the slot's frame memory, released with the frame) holding the pose at the frame's
  alpha, and points mesh + 856 + slot * 4 at it. The game's palette is never written (it may be the cache's, shared).
  vf96 then places the weapons from the new palette; the culling, the shadow passes and the skinning all read it.
- **The game's other readers:** the physics step's parts read their character's palette for the game slot during the
  tick, after the tick's scene build has pointed it at the palette written for the tick's own frame (17.6). The
  readers outside the posing (the physics step's three and the three not seen running) are wrapped: for the call the
  slot's written palettes are set back to the game's, then forward again.
- **Undoing:** when the next tick into the slot starts (`render_frames::OnTickStart`: every frame of that slot drawn,
  no game code of the tick run yet) each written pointer gets the tick's own back, when the object is still the same
  (object index, release count, address).
- **How:** each bone's local transform = its model-space matrix x inverse(its parent's), when the skeleton's parents
  are known and every matrix is rigid; rotation slerped the shorter way, translation and scale lerped; then composed
  back down the hierarchy. Interpolating in model space would shorten limbs on the chord; in parent space bone lengths
  stay (largest error 0.007 units, only where the animation itself moves a bone's offset is the length free). Root
  motion is masked in the palette; the model's root is its object matrix (D4a), interpolated with the same alpha.
- **Endpoints:** alpha 0 and 1 give the ticks' palettes exactly (unit tests); at alpha 1 nothing is written (the
  tick's palette is drawn).
- **The backstop per bone:** a bone that turns by more than 2.6 rad (149 degrees) in its parent's space in one tick
  (way round unknown) is shown at the new tick's local rotation, the rest of the pose interpolated. The fight's
  largest interpolated turn was 2.53 rad; 4 bones held in 267,768 model-ticks.
- **Synchronisation:** one alpha per frame for the camera, the player, the objects (D4a) and the poses; the poses
  are written in the same build as the objects' matrices.
- `--kuf_frame_interpolation_bones` (on): off shows each tick's pose in the frames between ticks (D4a's behaviour).
  `--kuf_trace_presentation_bones` (debug): every pose written (alpha, progress, length error) and every cut or
  layer change.

### 17.4 Transitions and discontinuities [V unless marked]
The decision uses the game's own state, not distances:

| Case | Rule | Fight at 120 (267,768 model-ticks) |
|---|---|---|
| Steady animation, loops (80 to 0 ...) | interpolated: the palettes are the poses, so a wrap is the clip's own last-to-first frame | |
| Walk/run/attack/dodge/knockback/death transitions | the game crossfades (a blend layer fading the old clip): interpolated between the two ticks' blended poses, which is what the crossfade shows | 2,704 clip changes with a fade, 2,897 fade steps ending or starting, all interpolated |
| Animation switched without a crossfade | the base clips differ and neither tick has a blend layer: cut ("snap") | 0 |
| Model new, or its pose missing at the tick before (spawn, culled the tick before, a level load) | cut ("new") | 111 |
| The model's root cut by D4a or D3b-3 (teleport, instant turn, a matrix not rigid, another object at the index, the camera's cuts for the player) | the pose cut with it | 1 ("turn") |
| Another skeleton, bone count or hierarchy (model change) | cut ("skeleton") | 0 |
| A bone that does not decompose | cut ("matrix") | 0 |
| Not in the object list (unlisted) | not written | 0 |
| A bone turning over 2.6 rad | that bone held at the new tick's (17.3) | 4 bones |

- A cut shows the new tick's pose in every frame of the interval (one tick at 30 Hz, as before D4b).
- Resets, level transitions and loading: the model's root or the presentation's interval is cut there (15.4, 16.3),
  and a model released and replaced at an index is never paired (identity, 16.2).
- [?] Ragdolls: none in the traced routes (17.2); a physics-posed character would be a model whose palette the
  physics step writes; its parts are D4b-3 (17.5).

### 17.5 Attachments, shadows and the rest of the frame [V unless marked]
- **Weapons and equipment:** vf96 places each socket child from the palette right after the pose; with the written
  palette in place they follow the interpolated hand. Probe in the fight (ticks 6900-7000 at 120, scratch): 5,421
  socket placements in render-only builds, each child's matrix equal to [offset x] the written palette's bone x the
  parent's interpolated slot matrix within 0.00096 (float rounding); every socket child was a rigid mesh (class
  0x821013D0). Hands and grips cannot separate: the weapon is placed from the same bone matrix that skins the hand.
- **The root:** the palette is model space; the model's slot matrix (D4a) is interpolated with the same alpha in the
  same build, so pose and root show one moment.
- **Physics-driven parts (K45, D4b-3):** one character in the fight (61 bones) has one: a 21-bone part of class
  0x82103828 whose palette the physics step makes from the bones it hangs from. It gets the parent's interpolated slot
  matrix, but its own pose stays the tick's (the physics step now reads the game's palettes, 17.6), so between ticks
  it can lag the parent's moving bones by up to a tick of their motion [not measured]. Posing it from the presented
  bones needs its palette layout and is left to D4b-3. **Done in D4b-3 (section 19).**
- **Shadows, lighting, transparency:** the shadow passes skin from mesh + 856 per pass, so they use the written pose
  as the lit pass does (the sweeps in 17.8 are of the lit, shadowed frame).
- **Culling:** the culling calls vf96 for the models it keeps, so what is culled is decided before the pose, from the
  model's root and bounds as in the tick [?: a limb crossing a view edge between ticks is culled as in the tick].
- **HUD:** world-anchored HUD elements are not interpolated (16.7, K39), as approved.

### 17.6 Simulation isolation [V]
- **What changes:** the pointer at mesh + 856 + slot * 4, from the tick's scene build (the tick's own frame) or a
  render-only build until the next tick into the slot starts. The game's palette is never written; the written one is
  frame memory from the game's own allocator, released with the slot's frame. No animation code runs in a
  render-only build that does not run in a tick's scene build: the builds re-evaluate the poses at unchanged layer
  times (the layers advance in the tick's update, not in the scene build); 0 rand() and 0 PhysX calls in every build.
- **Who could read a written palette (the fight at 120, a probe hooking every function that indexes the palette by
  a slot; scratch, removed):** on the game thread outside render-only builds, vf96's attachments (`sub_8219BAD8`,
  252,605 calls with a written palette installed: the frame's own posing, as intended), the bone queries 0 calls, the
  physics step's three readers 27,288 calls; `sub_821B5470`, `sub_821B9C98`, `sub_82567788` 0 calls. The render-slot
  readers ran only on the render thread (4,664,231, 898,236 and 30,269 calls, 0 on the game thread).
- **Found and corrected: the physics step read the tick frame's palette.** Before the correction the physics step's
  readers found a written palette installed in 8,125 of 8,366 calls on one run: its parts were driven by the
  interpolated pose of the tick's own frame, whose alpha depends on the host's timing. They run in the tick after its
  poses are evaluated and before its scene build ends (`sub_821F4D00`), in the tick's own slot. Now they are wrapped (17.3): 27,288 calls, 0 finding a written palette
  (run-summary `bones.physics_reads_written`, required 0 by `pose_check.py`). The Shadow Depths comparisons could not
  have caught it: no character there has a physics part, and the fight from a new game is not comparable tick by
  tick (16.5).
- **Per-tick state against 30 fps (Shadow Depths, `presentation.isolation`, `interpolated_matches_30`,
  `disabled_matches_30`):** with the poses written (the player, 69 bones) and, in the isolation run, every written
  bone raised by a quarter of the 400-unit offset: the seed and every object exactly equal (6 decimals) and the
  simulation camera exactly equal over 600 ticks.
- **The fight's isolation run (`presentation.objects_isolation`):** every written bone raised by 250 units: the game's
  own object positions never stepped by more than the 250-unit limit vertically, the route played to its end, and
  `presentation.pose_isolation_check` requires the game's bone queries and the physics readers never to find a
  written palette.
- **Not touched:** hitboxes and collision bodies (the game's own palettes and object matrices), animation state
  (layers, times, events: read only), combat timing, AI, save data.

### 17.7 Frame rates and cost [V]
The fight route from a new game, 1280x720, RX 9070 XT, the 250-350 s window (as 16.6), `--kuf_profile`, one run each.
The 120 and -1 rows were measured on the final build (with the physics-reader correction of 17.6); the others before
it:

| --kuf_frame_rate | poses | frames/s | ticks/s | present p50 / p99 (ms) | CPU | GPU busy | resident memory | pose write per frame |
|---|---|---|---|---|---|---|---|---|
| 0 (30, the original) | (off) | 30.0 | 30.00 | 33.47 / 37.49 | 243 % | 34 % | 1,392-1,394 MB | |
| 60 | on (3 runs) | 30.0 (locked, K44) | 30.00 | 32.44 / 36.43 | 174 % | 32 % | 1,388 MB | 90 us |
| 60 | off | 30.0 (locked from 240 s, K44) | 30.00 | | | | | |
| 120 | on | 115.2 | 30.00 | 8.81 / 34.42 | 288 % | 65 % | 1,385-1,388 MB | 113 us |
| 120 | off | 90.0 (locked, K44) | 30.00 | 9.53 / 34.59 | 250 % | 54 % | 1,384-1,388 MB | |
| 144 | on | 137.7 | 30.00 | 7.04 / 34.11 | 254 % | 62 % | 1,398 MB | 80 us |
| 240 | on | 133.4 | 30.00 | 6.57 / 34.05 | 301 % | 77 % | 1,385-1,386 MB | 87 us |
| 240 | off | 109.6 | 30.00 | 7.97 / 34.09 | 302 % | 72 % | 1,387-1,389 MB | |
| -1 (as many as fit) | on | 114.7 | 30.00 | 3.46 / 33.70 | 297 % | 64 % | 1,397-1,398 MB | 82 us |
| -1 | off | 129.0 | 30.00 | 3.87 / 33.71 | 298 % | 69 % | 1,392-1,396 MB | |

- **60 did not reach 60** (before the K44 correction, section 18) in any of three runs (two with the poses on, one off): after 80-240 s every frame between
  ticks was skipped as late for the rest of the run (K44, not caused by D4b; D4a's run had reached 60.0). Before the
  lock: 60.0. At 120 the frame rate sits at exactly 90 the same way: before the correction for 110-280 s (poses on)
  and 110-220 s (off); on the final build for 190-250 s (on, recovering to 114-120 after it) and from 110 s to the end
  (off, so its window mean is 90.0).
- **Poses cost no measurable frame rate:** on against off is within the route's run-to-run spread (before the correction
  110.4 against 112.6 at 120, 133.4 against 109.6 at 240, 130.6 against 110.0 uncapped; on the final build 115.2
  against 90.0 at 120, the off run locked by K44, and 114.7 against 129.0 uncapped; the fight differs between runs,
  K24). Neither direction holds across runs; a difference this small cannot be separated from that spread.
- **Cost of the poses:** 75-113 us per frame for up to 55 models and 110 bones (19 to 72 million bones written per
  run); per palette at most 0.15 ms, with rare single writes of 1.1-1.5 ms (the host preempting the game thread). The
  per-tick decision (capture, local transforms) is in the tick's scene build: at 120 its mean per 900 builds was
  2.3-4.3 ms in the fight with the poses on against 1.9-2.8 ms off.
- **Memory:** per model the captured poses of two ticks (64 bytes per bone each) and the decomposed pair (88 bytes
  per bone each), about 300 bytes per bone: at most 1.8 MB at 55 models of 110 bones (most have fewer); the written palettes are the game's frame memory, released with
  the frame (0 allocation failures). Resident memory flat (1,385-1,398 MB in all runs).
- **Every rate:** 30.00 ticks per second, tick interval p50 33.33 ms; 0 rand() and 0 PhysX calls in render-only builds;
  latency unchanged (full response p50 41-44 ms). On the final build 27,477 (120) and 27,285 (uncapped) physics
  reads with the poses on, 0 finding a written palette.

### 17.8 Visual correctness [V]
- **Every pose written (the traced fight at 120, `presentation.pose_check`):** 651,854 palettes written between ticks,
  all in parent space; each frame's pose at its alpha (progress recovered from the palette within 0.0001 of the
  frame's alpha); 0 going back between frames; 0 written in an interval where the pose was cut; bone lengths kept
  within 0.0074 units where the animation keeps a bone's offset.
- **Sweeps of the poses alone** (camera, player root and objects at alpha 1; `<label>_b000 ... _b100`): fight_a 1.232
  levels mean from alpha 0 to 1 in even steps (0.512 0.516 0.500 0.500), fight_b 2.069 (0.926 0.911 0.929 0.897), the
  middle frame between the ends, alpha 1 equal to the full sweep's alpha 1 (0.009 and 0.005 levels mean). Intermediate
  poses differ; the endpoints are the ticks' poses.
- **Native 4K and 32:9 (the walk route at 120, `presentation.pose_check_walk`):** 2,497 and 2,671 poses written, each
  at its alpha within 0.0001, lengths within 0.0043; sweeps of the player's pose alone 0.092 and 0.049 levels mean in
  even steps, alpha 1 equal to the full sweep's (0.003, 0.004).
- **Camera, player and world in sync:** one alpha per frame for all (17.3).

### 17.9 Tests
| Test | What |
|---|---|
| `presentation.poses` unit cases | parent-space interpolation (ends exact, half the angle at the middle, lengths kept, no chord, the progress recovered), cuts (another skeleton, a switch without a crossfade, a fade ending, a loop, a held bone, a shear, the model-space fallback) |
| `presentation.bones_120` + `presentation.pose_check` | the tutorial fight at 120 from a new game with every pose cut and written traced and two freezes sweeping the poses alone (17.8) |
| `presentation.pose_check_walk` | the walk route at 120, native 4K and 32:9 (17.8) |
| `presentation.pose_isolation_check` | the fight's isolation run (every written bone 250 units off): palettes allocated, the game's bone queries and physics readers never finding a written palette |
| `presentation.isolation`, `interpolated_matches_30`, `disabled_matches_30` | (15.9, 17.1) now with the player's pose written |

Shown to catch faults: `pose_check.py` on a copy of the isolation run's summary with `physics_reads_written` set to 3
(FAIL). The motion check failed during development on a pose written at the wrong alpha (the first model-space
progress metric) and the unit tests on a wrongly built held bone (both corrected, 17.3).

### 17.10 Remaining (D4b-3, D4b-4) [V unless marked]
- Physics-driven parts posed from the presented bones (K45): done in D4b-3 (section 19).
- Particles, effects, the effect clocks, texture and UV animation (16.7).
- World-anchored HUD elements (16.7; not started, as approved).
- The pacing lock at lower multiples of 30 (K44, D3b-1's presentation clock): not D4b, but it limits 60 and 120.
- [?] Ragdolls and physics-posed deaths on other levels; a level change during a crossfade (cut by the level's own
  signals, 15.4, but not traced with poses).

## 18. K44: the presentation clock after a late tick (2026-10-27)

### 18.1 Root cause [V]
The frames between ticks follow a presentation clock (`AfterTick`, frame_pacing.cpp): steps of 1 / rate, the tick's
own frame taking the first step from the tick's start. A step is built only if the build can end before the next
tick is due (`max(step, now) + cost > due`: cost the build estimate plus 0.5 ms, about 1.8 ms in play); otherwise it
was counted late and skipped, and the clock went on. The clock kept its phase from tick to tick and only restarted at
a tick's start when it was behind it. One tick starting late by nearly a frame step (the host's load) restarted it
at that late start; from then on the step before every next tick fell into the last ~1.8 ms before it, was skipped
as late, and the clock left that tick at the same phase again. The rate's period divides the tick (2 or 4 steps per
33.33 ms; the integer periods drift by 4 ns per 3 ticks), so nothing moved the phase out: 60 locked at 30, 120 at 90,
until another late tick happened to restart the clock elsewhere. Rates that do not divide the tick (144) move the
phase every tick and do not lock.

It is the scheduler alone: the builds measured 1-3 ms, the render thread was idle (no "render busy" drops in the
locked ticks), the tick rate stayed 30.00, and it reproduces offscreen without any display or present mode. The
earlier Python model of the loop did not include the restart at a late tick's start, so it never entered the phase.

Measured with `--kuf_trace_frame_clock` (one line per tick: the start against the schedule, the clock's phase, each
step and what became of it) and one tick held on purpose (`--kuf_debug_tick_stall_at` / `_us`) on the timing route,
the old clock (`--kuf_present_clock_keep_late_phase`):

| rate | before the held tick | the held tick (+16.1 ms / +7.7 ms late) | every traced tick after it (848) |
|---|---|---|---|
| 60 | phase +8.6 ms, 2 frames | restart at the late start; its step +16.67 late | phase +16.07 ms, step +32.74 late: 1 frame (848 of 848) |
| 120 | already locked: phase +7.4 ms, step +32.4 late, 3 frames | restart; step +25.00 late | phase +7.67 ms, step +32.67 late: 3 frames (847 of 848) |

The 120 run had locked on its own before the held tick, as K44's sessions did.

### 18.2 Correction [V]
A step too late to build before the next tick (and the steps after it) goes to the next tick's own frame: the clock
stays at that step, behind the next tick's start, which restarts it from that start. The steps are still counted
late (they are frames not drawn), but the phase cannot outlive the late tick. Nothing else changed: the same safety
check before each build, no catching up, no extra ticks, no waiting for a frame (the game thread sleeps until each
step as before; no busy wait). `--kuf_present_clock_keep_late_phase` (debug) restores the old clock for the fault
test. New in the run summary: `frames_per_tick` (frames per tick interval, the tick's own included), `late_ticks`
(ticks that dropped a step as late), `clock_restarts`.

With the correction, the same held tick: 60 recovers in the next tick (2 frames in 848 of 848 traced ticks, 0 late),
120 likewise (4 frames in 836 of 848, 0 late; 12 dropped a step because the render thread was still drawing).

### 18.3 Frame rates [V]
The fight route from a new game (16.6, 17.7), 1280x720 offscreen, skeletal poses on, `--kuf_profile`, one run each on
the final build; the 250-350 s window; frame times from the profile's per-frame times:

| requested | achieved | ticks/s | frame time p50 / p95 / p99 (ms) | CPU | GPU busy | memory | late ticks (whole run) | pose write per frame |
|---|---|---|---|---|---|---|---|---|
| 30 (off) | 30.0 | 30.00 | 33.42 / 34.59 / 35.38 | 238 % | 25 % | 1,381-1,382 MB | - | - |
| 60 | 60.0 | 30.00 | 16.71 / 20.04 / 20.82 | 232 % | 42 % | 1,381-1,382 MB | 17 of 10,691 | 145 us |
| 90 | 89.8 | 30.00 | 11.01 / 13.63 / 14.32 | 248 % | 51 % | 1,390-1,394 MB | 20 of 10,691 | 117 us |
| 120 | 119.4 | 30.00 | 8.37 / 11.45 / 11.94 | 279 % | 59 % | 1,385-1,386 MB | 24 of 10,693 | 107 us |
| 144 | 120.3 | 30.00 | 8.05 / 11.13 / 13.69 | 297 % | 64 % | 1,379-1,382 MB | 1,924 of 10,692 | 120 us |
| 240 | 105.4 | 30.00 | 9.16 / 13.38 / 14.36 | 299 % | 58 % | 1,379-1,383 MB | 35 of 10,691 | 103 us |
| -1 (as many as fit) | 115.8 | 30.00 | 8.40 / 10.60 / 13.80 | 299 % | 63 % | 1,382-1,384 MB | 0 | 81 us |

- **60, 90 and 120 hold their rate** for the whole run outside loading (10 s windows from 50 s, after the loading at 30-50 s: 60
  every window but one at 59; 90: 88-90; 120: 115-120 except 90-105 in the heaviest part of the fight at 190-230 s,
  where the uncapped run also falls to 93-102). Before K44 every 60 run locked at 30 within 80-240 s.
- **144, 240 and uncapped are limited by this machine in the fight** (RX 9070 XT, the game's render thread): 144 and
  240 reach their rate before the fight (144, 240) and 90-145 in it; the dropped frames are "render busy" (the render
  thread still drawing the frame before: 9,378 at 144, 35,715 at 240), not late. At 144 the period does not divide the
  tick, so a step lands in the last ~1.8 ms before a tick in about one tick of five (1,924 late ticks); those frames
  cannot be built in time and go to the tick's own frame.
- **Latency** unchanged: tick to present p50 10.3-11.8 ms with frames between ticks (47.1 at 30), full response p50
  43.6-45.1 ms (47.1 at 30), state age p50 36-41 ms.
- **Frame-time spread at 60:** p50 16.7 ms but p95 20 ms: the tick's own frame reaches the render thread after the
  tick's update and scene build (about 4-8 ms after the tick's start), the frame between ticks on its step, so the
  intervals alternate around 16.7 ms. The interpolation places each frame by when it is handed over (`FrameAlpha`), so
  motion stays even; the present times are not (not changed by K44; noted in 18.5).
- **Synchronisation overhead:** the game thread waits only for steps and for the render thread to be idle; 0 rand()
  and 0 PhysX calls in builds, 30.00 ticks per second at every rate.

### 18.4 Tests [V]
`timing.k44_60` / `k44_120` (one tick held 16.0 / 7.6 ms at tick 2100 on the timing route, the clock traced for 900
ticks) with `k44_check.py`: at least 95 % of the ticks after it draw all rate / 30 frames, at most 2 % drop a step as
late, at most 5 % a step for a busy render thread, 30 ticks per second. `timing.k44_60_fault` / `k44_120_fault`: the
same with the old clock, and the check requires the lock (at least 80 % of the ticks one frame short; measured 848 of
848 and 847 of 848): the tests catch the fault. `timing.k44_long_60`: six minutes at 60 through the tutorial fight with
no tick held, at most 2 % late ticks and no 30 s at 30 frames per second (measured: 21 of 10,693; every window from
20 s at 60; the same check fails the D4b-2 run that sat at 90 for minutes at 120). Each stall test passed in three
runs.

Windowed at 120 on a 3840x2160 120 Hz display (borderless, Render Native: 4K rendering), the timing route, VSync on
(FIFO only: `--vulkan_allow_present_mode_immediate=false --vulkan_allow_present_mode_mailbox=false`) and off
(immediate): 67-69 and 68-70 frames per second in play, 30.00 ticks per second, 20 / 22 late ticks in 4,000; the
frames per tick vary (2, 3, 4: no lock); the limit is the render thread at 4K ("render busy" 7,029 / 6,926), as in
the 120 fps play session (57-78).

### 18.5 Remaining
- The frame-time spread at 60 (18.3): the tick's own frame is handed over later than its step. Moving the build
  steps to match it is a pacing refinement, not part of K44.
- 144 and 240 are limited by the render thread on this machine in the fight.
- At native 4K the render thread limits 120 to about 68 on this machine, with VSync on or off.

## 19. D4b-3: physics-driven parts (K45, 2026-10-28)

### 19.1 How a part gets its pose [V unless marked]
Found with a probe on the physics step's part functions and the part's own virtual functions (scratch, removed):
- **Objects:** a part controller (class 0x821014C4, the `r3` of the physics step's `sub_821C39E8` and
  `sub_821C45C0`) holds the part (controller + 88: class 0x82103828, derived from the skinned model class
  0x821010E0) and the model it hangs from (controller + 92). The part has bones (mesh + 696) but no skeleton
  (mesh + 700 = 0): no hierarchy, no animation layers. Its vf96 is `sub_822345E0`, its render-thread skinning
  (vf124) `sub_82234608`. The model lists the part's object index in its list at mesh + 1028.
- **Order in a tick:** the scene build calls the part's vf96 with its palette pointer for the slot still 0; the
  physics step (`sub_821C39E8`) allocates the slot's palette (the game's `sub_8219A9B8`, frame memory, as for a
  model) and `sub_821C45C0` writes the simulated pose into it, in the model's space (as the model's own palette),
  each part bone a full matrix without a parent. The physics step reads the model's palette for the slot to do so
  (17.6: it now always gets the game's own).
- **In a render-only build:** nothing poses the part. Its vf96 runs from the model's (`lr` 0x8219D20C) and the
  render thread skins it from whatever its slot pointer holds: the tick's palette. Its root is the model's slot
  matrix, so it already followed the model's interpolated root (D4a); its bones stayed the tick's.
- **Consumers:** the render thread's skinning and the shadow passes (mesh + 856 per slot, as for models, 17.5); the
  physics step writes it. [?] Whether anything else reads a part's palette on the game thread was not probed
  separately; the part's pointer holds a written palette from right after its physics in the tick (the tick's own
  frame) until the next tick starts, and the physics readers' check covers the parts' written palettes too (19.3).
- **Which characters have them:** the fight's player (Kendal, 61 bones): one 21-bone part (the tabard); the walk
  route's character (Celine, the Shadow Depths save, 69 bones): four parts of 5, 9, 10 and 36 bones; on the menu screens
  before the fight (ticks 1711-1801, probably the character select): a 108-bone model with a 63-bone part. The
  lizardmen have none. [?] Other levels and characters not surveyed; any part of this class is handled the same way.

### 19.2 Interpolation [V]
A part bone's matrix is the physics result for the tick, in the model's space. Interpolating it there would drag it
on a straight line between the ticks while the model's bones it hangs from turn on arcs: the part would separate
from the body between ticks. Instead (`PreparePart`, `InterpolatePart`, presentation_math.cpp):
- **Anchors:** each part bone is taken relative to the bone it moved with most rigidly over the tick: among the
  model's bones and the part's own earlier bones (a chain hangs link by link), the one whose relation to it changed
  least (largest element), if that relation is a rigid transform at both ticks (a model bone scaled unevenly would
  shear it) and changed less than the part bone did in the model's space. Otherwise (a body moving on its own) the
  bone is unanchored and interpolated in the model's space.
- **Between the ticks:** the relation's rotation the shorter way (slerp), its scale linearly, its offset from the
  anchor by direction (slerp) and distance (linearly: a bone swinging about its anchor moves on the arc, not the
  chord); then placed on the anchor as presented in the same frame (the model's written palette, or the part bone
  already placed). So the part follows the interpolated model exactly, and its own swing is interpolated between the
  two physics results. The ends are the ticks' own palettes.
- **Held:** a relation turned by more than `kBoneTurn` (2.6 rad) in one tick is shown at tick b's (as for model
  bones, 17.3).
- **Cut (the tick's palette shown, written only at the tick):** `unlisted` (not in the model's list), `new` (no
  previous tick, another model, or another object at the same address: the model's index and generation, as D4a),
  `model` (the model's own pose not interpolated: cut, new, or no previous tick), `matrix` (no valid pair).
- **Written:** in the same build right after the model's pose (the model's `sub_8219C8A8` hook), into a palette of
  the part's own from the game's allocator (the slot's frame memory, as a model's, 17.6), its pointer set back with
  the models' (`g_poses_written`). Before each render-only build every part's pointer is set to the tick's palette:
  the build reuses the frame memory a palette written for an earlier frame was taken from (found by the objects
  sweep, 19.5).

During development: the first version (relations to model bones only) cut 5,193 of 7,753 part-ticks on sheared
relations, and with those allowed stretched a hanging bone by up to 68 units between ticks (the chord of a swing);
rigid anchors, chain anchors and the arc brought the largest stretch to 0.034, then 0.014.

### 19.3 Physics isolation [V]
- **The physics step never reads a written palette:** the part's own palette is written only into the build's
  frame memory and its pointer set back before the next tick's physics (the models' mechanism, 17.6); the model
  palettes the physics step reads are the game's (`WithGamePalettes`, D4b-2, unchanged). The fight at 120:
  23,598 physics reads, 0 finding a written palette (`bones.physics_reads_written`, `pose_check.py` requires 0); the
  isolation run (every written bone, model's and part's, 250 units off): 0.
- **Shown to catch the fault:** `--kuf_debug_physics_sees_presented` (debug) skips the restore for the physics
  step's readers; `presentation.bones_physics_fault` runs the fight with it and
  `presentation.bones_physics_fault_check` (`--expect-physics-fault`) requires the readers to see a written palette:
  27,048 of 27,291 calls did.
- **Not changed:** no PhysX step added or skipped (0 PhysX calls in render-only builds), constraints, collision
  bodies, ragdolls, the game's palettes and bone transforms; the part capture reads the pose after the physics step
  has written it. Per-tick state against 30 fps (`presentation.isolation`, `interpolated_matches_30`,
  `disabled_matches_30`) on the walk route, whose character has four parts: equal.

### 19.4 Transitions [V]
- Spawn: a part's first tick is cut (`new`); 2 in the fight, 4 in the walk route.
- Destruction or reuse of the address: the model's index and generation must match (`new`).
- The model's pose cut (a level change, a teleport, an animation switch without a crossfade, 17.4): the part is cut
  with it (`model`: 1 in the fight).
- Freezes and pauses (no tick, frames still built): the part's pointer goes back to the tick's palette before each
  build.

### 19.5 Visual correctness [V]
- **The fight at 120 (`presentation.pose_check`, parts):** 7,688 of 7,691 part-ticks interpolated (cuts: model 1,
  new 2), 93 bone-ticks held, 3,283 unanchored; 28,066 part palettes written, 0 in an interval where the part was
  cut; each at its frame's alpha (progress relative to the anchors within 0.00004); each anchored bone's distance from
  its anchor within 0.0143 units of the range of its two ticks' (no stretching; limit 0.1).
- **Native 4K and 32:9, the walk route** (four parts): 4,704 and 4,692 part-ticks interpolated, progress within
  0.00004, distance within 0.0071 and 0.0075; 1280x720: 4,748, 0.0103.
- **Sweeps:** the poses-only sweeps now include the parts (fight_a 0.300, fight_b 1.010 levels mean, even steps,
  alpha 1 equal to the full sweep's within 0.014). In the captures the tabard stays on the body and swings with it
  through the sweep.
- **Found and corrected: a part missing in held frames.** The first version wrote a part's palette only below alpha
  1. In a freeze (and the objects-only sweep, bones at alpha 1) its pointer still held the palette written for an
  earlier frame, whose frame memory the build had taken again: the tabard disappeared (`presentation.objects_check`:
  the objects-only sweep's alpha 1 0.115 levels mean from the full sweep's, limit 0.1; the same frames before D4b-3:
  0.004). Corrected (19.2): 0.004.
- **Shadows and culling:** the part is skinned in the shadow passes from the same pointer; culling is the model's
  (17.5).

### 19.6 Cost [V]
The fight route from a new game, 1280x720, the 250-350 s window, one run each on the final build, against the K44
build (18.3: poses on, parts the tick's):

| --kuf_frame_rate | build | frames/s | ticks/s | present p50 / p99 (ms) | CPU | GPU busy | memory | pose write per frame |
|---|---|---|---|---|---|---|---|---|
| 120 | K44 | 119.4 | 30.00 | 8.37 / 11.94 (frame time) | 279 % | 59 % | 1,385-1,386 MB | 107 us |
| 120 | D4b-3 | 115.4 | 30.00 | 8.48 / 34.28 | 293 % | 61 % | 1,383-1,386 MB | 119 us |
| -1 | K44 | 115.8 | 30.00 | 8.40 / 13.80 (frame time) | 299 % | 63 % | 1,382-1,384 MB | 81 us |
| -1 | D4b-3 | 107.1 | 30.00 | 3.23 / 33.64 | 300 % | 62 % | 1,387-1,389 MB | 90 us |

- **Parts:** 8,918 of 8,921 part-ticks interpolated in each run, 33,970 (120) and 43,860 (uncapped) part palettes
  written; the pose write per frame (models and parts) rose by 9-12 us. Physics reads 27,291, 0 finding a written
  palette; 0 rand() and 0 PhysX calls in builds; 0 allocation failures.
- **Frame rate:** 115.4 against 119.4 at 120 and 107.1 against 115.8 uncapped is within this route's run-to-run
  spread (17.7: 114.7-130.6 uncapped on one build; the fight differs between runs, K24), and the parts' own cost
  (about 10 us per frame) cannot account for it; one run each, so no difference is claimed either way. The present
  p99 (about 34 ms, whole run) matches the D4b-2 build's (17.7: 34.42 at 120, 33.70 uncapped); the K44 rows give
  the profile's frame times in the window instead (18.3).
- **Latency** unchanged: tick to present p50 10.7 / 11.7 ms, full response p50 44.0 / 45.0 ms.
- **Memory:** per part two ticks' palettes and the pair (about 300 bytes per bone, under 10 KB per 21-bone part);
  resident memory flat.

### 19.7 Tests
| Test | What |
|---|---|
| `presentation.poses` unit cases (parts) | a part on the bone it hangs from (ends exact, follows the model's interpolated bone, progress recovered), mismatched palettes refused, a bone turned at once held, a chain swinging link by link keeping each length, a bone swinging about its anchor on the arc |
| `presentation.pose_check` (`--min-parts 1000`) | the parts summary and every part palette written (above) |
| `presentation.pose_check_walk` | the same on the walk route at 120, native 4K and 32:9 |
| `presentation.bones_physics_fault` + `_check` | the fault injected: the physics readers must see a written palette |
| `presentation.objects_check` | its objects-only sweep caught the missing part (19.5) |

### 19.8 Remaining
- [?] Parts on other levels and characters (only the three characters above seen).
- An unanchored bone (a body moving on its own) moves on the straight line between its ticks.
- Particles, effects, texture animation and the world-anchored HUD (16.7, D4b-4 and later).

## 20. D4b-4: HUD, particles and effects (2026-10-28)

### 20.1 HUD elements and the world (D4b-4A) [V]
- **How the HUD is drawn:** the UI's update and submission (`sub_825BE6B0`, each tick) puts heap entries into the 2D
  draw-list object's lists (renderer + 30156: + 88 / + 120 / + 152 + slot * 16); each entry is a type and a screen
  rectangle (x0, y0, x1, y1 in the 1280x720 layout) and its drawing data. Render-only frames draw the tick's entries
  as they are (14.2), so an element the UI placed from a world point would stay where the tick put it.
- **Measured with `--kuf_trace_hud_from`** (every entry of each tick's frame logged at its release):
  - the Shadow Depths walk at 120: 82 entries per tick; through the route's turn (the camera turns 6 degrees per tick,
    the player stands, 31 ticks, about 84 degrees in all) every rectangle stayed the same;
  - the tutorial fight at 120, 600 ticks of combat (ticks 6700-7299): 79-112 entries per tick, none placed from the
    world. The changes: gauge fills changing width (the SP bar), one 4x18 minimap marker that moved by 1 pixel (it
    follows the player's position on the minimap, not the camera; integer pixels, 1 pixel in 140 ticks), and in one run
    a menu panel at the top right whose highlight frame (four 1-pixel lines around a 40x40 slot) jumped between two
    fixed slots three times (ticks 6751, 7170, 7240) and a 1x13 line in the same panel once.
  - The enemy name bar is a fixed bar at the top; there are no health bars, damage numbers or labels over characters
    or items in these scenes (the pick-up prompt is a screen-space message).
- **Result:** no HUD element needs the frame's camera or objects: nothing changed. The minimap markers keep the tick's
  integer position (their motion between ticks is a fraction of a pixel). At native 4K and 32:9 the entries are the
  same layout rectangles (the HUD is scaled by the display path, docs/DISPLAY.md): the walk check passes there too.
- **Guard:** `presentation.hud_check_walk` (the three walk runs, `--turn`) and `presentation.hud_check_fight` fail if
  an element keeps its size and moves by more than 2 pixels on 3 or more of any 10 consecutive ticks (an element
  placed from the world moves with the camera and the units on nearly every tick, on every second one if placed at
  15 Hz), or (in the turn) if any rectangle changes; isolated jumps (the highlight above) are listed in the output, not
  failed. The first version failed on any single move over 2 pixels and so failed on the highlight's jump; the
  criterion was changed after reading the entries (the slot boxes never moved, only the frame around the selected one).
  `presentation.hud_check_selftest`: a still HUD, a gauge and a highlight jumping between slots every 35 ticks pass;
  markers moving 9 pixels every tick or every second tick fail.

### 20.2 Effects: what moves and by which clock [V unless marked]
| System | Clock | In a render-only frame before D4b-4 | Now |
|---|---|---|---|
| Effect shaders' time (vegetation sway, ground mist, fireflies and the other draws that read the frame timer) | the frame timer: count of tick frames / 30 (renderer + 28864: + 12 count, + 20 rate), read by `sub_821D24F0`, `sub_821F0350`, `sub_822244B0` and `sub_82224C60` on the render thread only | the tick's time (count set back, 14.2) | the frame's own moment (20.3) |
| Effect pieces that are models (dust, smoke and debris pieces; dropped loot): class 0x82101540 objects spawned and released in combat (about 550 creations in the fight) | the tick: their world matrices are written per tick | the tick's | interpolated as objects since D4a (16), including smoke drawn attached to a dying character |
| GPU particles (`ParticleNormal.fx`, `ParticleSoft.fx`): spawned per tick, moved in the vertex shader by `ParticleTime` | `ParticleTime` = the render slot's world frame number, an integer (renderer + 26532 + slot * 4, set by `sub_8222C0E8` about 63 times per tick) | the tick's | unchanged (20.4) |
| Weapon trails (`SimpleTrail.fx`) | trail objects made at level load (`sub_82480448`, 10 in the fight), updated per tick [L] | the tick's | unchanged; no trail seen in the tested fights [?] |
| Flipbook and unit animations counted in world frames (unit + 716 = start + count - start count, 14.2) | the world frame count (0x82AE7620), an integer | the tick's frame | unchanged |
| The per-tick radius list (renderer + 28976 + slot * 16, filled by `sub_8269E2C8`: entries within a radius of a world point, faded with distance; applied once by the tick's frame, `sub_821ABF50`) [?: probably foliage pushed by characters] | the tick | applied by the tick's frame only | unchanged |
| CRT `rand()` | the game thread | 0 calls in render-only frames | 0 calls; the effect clock calls no game code |

- **Spawning, lifetimes, emitters:** all run in the tick (game thread), as before; nothing of D4b-4 runs game code,
  spawns or advances anything.
- **Particles' random numbers:** the spawning draws from the game thread's rand() in the tick; render-only frames
  draw none (0 calls, 14.3).

### 20.3 The effect clock at the frame's moment [V]
- Each frame (the tick's own and every render-only one) records how far its world is shown behind its tick: the lag,
  1 - its alpha (`kuf::presentation::EffectLag`, handed to the render thread with the frame).
- While one of the four readers draws that frame on the render thread, the frame timer's rate is set so that
  count / rate = (count - lag) / 30, and put back when it returns (`BeginEffectTime` / `EndEffectTime`,
  render_frames.cpp; nested readers use the outermost's). The count, which the game keeps, is never written.
- At alpha 1 the time is the tick's own (lag 0): the endpoints are the original frames. Across ticks the time is
  continuous: a tick's first frame at lag about 1 shows the time the frame before showed.
- `--kuf_frame_interpolation_effects` (on with interpolation; false: the tick's time, as before). With interpolation
  off or at 30 fps nothing changes.

### 20.4 Not interpolated, and why
- **GPU particles:** `ParticleTime` is a whole frame number; presenting it as count - lag would draw a particle born
  in the newest tick at a negative age, a position before it existed (extrapolated), and the shader's handling of
  that is not known. In the tested routes nothing visible is driven by it: shifting it back by 30 frames changed the
  walk's and the fight's sweep frames by at most 0.002 levels mean. It stays the tick's.
- **Trails, flipbooks, the radius list:** per-tick data or integer frames with no previous state kept; left at 30 Hz.

### 20.5 Isolation [V]
- The rate is changed only inside the four draw functions on the render thread; at every tick's start the game thread
  checks the rate is the game's own whenever no such draw runs (`effect_clock_leaks`, required 0). The fault
  `--kuf_debug_effect_rate_kept` (the rate not put back) must be counted: `presentation.effects_rate_fault_check`.
- No game code runs for it, no rand() call, no PhysX call; per-tick state against 30 fps on the walk route
  (`presentation.isolation`, `interpolated_matches_30`, `disabled_matches_30`, the effect clock on) stays equal.

### 20.6 Visual correctness
Measured with `effect_check.py` (latest presentation run, 2026-10-28):

| Run | Frames at their own moment | Outside their tick interval | Back in time | Effects-only sweep, alpha 0 to 1 (levels mean) | Steps | Alpha 1 to the full sweep's |
|---|---|---|---|---|---|---|
| walk 120 (1280x720) | 7,086 | 0 | 0 | walk 0.265, turn 0.109 | 0.127 0.114 0.069 0.068; 0.030 x 4 | 0.008, 0.007 |
| walk native 4K, 240 requested | 6,117 | 0 | 0 | walk 0.333, turn 0.149 | 0.107 0.106 0.103 0.091; 0.043 x 4 | 0.003, 0.004 |
| walk 32:9 (5120x1440), 240 requested | 6,668 | 0 | 0 | walk 0.220, turn 0.118 | 0.063-0.066; 0.035-0.036 | 0.005, 0.004 |
| fight 120 | 32,315 | 0 | 0 | fight_a 0.024, fight_b 0.014 | 0.008-0.010; - | 0.009, 0.006 |

- What moves in the sweeps is the swaying vegetation and the ground mist (diff images); the steps are even and alpha 1
  is the original frame (within capture noise, 0.003-0.009). In fight_b nothing in view moves with the effect clock
  (0.014, below the 0.02 the check needs to judge the steps): the sweep there shows only that alpha 1 is unchanged.
- Rate leaks 0 in every run; the fault run (`--kuf_debug_effect_rate_kept`) was caught at 3,491 tick starts.
- **Frames placed back in time (found by this check):** in the first fight run one frame showed 0.0055 tick (0.18 ms)
  before the frame handed before it, the first render-only frame after a tick's own frame (the tick's own frame was
  placed with its scene build's estimate, 2.6 ms, the next with a build's, 0.9 ms; when the tick's scene was quicker
  than its estimate the next frame came out earlier). This was in the camera and the objects as well, since D3b-3.
  `FrameAlpha` now never places a frame before the tick's previous one (`alpha_held` in the run summary: 1 of 32,315
  frames in the fight since, 0 in the walks).
- **K34's test and the effect phase:** `render.k34_check` (fbo against fsi frames) failed once in a parallel ctest
  run, 0.92 / 0.73 levels against 1.0 / 0.6. Not the effect clock: with it off and on, run one game at a time, the pairs
  gave the same 0.08 / 0.06; the two runs had reached gameplay 6 ticks apart and the test captured relative to
  gameplay, so the mist (timed by the frames drawn since the start) was at different moments (0.20 per tick apart in
  older pairs, before D4b-4 as well). The captures are now at fixed ticks; thresholds unchanged.

### 20.7 Cost
The tutorial fight route (`objects_route.kufscript`, 6 minutes from a new game) with `--kuf_profile`, one game at a
time, effect clock on unless marked. FPS per segment of the run (10 s bins: 60-150 s the opening and walk, 150-250 s
the tutorial, 250-350 s the fight); latency and pacing over the whole run.

| Requested | FPS 60-150 s | 150-250 s | fight 250-350 s | CPU / GPU (fight) | Present p50 / p99 ms | Tick p50 / p99 ms | Ticks/s | Full response p50 / p99 ms | Builds late / busy |
|---|---|---|---|---|---|---|---|---|---|
| 30 (original) | 30.0 | 30.0 | 30.0 | 240 % / 24 % | 33.45 / 36.77 | 33.33 / 35.02 | 29.99 | 41.5 / 69.1 | - |
| 60 | 60.0 | 60.0 | 60.0 | 216 % / 37 % | 17.95 / 34.92 | 33.33 / 33.98 | 30.00 | 44.3 / 101.0 | 14 / 712 |
| 120 | 116.3 | 106.8 | 116.4 | 276 % / 60 % | 8.55 / 34.36 | 33.33 / 33.95 | 30.00 | 43.9 / 100.7 | 55 / 4,077 |
| 120, effect clock off | 115.7 | 104.2 | 117.3 | 289 % / 59 % | 8.56 / 34.31 | 33.33 / 33.66 | 30.00 | 44.2 / 99.8 | 56 / 4,306 |
| 144 | 128.1 | 104.3 | 115.1 | 298 % / 63 % | 8.49 / 34.19 | 33.33 / 33.88 | 30.00 | 45.0 / 100.8 | 1,969 / 10,658 |
| 240 | 183.5, 185.3 | 104.5, 107.6 | 112.1, 140.7 | 302 % / 63 % | 7.97 / 34.03 | 33.33 / 33.93 | 30.00 | 45.0 / 100.9 | 133 / 37,608 |
| 240, effect clock off | 184.0, 190.8 | 105.5, 122.2 | 129.5, 100.9 | 303 % / 64 % | 6.82 / 33.99 | 33.33 / 33.97 | 30.00 | 44.4 / 101.1 | 148 / 35,362 |
| uncapped | 235.9 | 105.9 | 118.7 | 298 % / 63 % | 3.81 / 33.66 | 33.33 / 34.05 | 30.00 | 44.7 / 100.8 | 0 / 0 |

- **Cost of the effect clock: none measurable.** It writes one float before and after each of the four draws (460-630
  times per frame) and the off setting does the same with a lag of 0; the walk segment at 240 is 183.5-185.3 on and
  184.0-190.8 off. The fight segment varies from run to run (100.9-140.7 in four runs at 240, on and off alike: the
  first pair favoured off by 17 fps, a second pair favoured on by 40); it is not the effect clock.
- **Actual against requested:** 60 is reached everywhere and 120 nearly (116 in the opening and the fight, 104-107
  in the tutorial); higher requests reach 128-236 in the opening; the tutorial and the fight top out at 100-140 (render thread and
  GPU, K39, unchanged by D4b-4).
- **Simulation:** 30.00 ticks per second at every rate; 0 rand() and 0 PhysX calls in the render-only frames of every
  run; memory 1,375-1,393 MB at every rate (the effect clock allocates nothing).
- **Latency:** the frame shown is at most a tick behind the newest (no extrapolation): full response p50 44-45 ms at
  every rate above 30 (41.5 at 30 fps), p99 about 100 ms (three ticks) as since D4b; unchanged by D4b-4.
- **Longer run with saving and loading:** the save chain (`save_new`: a new game, the fight, a save through Quit;
  `save_continue`: CONTINUE in a new process, a re-equip, a save; `save_reload`: a third process loads it) at 120 with
  the effect clock traced, saves isolated in the runs' folders (`--run-prefix d4b4save`): all three passed; 21,487 +
  7,103 + 3,508 ticks, 59,328 + 15,894 + 11,493 frames at their own moment, 0 outside their tick interval, 0 back in
  time, 0 rate leaks, 0 restore failures (14 records left out in the saves, K46), 1 frame held (FrameAlpha).

### 20.8 Tests
| Test | What |
|---|---|
| `presentation.effect_check_presentation_120`, `_native_4k`, `_32_9`, `presentation.effect_check_fight` | every frame's effect time between its two ticks' (within 1e-4 s), never back in time outside the freezes, 0 rate leaks, the effects-only sweeps (`<label>_e000 ... _e100`) stepping evenly and ending on the full sweep's frame |
| `presentation.effects_rate_fault` + `_check` | the rate left changed: the leak check must count it |
| `presentation.hud_check_walk`, `_fight`, `_selftest` | the HUD (20.1) |
| `render.k46_*`, `gameplay.k24_*` | K46 and K24 (14.8, docs/KNOWN_ISSUES.md) |

Full regression (2026-10-29, at c83c6d0): 1,876 of 1,876 passed (`ctest -j6`); SDK patches apply; the game folder
verified before and after; 53 save files unchanged (sha256); tracked files clean.

### 20.9 Remaining
- GPU particles, weapon trails, flipbook animations and the per-tick radius list stay at 30 Hz (20.4).
- [?] Effects on other levels (magic, fire, water) not surveyed; those timed by the frame timer follow the frame.

## 21. D5: the frame rate in PC Settings (2026-10-29)

### 21.1 Where [V]
The PC page of both Settings menus (in game: options menu > Settings > PC Settings; on the title screen: SETTINGS >
PC Settings) scrolls through its pages with Down past the last row: the camera rows, Restore Defaults, Distance and
Field of View, the display page, then two new pages. Like the display page their rows show their values in the label.

| Page | Row | Values | Default |
|---|---|---|---|
| Frame rate | FPS | 30 (Original), 60, 90, 120, 144, 165, 240, Unlimited (a limit from the file between 31 and 480 is shown as its number; Left / Right step to its neighbours) | 30 (Original) |
| Frame rate | Interpolation | On / Off: the camera and the player drawn between ticks (Off: each frame between ticks repeats its tick) | On |
| Advanced (below) | World Objects | On / Off: enemies, other characters, items between ticks (D4a) | On |
| Advanced | Animation | On / Off: skeletal poses and the physics-driven parts hanging from them (D4b, D4b-3) | On |
| Advanced | Effects | On / Off: the effect shaders' clock at the frame's moment (D4b-4) | On |

- The FPS values are limits: the frames presented per second never exceed them; the hardware may draw fewer (21.6).
- The Advanced rows act only with Interpolation On and FPS above 30. Camera and player cannot be switched separately
  (one switch, as in D3b-3), and the physics-driven parts have no switch of their own (they hang from the
  interpolated pose, so they follow Animation): no control is shown for either.
- Input: Left / Right step the value (toggles: Left On, Right Off, as the camera page's), Select (Enter, A, left
  click) steps forward and wraps, so the mouse alone (wheel to move, left click to change, right click back) can set
  every row. Back saves (pc_settings.toml) and leaves, as on the other pages.
- No launcher, overlay or separate window: the rows are the game's own controls (pc_settings_menu.cpp).

### 21.2 Storage [V]
`pc_settings.toml`, table `[frame_rate]`: `limit_fps` (30 = the original, 31-480, 0 = unlimited), `interpolation`,
`interpolate_objects`, `interpolate_animation`, `interpolate_effects`. A file without the table (from M6B or M6C)
stays at 30 fps: nothing changes for an existing installation until the player raises FPS. Invalid values fall back
to their defaults one by one with a warning (29, 481, -1, a float, a string: 30; a non-boolean toggle: On); the file
is left as it is until the player changes something. Unit tests: `settings.*` (defaults, round trip of every menu
value and custom limits, invalid values, overrides).

### 21.3 Command line over the saved settings [V]
`--kuf_frame_rate`, `--kuf_frame_interpolation`, `--kuf_frame_interpolation_objects`, `_bones` and `_effects` still
work (tests, diagnostics). Rule: **a flag given on the command line (or as a REX_ environment value) wins over the
saved setting for that run, flag by flag**; the settings not given come from the file. The log says which flags won
("[kuf-pacing] frame rate from the command line for this run (the saved settings unchanged): ..."), the menu shows
the values in effect, and the file keeps its own frame-rate values when it is written for another change; a
frame-rate value changed in the menu becomes a normal setting again and is saved. `--kuf_pc_settings=
"frame_rate.limit_fps=120"` overrides like any PC setting (M6B's rule: not written unless a setting is changed).

### 21.4 Switching while the game runs [V]
Every change applies at once, in menus and in the title screen, without a restart: the menu hands the settings to
the frame scheduler (`kuf::pacing::RequestFrameRate`), which applies them when the next tick has been handed to the
render thread (`ApplyFrameRequest`, frame_pacing.cpp), within one tick:

1. Both "free" slots held: the render thread has drawn everything handed to it and waits.
2. `presentation::Reset`: every value written into either slot for a presented frame (camera and field of view, the
   player, world objects, palette pointers) gets the game's own back where the slot still holds what was written
   (the identity checks of D4a and D4b), and the history is dropped: the next tick is a first tick (cut "first"),
   so nothing is interpolated across the change.
3. Render-only frames stopping (to 30): both slots' kept releases run (the HUD lists and frame arenas the next ticks
   into the slots would have released, render_frames.cpp) - nothing stale is left for the original pacing.
4. The new rate and switches; the SDK's guest vblank follows (render-only frames need its 1000 Hz vblanks, the
   original pacing its 60 Hz ones: the `vsync` cvar, read live by the vblank worker and the command processor); the
   presentation clock restarts from the next tick (K44).
5. This tick builds no render-only frames; they start from the next tick's own scene build (which saves what they
   need, K46's lists included).

The render thread's identity is now recorded in every mode (a run started at 30 can switch later). A change costs one
tick without frames between ticks; switching took 3.8-6.3 ms in game (the wait for the render thread), 25-57 ms on
the title screen (its movie keeps the render thread drawing). Diagnostic modes (`--kuf_render_builds`,
`--kuf_render_repeat`, `--kuf_fixed_tick=false`) refuse changes (logged).

Verified in `frame_rate_switch` (30 -> 60 -> 90 -> 120 -> 90 -> 60 -> 30 -> ... -> 120 -> ... -> Unlimited -> ... -> 60,
Interpolation and each advanced row Off and On, then a walk): every change applied within 0.15 s of the menu, the
guest vblank right after each, each held rate drawn, no rand() or PhysX call in a render-only frame, no restore
failure, no effect-clock leak; and the game's state per tick (seed, every object, the simulation camera) equal to
the same route with every change dropped (`--kuf_debug_frame_rate_ignored`, 30 fps throughout) for 6,700
ticks: no gameplay reset, no simulation step added or lost, the camera unchanged. The run: 27 changes, all applied (3.8-6.3 ms each after the first, 36 ms), the held rates 60.1, 119.7, 30.0, 211 (unlimited) and 60 fps.

### 21.5 VSync and the display [V]
- **No second limiter:** the only frame limiter is the presentation clock (K44). The host presents with Vulkan's
  IMMEDIATE mode where available (the SDK's `vulkan_allow_present_mode_immediate`, on; logged "presentation mode 0"):
  there is no host VSync, frames may tear, and the refresh rate does not cap the frame rate. The guest vblank (the
  console's 60 Hz, `vsync`) is switched with the frame rate as above; it is not a player option.
- **No VSync option is offered** (none existed before D5, and adding one needs the swapchain's present mode
  chosen at creation: FIFO or mailbox would cap or pace frames to the refresh rate): a limitation, 21.8.
- **Display changes keep the frame rate:** `frame_rate_display` at 120 (from the settings): Mode Windowed, a smaller
  size, Widescreen Off and On, Apply and Keep (the swapchain recreated at 1920x1080); before and after, 118.1 and 119.6 frames per second.
  Fullscreen and borderless differ only in the window; the frame rate and the display rollback (Apply / Keep / 15 s)
  are independent. Render scale (the native 4K and 32:9 cost): 21.6.

### 21.6 Frame rate achieved [M]
The objects route (tests/presentation/objects_route.kufscript: the camp, a walk, a fight from about 250 s) for 360 s
per run, the frame rate set through the settings (`--kuf_pc_settings=frame_rate.limit_fps=N`, the menu's path),
`--kuf_profile`, one game at a time on this machine. The frame is 1280x720 (the sandbox's simulated output) unless
named: `k4_*` render 3840x2160 (render scale 3x3), `uw_*` 5120x1440 (32:9, 4x2). Frame time: the interval between
presents; tick: the game's 30 Hz tick interval; full response: from a tick's start (where it reads input) to the first
present showing that tick completely (15.6's latency measure); builds late / busy: render-only frames built late or
skipped because the render thread was still drawing.

| Run | requested | FPS 60-150 s | 150-250 s | fight 250-350 s | frame time p50 / p95 / p99 ms | tick p50 / p99 ms | ticks/s | full response p50 / p99 ms | CPU % / GPU % (fight) | RSS MB | builds late / busy |
|---|---|---|---|---|---|---|---|---|---|---|---|
| r30 | 30 | 29.7 | 30.0 | 30.0 | 33.42 / 35.01 / 37.41 | 33.33 / 35.41 | 29.92 | 45.9 / 69.4 | 239 / 25 | 1392 | 0 / 0 |
| r60 | 60 | 59.2 | 60.0 | 60.0 | 17.67 / 27.75 / 34.95 | 33.33 / 34.06 | 29.92 | 44.3 / 100.9 | 223 / 39 | 1384 | 16 / 715 |
| r90 | 90 | 87.1 | 59.7 | 50.6 | 13.35 / 29.64 / 35.09 | 33.33 / 34.28 | 29.88 | 53.7 / 101.0 | 286 / 35 | 1387 | 54 / 8518 |
| r120 | 120 | 89.4 | 75.9 | 100.4 | 9.01 / 21.88 / 34.92 | 33.33 / 35.25 | 29.68 | 48.3 / 103.9 | 264 / 59 | 1401 | 94 / 10433 |
| r144 | 144 | 127.2 | 106.9 | 117.7 | 8.03 / 13.00 / 34.34 | 33.33 / 34.34 | 29.89 | 44.4 / 101.3 | 296 / 64 | 1394 | 1998 / 10002 |
| r165 | 165 | 137.7 | 98.3 | 134.5 | 7.38 / 12.65 / 34.18 | 33.33 / 34.06 | 29.91 | 44.3 / 101.3 | 302 / 71 | 1386 | 250 / 15664 |
| r240 | 240 | 184.2 | 97.3 | 125.2 | 7.10 / 12.30 / 34.03 | 33.33 / 34.06 | 29.92 | 44.3 / 101.0 | 300 / 73 | 1389 | 161 / 36893 |
| r0 | unlimited | 219.0 | 147.9 | 163.7 | 4.98 / 10.12 / 33.50 | 33.33 / 34.60 | 29.88 | 41.4 / 101.7 | 304 / 81 | 1394 | 0 / 0 |
| k4_60 | 60 | 58.7 | 59.9 | 59.9 | 16.86 / 27.86 / 35.02 | 33.33 / 34.35 | 29.88 | 45.9 / 102.5 | 242 / 88 | 1375 | 19 / 766 |
| k4_120 | 120 | 51.3 | 76.4 | 87.9 | 8.47 / 34.73 / 39.16 | 33.33 / 35.14 | 29.87 | 49.3 / 102.9 | 258 / 99 | 1380 | 111 / 15938 |
| k4_0 | unlimited | 76.1 | 69.4 | 66.4 | 11.31 / 28.18 / 34.84 | 33.33 / 34.37 | 29.88 | 50.4 / 102.1 | 286 / 99 | 1373 | 0 / 0 |
| uw_60 | 60 | 59.0 | 59.8 | 59.9 | 17.52 / 25.78 / 34.90 | 33.33 / 34.08 | 29.91 | 45.1 / 101.2 | 228 / 85 | 1382 | 15 / 732 |
| uw_120 | 120 | 82.4 | 75.3 | 78.7 | 10.17 / 25.37 / 34.79 | 33.33 / 34.15 | 29.92 | 46.8 / 101.4 | 287 / 99 | 1375 | 60 / 14494 |
| uw_0 | unlimited | 82.4 | 74.9 | 65.9 | 10.81 / 27.29 / 34.74 | 33.33 / 34.18 | 29.91 | 48.5 / 101.6 | 281 / 100 | 1369 | 0 / 0 |

- **Every limit holds:** no run presents more frames than its limit; 30 stays the original (29.7-30.0, frame time
  p50 33.4 ms). 60 is reached everywhere, at 4K and 32:9 too (58.7-60.0).
- **Above 60 the hardware sets the rate**, not the limit: at 1280x720, 127-138 fps in the camp at 144/165, 184-219 at
  240/Unlimited, 75-135 in the busy parts (the render thread draws every frame of the scene: "busy" counts frames it
  could not take). In this sweep 90 ran 51-60 fps in the busy middle of the route while 120 ran
  76-100; not reproduced (22.4: 88.6-89.9 in every segment of two runs, 120 at 106-118 on the same build; the
  sweep's 120 was low as well, the machine probably busier). At 4K and 32:9 the GPU is full (88-100 %): 51-88 fps
  at 120, 66-82 Unlimited.
- **The simulation is unaffected:** 29.7-29.9 ticks per second and tick p50 33.33 ms in every run (the same as at 30).
- **Pacing:** frame time p95 10-13 ms at 144-Unlimited at 1280x720; the p99 of about 34 ms in every run is the one
  frame per tick switch to the next tick's first frame that waits on the tick's own scene build (K44's clock restarts
  there), as before D5.
- **Latency:** the full response p50 is 41-54 ms above 30 against 45.9 ms at 30; its p99 (about 100 ms, at 30: 69 ms)
  comes from the ticks whose first frame waits for the render thread (busy), as in 15.6.
- Memory: 1.37-1.40 GB resident in every run (no growth with the frame rate).
- The title screen: about 36 fps whatever the setting (its movie, K47).

### 21.7 Tests [V]
| Test | What |
|---|---|
| `settings.*` unit cases | the [frame_rate] table: defaults (30, interpolation on), every menu value and custom limits round trip, invalid values one by one, overrides |
| `settings.frame_rate_menu` + `_check_menu` | keyboard, in game: every row and both pages, 30 -> 60 -> 120 -> 30 and 120 -> Unlimited -> 60, Up/Down across the pages, the file written; gameplay at 60 after |
| `settings.frame_rate_persist` + `_check_persist` | a new process with that file: 60 fps from the first frame without flags, the advanced settings in effect; changed back to 30 and saved |
| `settings.frame_rate_controller` + check | virtual Xbox 360 pad: D-pad, A (steps), B |
| `settings.frame_rate_mouse_title` + check | the title SETTINGS with the wheel, left clicks (driver and real SDL events) and right clicks |
| `settings.frame_rate_switch`, `_switch_ignored`, `_switch_state`, `_check_switch`, `_check_switch_ignored` | runtime switching with the game's state compared to the same input with every change dropped; the fault run shows the check catches a menu that does not reach the scheduler |
| `settings.frame_rate_cli` + check | `--kuf_frame_rate=120` over a file saying 60: 120 drawn, the file's other value used, the file keeps 60 after another change is saved |
| `settings.frame_rate_invalid` | invalid values: the original 30 fps, warnings, no render-only frames |
| `settings.frame_rate_display` + check | display mode, size, widescreen, apply and keep at 120 |
| `settings.frame_rate_k44_60` + check | the K44 late-tick test with 60 fps from the settings path: every following tick draws its two frames |
| `settings.frame_rate_save` + check | 120 set in the menu, the Quit save, the title, CONTINUE: the save loads, still 120 |

### 21.8 Limitations
- No VSync option; frames are presented without waiting for the refresh (tearing possible) (21.5).
- The title screen and its menus are drawn over the title-loop movie: 30-36 frames per second whatever the
  setting: the movie player sleeps on the render thread inside each frame until its next movie frame (22.5, K47); in
  game the rates of 21.6.
- A frame-rate change costs one tick without frames between ticks (a 33 ms step), by design (21.4).
- Camera and player interpolation share one switch; the physics-driven parts follow Animation.
- The labels are English, as the other PC Settings.
- Above 60 the rate depends on the scene and the hardware (21.6); the frame clock keeps the phase it restarted
  with, which can space a tick's frames unevenly (22.6, K49).

### 21.9 Manual playtest
A sandbox with copies of saves and its own settings (the game folder, `~/.local/share` saves and the real
`pc_settings.toml` untouched); it opens a window on the desktop:

    tools/run_sandboxed.sh -t 3600 -n play-d5 --gpu --window --audio --saves-from build/m6-saves/celine-shadows-depths-1

Then, the persistence across launches (the first session's saves and settings, a new run directory):

    tools/run_sandboxed.sh -t 3600 -n play-d5-2 --gpu --window --audio --saves-from build/runs/play-d5 --pc-settings-from build/runs/play-d5

1. Title: SETTINGS > PC Settings, Down past the display page: FPS 30 (Original), Interpolation On; Advanced below:
   all On (the defaults).
2. CONTINUE into the game; options menu > Settings > PC Settings > FPS 60, Back. The game runs at 60 at once, nothing
   jumps, the camera stays where it was.
3. 120, then Unlimited, then 30: each applies when the menu changes it; 30 looks like the original.
4. With a controller: D-pad to the row, Left/Right or A to change, B back. With the mouse: wheel, left click, right
   click.
5. Interpolation Off at 120: the camera moves in 30 Hz steps again; World Objects / Animation / Effects Off: enemies,
   poses or the water and fire shaders step at 30 Hz.
6. Display page: windowed, a size, Apply, Keep: the frame rate stays.
7. Quit (save) to the title, CONTINUE: the save loads, the frame rate kept. Close the game; the second command: the
   settings come back from the first session.

## 22. D5 validation: the full regression within memory, 90 fps, the title screen (2026-10-30)

### 22.1 Why the D5 regression stopped [M]
The D5 full regression (`ctest -j6`) was stopped by the session's low-memory guard after 1,810 of 1,902 tests had
passed (none failed; an earlier report said 811: a miscount). Measured with a 1 s sampler of the test process tree
and each check run alone (peak RSS from the kernel at exit):
- **`presentation.check` held 14 GB:** it converted all 72 captures of each run to float64 before checking three
  sweeps of eight; at native 4K that is 72 x 3840 x 2160 x 3 x 8 bytes. At `-j6` it ran beside the next game and
  other checks while the desktop's applications used about 15 GB of the 31 GB: the guard stopped the run (a manual
  run of the old check beside one game took the available memory from 14 GB to 3.2 GB). Fixed (cbb6931): each sweep
  reads its eight captures when it is checked; 2.0 GB, 71 s -> 9 s, output byte-identical on the 1280x720 run.
  `tools/peak_rss.py` fails a test whose peak RSS exceeds a limit; `presentation.check` runs under 4096 MB, with
  self-tests `tools.peak_rss_under_limit` / `_over_limit`.
- **Freeze runs held two copies of guest memory:** `RunBuildFreeze` (the alpha sweeps and the isolation diff, test
  runs only) assigned a new copy of all readable guest memory (about 3 GB) while the previous one was still held: test
  game processes peaked at 9.8 GB (objects_120, bones_120, presentation_120, frames_complete). The copy is now released
  first (90bf881): 6.8 GB. Reading every guest page also commits it (about 2.3 GB stays resident after the first
  freeze); other game runs stay at 1.1-1.5 GB.
- **Every check measured:** the 315 tests that do not run the game, each run alone after the fixes: largest peak
  2.4 GB (`presentation.effect_check_presentation_native_4k`), then 2.1, 2.0 (`presentation.check`), 1.6 and 1.6 GB;
  the rest under 0.7 GB.
- **No accumulation, no orphans:** every game test holds the `game_gpu` resource lock, so at most one game runs at a
  time (the sampler saw at most one in every batch); no game, sandbox or test process was left after any batch;
  memory returned to the baseline between tests.

### 22.2 The regression in batches [V]
The suite split into six batches by fixture groups (a test's fixture setups and checks always in the same batch, so
nothing ran twice or out of order), run one after another with `ctest -j4` and a 1 s memory sampler (lowest
available memory, the test process tree, games at once):

| Batch | Tests | What |
|---|---|---|
| b0 | 1,719 | unit and tool tests (no game) |
| b1-b4 | 29, 44, 53, 57 | game runs and their checks, about 1.5 h each |
| b5 | 2 | the peak-memory guard's self-tests |

    ctest --test-dir build/linux-amd64-relwithdebinfo -j4 --output-on-failure --output-junit build/logs/d5v-<b>.xml --tests-from-file <b>.txt

Three runs of the suite: round 1 on ae186b8 + the presentation.check fix, round 2 on cca44ac, round 3 on 8c70d75.
Round 3 was stopped when the roadmap's testing policy reserved the full regression for feature freeze: b0 1,719 of
1,719, b1 29 of 29, b2 35 of 44 completed, 0 failed; 121 tests not run on 8c70d75 (deferred to feature freeze). Rounds 1 and 2 found four faults, all fixed with focused changes:

| Found | Cause | Fix |
|---|---|---|
| `presentation.effect_check_fight`: 2 effect-clock rate leaks (round 1, ticks 4511 and 4723, not in a freeze) | the tick-start check read the draw depth, the rate, the depth again; with the game thread preempted longer than one draw (the render thread sets the rate about 58,000 times a second) a whole draw fell between the reads | d0e5a5a: the set counter compared as well; the fault test still counts leaks |
| `presentation.pose_check`: a part bone 9.8 units off its anchor's distance (tick 7737, the player's tabard) | its offset from the anchor turned to the other side in one tick while its rotation turned less than kBoneTurn: `ArcPosition`'s fallback for opposite directions took the line through the anchor (the three traced values fit it exactly) | cca44ac: held at tick b's like a turned bone; unit test (2.5 instead of 5 without it) |
| `presentation.hud_check_walk` (native 4K): ticks 1903 and 1920 of the turn untraced | the HUD trace labelled a frame with the ticks completed when it was drawn; a slow frame (a pipeline created) was drawn after the next tick and logged as its, that one then skipped | 8c70d75: each frame carries its tick from the hand-off (also the effect-clock trace) |
| `timing.k44_120_check`: 58 of 848 ticks dropped a step, render busy (limit 5 %) | not reproduced: the same test 0, 0, 1 alone afterwards (4 and 8 in earlier rounds); the drops came in bursts of 9 per second | none (22.6 notes the clock phase it ran with) |

### 22.3 D5 behaviour [V]
In every round all `settings.frame_rate_*` tests passed (menu by keyboard, controller and mouse, persistence,
switching with the state comparison, the fault run, command-line precedence, invalid values, display changes, save
and CONTINUE, K44 at 60 from the settings), with `render.k46_*` (released objects), the `timing.*` K44 tests, the
fixed 30 Hz tests and the M6B camera and M6C display suites.

### 22.4 The 90 fps result [M]
Two runs each at 90 and 120 (objects route, `--kuf_profile`, `--kuf_trace_frame_clock` over 900 ticks in the middle
of the route and in the fight; diagnostics only, no change):

| Run | fps 60-150 / 150-250 / 250-350 s | frames per tick (traced) | busy / late steps | clock phase (median) | render thread CPU (fight) | frame time p50 / p95 / p99 ms |
|---|---|---|---|---|---|---|
| 90, middle | 88.6 / 89.9 / 89.7 | 3 on 900 of 900 | 0 / 0 | +1.83 ms | 71 % | 11.13 / 19.38 / 34.62 |
| 90, fight | 88.6 / 89.6 / 89.8 | 3 on 891 of 900 | 9 / 0 | +2.71 ms | 55 % | 11.15 / 16.82 / 34.65 |
| 120, middle | 117.5 / 108.5 / 112.8 | 4 on 820 of 900 | 80 / 0 | +1.93 ms | 78 % | 8.55 / 12.54 / 34.24 |
| 120, fight | 117.8 / 115.5 / 105.7 | 4 on 460 of 900 | 442 / 0 | +0.10 ms | 80 % | 8.57 / 12.70 / 34.22 |

90 holds its rate; the low 90 of 21.6 (51-60 fps) did not reproduce. Ticks 29.92 per second, tick p99 under 34 ms
in all four. At 120 the fight's frames are limited by the render thread (busy steps late in the tick, median +25 ms):
that is the render cost, D6.

### 22.5 The title screen's 30-36 fps (K47) [V]
Title menu over the title-loop movie, 60 s between two marks:

| Limit | fps | render thread CPU | GPU command thread | busy steps (900 ticks traced) |
|---|---|---|---|---|
| 30 | 30.0 | 42 % (spins in its D3D library, K10) | 11 % | - |
| 120 | 32.7 | 6 % | 16 % | 2,594 (frames per tick: 1 on 837 ticks) |
| Unlimited | 33.3 | 6 % | 16 % | - |

The render thread is "busy" while using 6 % of a core: it waits, it does not draw. Three thread dumps of a sandboxed
run at 120 (`menu_test.py --gdb`, 50, 56 and 62 s, the title menu up) all show it asleep in the guest's
`KeDelayExecutionThread`, called by the movie player from the frame's draw: `sub_821F5608` (the frame) ->
`sub_821F6B78` -> `sub_821F8960` -> `sub_826E8E78` -> `sub_826E9440` (the movie player, beside its play and stop
functions) -> `sub_82715A90` -> sleep. The player waits inside the frame for its next movie frame (it decodes
against real time, 4.3 Q3), so a frame drawn over the movie cannot finish before the movie advances. Cause: the movie
playback's wait on the render thread; not CPU contention, not the frame clock.
Options for D6 (none made here): render-only frames over a movie skip the player's update and draw its current
texture (the tick's own frame keeps the wait, so movie and audio timing are unchanged); or the player's wait moved
out of the frame. Either needs the movie and its audio checked against the 30 fps run (captures, K14's frame counts).

### 22.6 The frame clock's phase (for D6) [M]
The presentation clock restarts from a tick's start only when it falls behind (K44); otherwise it keeps its phase.
Runs at 120 settled at phases between +0.1 and +5.9 ms: at +5.9 ms a tick's frames are presented 14.3, 8.3, 8.3 and
2.4 ms apart (the tick's own frame at its start, the render-only ones on the clock), and the last render-only frame
leaves the render thread 2.4 ms before the next tick (where the failed k44_120 run's busy steps were). Even pacing
needs the clock aligned with the tick (phase 0) at rates that divide 30 Hz; a pacing change, D6 (K49).

### 22.7 Integrity [V]
SDK patches 0001-0010 apply; the game folder verified before and after (`tools/verify_game_dir.sh`); the 53 save files
under build/ unchanged (sha256 before and after); `tools/check_tracked.py` clean.

## 23. D6 (M7 feature 8, 2026-10-30)

### 23.1 The clock on the tick's schedule (K49) [M]
At frame rates that are a multiple of 30 (60, 90, 120, 240) a tick holds a whole number of frame steps. The
presentation clock now starts each tick's steps on its schedule (`scheduled + k / rate`) instead of keeping the
phase it restarted with (22.6); steps already past when the tick started late belong to the tick's own frame, and
the last step, which falls nanoseconds short of the next tick (n x 1/rate rounded down), is the next tick's
(`--kuf_present_clock_align`, default on; off, and with the K44 fault switch, the D5 clock). Other rates (144, 165,
unlimited) keep the D5 clock.

`timing.k44_120` (400 traced ticks at 120): steps 8.27 / 8.33 / 8.34 ms apart (min / median / max), 4 frames on every
tick, the last render-only frame 24.9 ms after the tick's start (median; max 25.0): 8.3 ms before the next tick
instead of 2.4 at the +5.9 ms phase. The four K44 tests and checks (60, 120, faults) pass; a first version counted
that nanosecond-short step as late on every tick (848 of 848), fixed before commit. `settings.frame_rate_menu`,
`frame_rate_switch` (+ state, checks) passed with the first version. Deferred to the regression: the K44 long run,
presentation and HUD checks at 60/90/120.

### 23.2 The title movie's wait (K47): not changed
The wait is inside `sub_82715A90`, a 1,617-line movie decode / present function called from the frame's draw; a
change needs its reverse engineering (what it updates, what it draws, what the audio follows) and the checks named in
22.5. Not attempted as a quick patch.

## Appendix A: probe scenario (scratch, not committed as a test)
`python3 -I tools/menu_test.py --run-prefix m6d-hz<R> --game-arg=--kuf_profile=true
--game-arg=--video_mode_refresh_rate=<R> tick_rate.toml` (and `--game-arg=--vsync=false`):
```toml
[scenario]
description = "M6D timing probe"
seconds = 150
saves_from_dir = "build/m6-saves/celine-shadows-depths-1"
script = """
500 tap Return
700 press Return
850 press Return
1000 press Down
1100 press Return
1300 repeat 60 until gameplay press Return
@gameplay+300 capture start
@gameplay+300 down W
@gameplay+390 capture walk_90
@gameplay+480 up W
@gameplay+480 capture walk_180
@gameplay+490 down L
@gameplay+515 up L
@gameplay+600 capture turned
@gameplay+900 capture idle_600
@gameplay+1500 capture idle_1200
"""
```
Analysis: `build/runs/m6d-*-tick_rate/user/{run-summary.json,captures/captures.jsonl,profile/}`;
`tools/profile_report.py --from-s/--to-s` over the capture window.
