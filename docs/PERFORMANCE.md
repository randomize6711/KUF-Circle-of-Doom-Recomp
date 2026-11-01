# Performance (M6A; M8: section 7)

How the port's performance is measured, what was found, what was changed, and what remains. Machine of all
measurements: AMD Ryzen 7 5700X3D (8 cores, 16 threads), Radeon RX 9070 XT (Mesa radv), 31 GB, Linux 7.2,
build `linux-amd64-relwithdebinfo`, offscreen sandboxed runs (`tools/run_sandboxed.sh --gpu`) unless noted.

## 1. Methodology

### 1.1 Tools

| Tool | What |
|---|---|
| `--kuf_profile=true` (src/runtime/profiler.cpp) | Per presented frame: when the game's present call began and how long it took; per thread and second: CPU time (with the thread's name); per 100 ms: amdgpu `gpu_busy_percent` (whole GPU, includes the desktop). Written to `<run>/user/profile/` at exit |
| `--kuf_profile_samples=true` | Statistical CPU samples: a perf software CPU-clock event per thread (`perf_event_open` on the process's own threads, unprivileged at `perf_event_paranoid` 2, user-space addresses only), 1000 per second of each thread's CPU time, drained by the profiler thread. No signals: covers threads that block signals (SDL's) and interrupts nothing |
| `--gpu_cp_stats=true` (SDK patch 0006) | Every 10 s, where the GPU command processor thread spent its time (executing packets, waiting for commands, `WAIT_REG_MEM` waits); and every presentation the Vulkan backend skips |
| `run-summary.json` (telemetry, every run) | Process CPU % and RSS per 10 s, frames presented |
| `tools/profile_report.py <run> [--from-context gameplay] [--from-s/--to-s] [--json]` | Frame times (average fps, median, 95th/99th percentile, maximum, frames over 40/50 ms, slowest 1 s and 5 s), CPU per thread group, GPU busy, samples by kind (recompiled guest code `sub_XXXXXXXX`, runtime, GPU backend, driver, C library), by thread group and by function |
| `tools/menu_test.py --game-arg=... [--cache-from <run>]` | Runs a workload with extra options, and optionally with a copy of an earlier run's shader cache (warm cache) |

Not available: `perf` and other profilers (no packages may be installed). Two kernel facts found on the way:
a timer on a thread's CPU clock gets the process SIGKILLed on this 7.2 kernel (K27), and a process CPU-time
timer (`ITIMER_PROF`) lands in whichever thread does not block the signal, misattributing SDL's threads'
time. Hence the perf-event sampler.

### 1.2 Workloads

| Workload | What | Length |
|---|---|---|
| `tests/perf/menu_idle.toml` | title, storage prompt, main menu idle | 100 s (measured from 35 s) |
| `tests/perf/explore_shadows_depths.toml` | CONTINUE from the campaign fixture, walk and turn through Shadow Depths with whatever enemies come | ~4 min of play |
| `tests/gameplay/combat_loot.toml` | the tutorial route into the fight with a dozen lizardmen (K19's workload), then a pick-up sweep | ~10 min |
| the player's session m6-play-1 | real audio device, window, Forest of Embracing to Shadow Depths | 33 min |

### 1.3 Rules

- The game presents at most 30 frames per second (it waits for every second vblank of a 60 Hz guest
  display). At the cap, frame time says whether the game *holds* 30 fps, not how much faster a frame could
  be: improvements are therefore shown as fewer missed frames, lower percentiles and worst windows, and as CPU
  time, never as fps above 30. Capacity is measured separately with the cap removed (section 4).
- GPU utilisation is never taken as a bottleneck on its own: GPU busy is reported next to the command
  processor's time split and the uncapped frame rate.
- Gameplay numbers start 20 s after entering play (`--from-context gameplay`) unless marked otherwise.
- Sandboxed runs have no sound device: SDL's playback thread spins on ALSA's null sink (`SDLAudioP`, one core)
  and the audio is paced by the port (K14). That thread does not exist in this form with a real device and is
  left out of comparisons.

## 2. Results

### 2.1 Frame times

| Workload | Build | fps avg | median | p95 | p99 | worst 1 s |
|---|---|---|---|---|---|---|
| Main menu idle | before | 29.00 | 32.9 ms | 37.4 | 41.8 | 34.7 ms |
| Main menu idle | after | 29.99 | 34.4 ms | 36.7 | 39.0 | 34.6 ms |
| Shadow Depths exploration | before | 23.30 | 37.0 ms | 74.7 | 96.2 | 118.0 ms |
| Shadow Depths exploration | after pacing fix | 29.99 | 33.4 ms | 35.0 | 36.2 | 35.6 ms |
| Shadow Depths exploration | after both fixes | 29.99 | 33.4 ms | 34.8 | 36.0 | 35.5 ms |
| Lizardmen fight (K19) | before | 29.05 | 32.4 ms | 42.6 | 53.5 | 56.3 ms |
| Lizardmen fight (K19) | after both fixes | 30.00 | 33.4 ms | 34.7 | 35.5 | 35.0 ms |
| Exploration, from entering play | after, cold shader cache | 29.96 | 33.5 ms | 34.4 | 35.3 | 59.1 ms |
| Exploration, from entering play | after, warm cache | 29.99 | 33.5 ms | 34.6 | 35.0 | 37.1 ms |
| Fight, from entering play | after, cold shader cache | 29.98 | 33.4 ms | 34.5 | 35.1 | 44.2 ms |
| Fight, from entering play | after, warm cache | 29.99 | 33.4 ms | 34.6 | 35.1 | 36.4 ms |

The player's own session (m6-play-1) held 300 frames per 10 s on the menus and 30.0 fps median (5th
percentile 29.5) in play.

### 2.2 CPU, GPU and memory

Steady state after loading; CPU in % of one core; process CPU from `run-summary.json` includes the
test-environment `SDLAudioP` thread (about 100 %).

| Workload | Build | process CPU | guest threads | GPU command processor | GPU busy (mean) | RSS at end |
|---|---|---|---|---|---|---|
| Main menu idle | before | 142 | 18 | 11 | 16 % | 1,096 MB |
| Main menu idle | after | 176 | 56 | 8 | 13 % | 1,097 MB |
| Exploration | before | ~220 | 88 | 18 | 24 % | 1,415 MB (340 s run) |
| Exploration | pacing fix only | ~322 | | | | 1,439 MB |
| Exploration | after both fixes | ~240 | 113 | 14 | 25 % | 1,439 MB |
| Fight | before | ~213 | 88 | 13 | 27 % | 1,408 MB |
| Fight | after both fixes | ~233 | 108 | 13 | 26 % | 1,418 MB |

Reading the table: before the fixes the game threads were partly stalled (fewer frames); after, the game
runs every frame, and its D3D library spins while it waits for the GPU (section 3.3), so guest CPU is higher
while frame pacing is right. The "pacing fix only" row is the audio worker spinning on a whole core (fixed by
3.2). Memory is unchanged by the fixes: RSS rises while areas load and then levels off (+122 MB over 33 min and
three areas in m6-play-1; no growth during the fixed workloads). GPU load at 30 fps is low (13-27 %).

## 3. Findings and changes

**3.1 Silent-run audio pacing slowed the game (fixed).** The old `--kuf_audio_pace` slept inside `SubmitFrame`,
which the game calls from its own audio callback: the game's audio work stalled up to 5.3 ms per audio frame,
and the game thread waited on it. Effects: menus 290 frames per 10 s instead of 300; exploration 23.3 fps with
118 ms worst seconds; the fight 29 fps with 56 ms worst seconds; this also explains K19's 21-25 fps, which was
measured in silent runs. Now the pacer works like a device: the SDL driver's "frame played" releases go to a
pacing thread, which passes them on at 48 kHz real time to the semaphore the SDK's audio worker waits on; the
game's callback never blocks (src/runtime/audio_meter.cpp). Only silent sandbox runs use it.

**3.2 Runtime: alertable waits on several objects spun a core (fixed, SDK patch 0007).** `WaitMultiple` slept
`min(remaining, 1 ms)` with the remaining time truncated to whole milliseconds, i.e. not at all in the last
millisecond; alertable waits poll in 1 ms slices, so they never slept. The SDK's audio worker used 80 % of a
core; after the fix 0.7 %. Guest `KeWaitForMultipleObjects` uses the same path. With a real device the audio
worker waits the same way, so the saving applies to normal play (to be confirmed in a played session).

**3.3 The 30 fps cap and where the guest waits (characterised, unchanged).** Per frame the GPU command
processor executes one `WAIT_REG_MEM` on a memory value the game writes (the game's frame-pacing fence: 300
waits per 10 s, 8.4 s of 10 s sleeping in 1 ms steps during exploration). Meanwhile the game's render thread
fills the ring buffer and then spins in its D3D library waiting for ring space (`sub_821647B0` ->
`sub_82160C90`, a yield loop with a 5 s timeout): the hottest guest code in every capped profile, 17-20 % of all
samples. This is how the cap looks from inside, not lost work: with vsync off both waits disappear (section 4).

**3.4 Shader and pipeline creation (K26, measured).** With an empty cache, which every sandboxed run has, the
runtime creates pipelines during the first minutes of play; with asynchronous compilation it skips presenting
a frame that used a placeholder, and the guest also sees some slow frames: the fight 68 skipped presentations
and 30 guest frames over 50 ms, exploration 50 and 28. With the cache of the previous run (`--cache-from`): 227
pipelines created from it at start, 3 / 1 skipped presentations, 3 / 2 frames over 50 ms. The cache lives in
`<user data>/cache` and persists between sessions, so a player sees these hitches the first time a place or
effect appears. Unchanged in M6A (strategies: section 5).

## 4. Capacity beyond the 30 fps cap

With `--vsync=false` the guest gets 1000 vblanks per second and the game runs as fast as it can (its
simulation advances per frame, so it also plays faster: a measurement, not a mode to play in). Fight workload:

| | value |
|---|---|
| fps during the fight (10 s intervals) | 123-198 |
| fps afterwards (same area, enemies dead) | 163-165 |
| frame time median / 95th / 99th | 6.0 / 7.2 / 8.4 ms |
| GPU command processor thread | 94 % of a core; 9.7 s of 10 s executing packets, no waits |
| GPU busy | 84 % (median 85 %) |
| guest threads | 138 % of a core in total |

At 30 fps the port uses roughly a quarter of its frame time. The first limit above the cap is the runtime's
GPU command processor (one thread translating the game's command stream: register writes `WriteRegister` 6 %
of its samples, draw setup); the GPU itself is close behind. The recompiled game code is not the limit.

## 5. Remaining bottlenecks, and what was not changed

| Item | Evidence | Why unchanged | Where next |
|---|---|---|---|
| GPU command processor is single-threaded | 94 % of a core uncapped | no effect at 30 fps; a larger change in the SDK | M6E (higher frame rates): profile `WriteRegister` and draw setup, cache register state, consider splitting translation and submission |
| D3D ring-buffer spin and the per-frame `WAIT_REG_MEM` | 17-20 % of samples; guest CPU 56 % of a core on an idle menu | the engine's own pacing; replacing the spin with a sleep would change timing | M6E (frame pacing is redesigned there anyway) |
| `WAIT_REG_MEM` 1 ms sleep granularity | p99 36 ms in exploration | costs nothing measurable at 30 fps | revisit with uncapped frame rates |
| Pipeline creation hitches on a cold cache (K26) | section 3.4 | the cache already removes them after the first time | M6F options below |
| Memory growth per area | +122 MB over 33 min | levels off; no leak seen in fixed workloads | the 2-hour stability run (M6 plan) |
| Test-only: `SDLAudioP` spin in silent runs | one core | not present with a real device; affects only sandbox CPU numbers | none needed |

K26 strategies for later (none implemented in M6A; no game content may be shipped, so a pre-built cache from
the game's shaders cannot be distributed):

1. **Precompile the known pipelines at start, before play** (the runtime already does this for the cached
   ones: 227 at start); make it non-blocking but complete before the title screen ends.
2. **Compile pending pipelines during loading screens** synchronously, where a pause is invisible, instead of
   asynchronously during play.
3. **Keep the driver's own cache warm**: Mesa's shader cache (`~/.cache/mesa_shader_cache`) persists for a
   player; sandboxed test runs start without it (their home is per run), which overstates first-run hitches.
4. **Shorter pipeline creation**: graphics pipeline libraries (`VK_EXT_graphics_pipeline_library`) to link
   pipelines from pre-compiled stages quickly, where supported.
5. **Do not skip presentation** for placeholder draws (draw the frame with the placeholder or without the new
   object instead of not presenting): fewer visible hitches, possible one-frame visual differences.

## 6. Native render resolution (M6C follow-up)

The render scale multiplies the pixels of the game's whole frame (docs/DISPLAY.md section 1); the game's
simulation and its 30 fps cap are unchanged. Workload: `tests/perf/explore_shadows_depths.toml` with
`--kuf_profile=true --gpu_cp_stats=true`, the output simulated with `--kuf_display_size`, numbers from 20 s after
entering play (`profile_report.py --from-context gameplay`, about 347 s, 10,400 frames per run); every run starts
with an empty shader cache (sandboxed runs); VRAM is amdgpu `mem_info_vram_used` sampled each second from outside
the sandbox, minus the idle desktop (3.64 GB). Runs `build/runs/nperf-*` (2026-10-24, commit 6e9200c).

### 6.1 At the cap (what the player gets)

| Output | Render | Rendered frame | fps | frame median / p95 / p99 / max (ms) | >40 ms | guest CPU | GPU command thread | GPU busy mean / p95 | VRAM (game) |
|---|---|---|---|---|---|---|---|---|---|
| 1280x720 | Native = 1x | 1280x720 | 29.99 | 33.4 / 34.7 / 36.3 / 78 | 31 | 112 % | 18 % | 25 / 29 % | 0.95 GB |
| 3840x2160 | 1x (output scaling only) | 1280x720 | 29.99 | 33.3 / 34.6 / 36.1 / 77 | 38 | 112 % | 15 % | 23 / 29 % | 0.95 GB |
| 1920x1080 | Native (2x2) | 2560x1440 | 29.98 | 33.3 / 34.7 / 36.8 / 89 | 38 | 113 % | 20 % | 33 / 39 % | 2.17 GB |
| 2560x1440 | Native (2x2) | 2560x1440 | 29.98 | 33.4 / 34.7 / 36.1 / 86 | 32 | 112 % | 18 % | 39 / 46 % | 2.08 GB |
| 3440x1440 | Native (3x2) | 3840x1440 | 29.98 | 33.3 / 34.7 / 36.3 / 89 | 27 | 111 % | 17 % | 50 / 54 % | 2.60 GB |
| 3840x2160 | Native (3x3) | 3840x2160 | 29.97 | 33.5 / 34.7 / 36.2 / 96 | 35 | 113 % | 19 % | 56 / 66 % | 3.17 GB |
| 5120x1440 | Native (4x2) | 5120x1440 | 29.96 | 33.5 / 35.5 / 37.7 / 94 | 49 | 114 % | 20 % | 58 / 67 % | 2.94 GB |

- Every configuration holds 30 fps; frame-time percentiles are those of the cap at every resolution. The guest's
  CPU time (the game) does not change with the render scale; the GPU command thread stays at 15-20 % of a core.
- **Native rendering versus output scaling**: rendering the 1280x720 frame for a 4K output costs the same as for
  a 720p one (GPU 23 vs 25 % busy, same VRAM): the output size alone costs nothing measurable. The presenter's
  scaling to a real window (one full-screen pass) is not exercised by offscreen runs. Native rendering's cost is
  the scaled frame: GPU busy rises with the rendered pixels (25 % at 0.9 MP to 56-58 % at 7.4-8.3 MP; busy is
  sampled at whatever clock the GPU picked, so it is only indicative: 1080p and 1440p render the same frame but
  read 33 and 39 %).
- VRAM: about 1 GB at 1x, 2.1-2.2 GB at 2x2, 2.6 GB at 3x2, 2.9-3.2 GB at 4x2 / 3x3 (render targets, the scaled
  resolve buffer and the textures made from resolves grow with the scale).
- The single slow frames (max 77-96 ms, 27-49 frames over 40 ms in 10,400) occur at 1x as well: pipeline creation
  with an empty shader cache (the "Vulkan Pipeline" thread is busy then), not the render scale.

### 6.2 Capacity (cap removed with `--vsync=false`, measurement only; frame-rate work is a later milestone)

| Rendered frame | fps | frame median / p95 / p99 (ms) | guest CPU | GPU command thread | GPU busy |
|---|---|---|---|---|---|
| 1280x720 (1x) | 305.8 | 3.1 / 4.2 / 5.2 | 184 % | 91 % | 85 % |
| 2560x1440 (2x2: 1080p, 1440p) | 170.7 | 5.3 / 10.7 / 11.9 | 170 % | 89 % | 98 % |
| 3840x1440 (3x2: 3440x1440) | 128.0 | 5.3 / 14.5 / 19.1 | 152 % | 64 % | 97 % |
| 3840x2160 (3x3: 4K) | 99.5 | 8.5 / 19.5 / 24.0 | 133 % | 35 % | 99 % |
| 5120x1440 (4x2: 32:9) | 99.6 | 6.4 / 19.3 / 22.5 | 142 % | 61 % | 99 % |

- At 1x the game is limited by the guest CPU and the GPU command thread; from 2x2 on it is GPU-bound (97-99 %
  busy). The average frame costs 3.3 ms at 1x, 5.9 ms at 2x2, 7.8 ms at 3x2 and 10.0 ms at 3x3 / 4x2, i.e. about
  0.9 ms per additional million rendered pixels on this GPU: native 4K uses about a third of the 33 ms frame of
  the 30 fps cap.

## 7. M8: performance and stability optimisation (2026-10-31)

Branch `m8/performance` (from `m7/feature-completion` 3eb605e). Baseline build: commit 7024ad4 (the M7 code plus
the M8 measurement tools; the tools change nothing unless their options are given), frozen as
`build/m8-frozen-base/bin` so the main tree could be rebuilt while the baseline ran. Final build: commit 99fdccb
(`build/m8-frozen-final/bin`). While the baseline ran, small builds of the main tree ran at the lowest priority
with a 2-core quota (during the GPU-bound 4K runs).

### 7.1 Environment

| | |
|---|---|
| CPU | AMD Ryzen 7 5700X3D (8 cores, 16 threads) |
| GPU | AMD Radeon RX 9070 XT (RADV GFX1201), Mesa 26.2.5, Vulkan 1.4.354 |
| RAM | 31 GB (the desktop and a browser running beside the tests) |
| OS | Arch-based (omarchy), Linux 7.2.8 |
| Build | `linux-amd64-relwithdebinfo` (clang, -O2, x86-64-v2) |
| Runs | `tools/run_sandboxed.sh --gpu` through `tools/bench.py`: offscreen (no swapchain, so no VSync and no presentation to a display: the frame rate is limited only by the game's pacing and the frame-rate setting), the output size simulated with `--kuf_display_size`, Render Native, no sound device (the test-only `SDLAudioP` thread spins on ALSA's null sink and is left out of the CPU totals, K10) |
| Isolation | read-only game folder, no network, no display or audio sockets, fresh saves and settings per run (saves copied from `build/m6-saves`), a hard timeout, and a transient systemd user scope per run (MemoryMax 10 GB, CPUQuota 12 cores) |

### 7.2 Tools (new in M8)

| Tool | What |
|---|---|
| `tools/bench.py run/analyze/report` | Runs a workload over output sizes and frame rates (`m8-<tag>-<size>-<fps>-r<n>-<workload>`), samples VRAM (amdgpu `mem_info_vram_used` minus the reading before the run) and the game's RSS from outside every 0.5 s, and reports per measured window: presented frames (average, median, 1 % and 0.1 % lows = fps over the slowest 1 % / 0.1 % of frame times), frame-time mean / p95 / p99 / max, frames over twice the median, frame-time standard deviation and jitter (mean change between consecutive frame times), CPU per thread group, GPU busy, GPU time per frame, GPU completion intervals, the GPU sections, render-only frames dropped, pipelines created and presentations skipped for them, boot and world-load times, peak RSS and VRAM, exit status |
| `tools/bench_m8_matrix.sh <tag> [cold|warm|all] [preset]` | The M8 matrix (7.3) |
| SDK patch 0012 `--gpu_timestamp_stats` | GPU time per guest frame from Vulkan timestamps at the start and end of every submission; GPU frame completion intervals (the offscreen stand-in for when frames would reach a display); a read-only cvar `gpu_recent_frame_us` (recent GPU time per frame) |
| SDK patch 0012 `--gpu_timestamp_sections` | The GPU time of each frame split into draws, render target transfers (EDRAM ownership copies of the host render target path), resolves and texture loads; with the GPU trace (`--gpu_trace_frame`) each transfer is logged |
| `tools/stability.py` | Repeated sandboxed stages (7.9) judged by process health, unexpected log errors and the memory trend |
| `tests/perf/bench_menu.toml`, `bench_explore.toml`, `bench_fight.toml` | The workloads (7.3) |
| `tests/perf/render_equivalence.toml` | One route with captures at fixed ticks, to compare a GPU change with its control |
| `tests/stability/*.toml` | 30 minutes of play at 120 fps; frame-rate limit switched every 5 s (the test script's new `framerate` verb); window resized every 4 s |

### 7.3 Workloads, matrix and how to read the numbers

| Workload | Route (script times are game ticks, the same at every frame rate) | Measured windows |
|---|---|---|
| `bench_menu` | cold start, title, storage question, main menu idling over its animated background (105 s) | 35-98 s |
| `bench_explore` | CONTINUE from the Shadow Depths fixture (world load), then the M6A walk-and-turn route (225 s) | 140 s of play (`@gameplay+600..+4800`) |
| `bench_fight` | new game, the tutorial forest, the status and inventory screens, the lizardmen fight (315 s; the K24 drop seed) | the status/inventory screens and back (`menus`, 10 s); 99 s of fighting (`fight`) |

The matrix (`tools/bench_m8_matrix.sh`): with an empty pipeline cache (`<tag>-cold`) the three workloads at 3840x2160,
30 and 120 fps, once; with a warm cache (a copy of a cold run's) exploration and the fight at 3840x2160 for 30, 60,
120 and 144 fps twice, at 2560x1440 and 1920x1080 once each, and the menu at 3840x2160 for 30 and 120 twice: 39 runs,
about 2.5 hours. The same matrix ran on the baseline build (`base`) and the final build (`final`).

Reading the numbers:
- **Presented frames** are the guest's present calls (frames.csv). Every frame above 30 per second is a render-only
  frame drawn at its own moment between two game ticks (interpolated camera, characters, objects, effect shaders: they
  are distinct images, not repeats); particles step at 30 Hz (K39). The game always runs 30 ticks per second.
- **1 % / 0.1 % low** = 1000 / the mean of the slowest 1 % / 0.1 % frame times of the window. **Jitter** = the mean
  change between consecutive frame times: frames delivered in bursts (5 and 21 ms alternately) have a large jitter at
  the same average.
- **Offscreen runs have no display**: no VSync, no swapchain, no presentation of the scaled frame to a window. The
  GPU completion interval (patch 0012) is the stand-in for when frames would reach a display.
- **1920x1080 and 2560x1440 render the same 2560x1440 frame** at Render Native (whole multiples of the game's
  1280x720); only a real display adds the final scaling pass, which offscreen runs do not draw. 3840x2160 renders
  3840x2160.
- **GPU ms per frame** is GPU execution time from timestamps. The GPU lowers its clocks when lightly loaded, so at
  30 fps a frame reads longer (16 ms at 4K) than at 120 (10-12 ms): compare it only between runs at the same frame rate.
  **GPU busy** (amdgpu) includes the desktop and is only indicative.
- **CPU %** is per thread group in % of one core, from /proc; the test-only `SDLAudioP` thread (ALSA's null sink, about
  one core) is left out of the totals. The GPU command processor thread's CPU time includes yielding while it waits,
  so it grows into time it does not need.
- **The menu** runs at 33 fps whatever the limit: the title movie's wait on the render thread (K47).
- **The fight is not deterministic** (K24): its enemies and effects differ between runs, so its spread is wider.
- **Peak VRAM** is amdgpu's used VRAM minus its reading before the run (other desktop activity shifts it by tens of MB).


### 7.4 Findings and changes

Every change was measured against the same build with the change switched off (one change per comparison), and kept
only with evidence. Runs at Render Native with a warm pipeline cache unless marked; "n" is the number of runs.

**7.4.1 Render target transfers (K73): kept.** `--gpu_timestamp_sections` split a 4K gameplay frame (120 limit,
Shadow Depths) into draws 8.2 ms, render target transfers 3.7 ms (19 per frame), resolves 1.5 ms (12 per frame).
The GPU trace of one frame listed each transfer: small render targets (a 560-pixel-wide depth buffer at EDRAM tile 224,
the 4x MSAA depth passes at 720) received copies of the whole remaining EDRAM (tiles 720..2048, 1.7 million guest
pixels before the 3x3 scale), i.e. the render target cache took the game's unclipped draws (scissor 8192, K34) to
reach the end of the EDRAM. ReXGlue ships `execute_unclipped_draw_vs_on_cpu=false`; Xenia's default is true, which
runs those draws' vertex shaders on the CPU to find their real extent. The port now defaults it to true
(src/runtime/gpu_defaults.cpp). Render equivalence (tests/perf/render_equivalence.toml, 11 captures from the title
menu to the pause menu, 4K): the control pair (the option off twice) is bit-identical; with the option, 3 gameplay
captures differ by at most 2 levels at a few pixels (mean difference 0.0001; no pixel beyond 2), consistent with
fewer lossy round trips through other render targets' formats. Effect at 3840x2160:

| 120 limit, Shadow Depths | before (n = 2) | after (n = 2) | change |
|---|---|---|---|
| average fps | 77.6 | 90.0 | +16 % |
| 1 % low fps | 33.2 | 42.9 | +29 % |
| 0.1 % low fps | 17.8 | 32.9 | +85 % |
| p99 frame time | 26.1 ms | 22.2 ms | -15 % |
| frame-time jitter | 10.3 ms | 5.9 ms | -43 % |
| GPU ms per frame | 12.0 | 10.1 | -16 % |
| transfers ms per frame | 3.7 (one run) | 1.4 | |
| GPU command thread CPU per frame | 0.67 % per fps | 0.70 % per fps | +5 % (the vertex shaders run on the CPU) |

At 30 fps the frame times are unchanged (the cap) and the GPU time per frame falls 16.2 -> 13.5 ms. (The commit
message of d0838a7 compares with 76.4 fps: that mean included one cold-cache run; the warm-cache figures are above.)

**7.4.2 Frames bunched behind the tick's frame: kept (`--kuf_present_clock_from_handoff`).** At 60 fps with GPU time to
spare (2560x1440) the frames alternated about 14 and 19.5 ms apart (jitter 6-7 ms at every output size). The clock on
the tick's schedule (K49) placed the render-only frames from the tick's scheduled start, but the tick's own frame
reaches the renderer several ms later (the tick's work and its scene build), so it came late and the next frame early.
The render-only frames now step from the hand-off of the tick's frame, drawn slightly closer together when the
hand-off came so late that the last one would not fit before the next tick. Same build, option off / on (n = 1 each):

| | 1440p 60 explore | 4K 60 explore | 1440p 60 fight | 1440p 120 explore | 4K 120 explore |
|---|---|---|---|---|---|
| jitter off -> on (ms) | 6.3 -> 3.0 | 6.8 -> 3.4 | 4.7 -> 2.4 | 2.4 -> 1.7 | 5.9 -> 5.2 |
| frame-time stdev (ms) | 3.5 -> 2.2 | 3.7 -> 2.1 | 2.4 -> 2.2 | 2.6 -> 2.7 | 4.7 -> 4.6 |
| GPU completion interval stdev (ms) | 3.62 -> 2.07 | 3.07 -> 1.87 | 2.35 -> 2.33 | 2.28 -> 2.05 | 1.41 -> 1.52 |
| average fps | 59.9 -> 59.9 | 60.0 -> 60.0 | 60.0 -> 59.8 | 108.9 -> 106.9 | 90.0 -> 90.3 |

At 60 the improvement is clear; at 120 the differences are within run-to-run variation.

**7.4.3 Adaptive frame rate: opt-in (`--kuf_frame_rate_adaptive`, default off).** When the GPU cannot draw the limit's
frames (4K at 120: about 10-12 ms of GPU time per frame), steps are dropped as "render busy" and the frames that are
drawn come in bursts (alternately about 5 and 21 ms apart in the baseline). The option draws the largest multiple of 30
the GPU sustains (src/runtime/adaptive_rate.h, unit-tested; the GPU time from patch 0012's `gpu_recent_frame_us`).
At 4K with a 120 limit (n = 2): an even 60 fps (jitter 2.8-3.1 ms, stdev 1.9-2.0 ms, 2-3 frames dropped per run)
against about 90 fps with 5-9 ms jitter and 2,600-4,600 dropped steps; 1 % lows 41-43 against 39-43. Whether an even 60 or
an uneven 90 looks better depends on the display (a VRR display shows the uneven frames when they are ready) and the
player, so it is not the default. It suits slower GPUs aiming at a stable 60.

**7.4.4 Register writes: reverted.** The CPU samples (1440p, 120 limit) put a third of the GPU command processor
thread's time in register writes, 6 % in `RegisterFile::GetRegisterInfo`, a lookup made on every write only for a
debug message. Two variants (checking the log level first; a bitmap of known registers built once) were measured:
the command thread's samples per frame 5.73 -> 5.92 / 5.73 and 107.0 -> 107.3 / 106.6 fps: no measurable gain (the
thread spends the time it saves waiting), so the SDK patch was not kept. Found on the way: `profile_report.py`
symbolised profiles with whatever binaries were in the build directory; it now warns when they are newer than the
profile.

**7.4.5 Investigated, unchanged.**
- *Recompiled code dispatch*: indirect calls go through a flat per-module table (one bounds check, one load,
  `REX_CALL_INDIRECT_FUNC`); not visible in the profiles.
- *Busy waiting*: the game's render thread spends about 30 % of its samples in the D3D library's ring-space wait
  (`sub_82160C90`, `sub_821647B0`) when the GPU or the command processor is the limit (as in M6A); at frame rates above
  30 the port turns the SDK's `vsync` off and `WAIT_REG_MEM` yields instead of sleeping in 1 ms steps, so there is no
  sleep-granularity loss above 30 fps (the cost is CPU time). Not changed: it is the engine's own pacing.
- *Texture loads*: 11 per frame at 4K, 2.4-3.1 ms of GPU time: every resolve (render to texture) is resolved into the
  scaled resolve buffer (1.3-2.3 ms) and loaded back into a texture. The largest remaining GPU cost after the draws;
  changing it means changing the SDK's resolve and texture load paths (next GPU target, section 7.8).
- *Loading*: world load (CONTINUE to gameplay) 10.1-10.2 s, boot to the title 10.6-11.2 s, in every configuration.
  No thread is busy while loading (the busiest guest thread 19 % of a core). With the tick uncapped (diagnostic
  `--kuf_fixed_tick=false --vsync=false`) the world load takes 3.1 s and the boot 4.2 s: loading is paced per game tick.
  A faster loading mode (ticks uncapped only while a loading screen is up) could save about 7 s per area change, but
  needs the game's loading state identified and checks that nothing timed by ticks (scripts, the intro, the publisher
  logos, music) changes; not done in M8 (section 7.8).
- *Shader and pipeline stalls* (K26): with an empty cache 107 pipelines were created in the measured 140 s of play at
  4K and 27 presentations skipped for placeholder pipelines (0.1 % low 11.7-14.2 fps). The "warm" runs start with a
  copy of one cold run's cache and still created 0-59 pipelines and skipped 0-15 presentations per window (that cache
  came from another frame rate's or output's run and did not hold every pipeline). A player's own cache persists and
  grows across sessions; the remaining hitches are the first sight of a pipeline (K26 strategies, section 5).
- *Memory*: resident memory 1.38-1.42 GB in play, 1.08 GB on the menus, at every output size and frame rate; VRAM
  about 1.9 GB over the idle desktop at 1440p and 3.0-3.3 GB at 4K. No growth in any measured window (section 7.7 for
  the long run).
- *Co-op disconnect crash* (K72): static analysis only, unverified (no multiplayer sessions in M8).

### 7.5 Results: baseline and optimised

**Primary configuration**: 3840x2160 (Render Native), 120 fps limit, Shadow Depths exploration, the measured 140 s
window, warm pipeline cache; baseline build n = 2, final build n = 2 (about 10,800 and 12,600 frames per run).

| Metric | Baseline | Optimized | Change |
|---|---|---|---|
| Average FPS | 77.6 | 90.1 | +16.1 % |
| Median frame time | 10.74 ms | 10.00 ms | -6.9 % |
| 1 % low FPS | 33.2 | 43.0 | +29.3 % |
| 0.1 % low FPS | 17.8 | 34.9 | +96 % |
| Average frame time | 12.88 ms | 11.10 ms | -13.8 % |
| 95th-percentile frame time | 23.6 ms | 20.0 ms | -15.1 % |
| 99th-percentile frame time | 26.1 ms | 22.3 ms | -14.5 % |
| Worst frame | 113.3 ms | 45.4 ms | -60 % |
| Frame-time jitter | 10.3 ms | 5.4 ms | -48 % |
| CPU usage (process, % of one core, without the test-only audio thread) | 176 % | 180 % | +2.4 % |
| CPU: guest threads / GPU command thread | 110 % / 52 % | 104 % / 62 % | -5.7 % / +21 % (at +16 % frames) |
| GPU frame time (timestamps) | 11.97 ms | 10.12 ms | -15.4 % |
| GPU busy (amdgpu, whole GPU) | 98 % | 96 % | -2 points |
| GPU frame completion interval p99 / stdev | unavailable (not measured by the baseline build) | 15.1 ms / 1.4 ms | n/a |
| Peak memory (RSS) | 1,409 MB | 1,404 MB | -0.3 % |
| Peak VRAM (over the idle desktop) | 3,019 MB | 3,002 MB | -0.6 % |
| World load (CONTINUE to gameplay) | 10.17 s | 10.18 s | +0.1 % (unchanged) |
| Boot to the title | 11.14 s | 11.16 s | +0.2 % (unchanged) |

The 120 fps target at 4K was not reached (7.6). The other cells of the matrix (n = 1 at 1920x1080 and 2560x1440; the
fight differs between runs, K24):

**Baseline (warm cache):**

| Workload | Output | Limit | n | avg fps | 1 % low | 0.1 % low | p99 ms | jitter ms | GPU ms/frame | CPU guest % | CPU GPU-cmd % |
|---|---|---|---|---|---|---|---|---|---|---|---|
| explore | 1920x1080 | 30 | 1 | 30.0 | 27.2 | 21.6 | 35.0 | 1.1 | 9.0 | 113 | 17 |
| explore | 1920x1080 | 60 | 1 | 59.8 | 34.3 | 15.4 | 22.7 | 6.6 | 8.6 | 63 | 48 |
| explore | 1920x1080 | 120 | 1 | 109.4 | 48.0 | 35.5 | 19.2 | 2.6 | 6.9 | 92 | 69 |
| explore | 1920x1080 | 144 | 1 | 114.7 | 48.5 | 37.6 | 19.1 | 1.7 | 6.7 | 104 | 77 |
| explore | 2560x1440 | 30 | 1 | 30.0 | 27.2 | 22.2 | 35.1 | 1.2 | 8.9 | 114 | 21 |
| explore | 2560x1440 | 60 | 1 | 60.0 | 39.1 | 24.0 | 22.7 | 6.7 | 8.5 | 59 | 46 |
| explore | 2560x1440 | 120 | 1 | 95.3 | 45.0 | 29.1 | 20.3 | 2.5 | 7.5 | 101 | 73 |
| explore | 2560x1440 | 144 | 1 | 119.5 | 51.9 | 36.1 | 15.7 | 1.7 | 6.7 | 101 | 74 |
| explore | 3840x2160 | 30 | 2 | 30.0 | 27.1 | 20.6 | 35.0 (35.0–35.1) | 1.1 | 16.2 (16.1–16.2) | 113 | 19 (18–20) |
| explore | 3840x2160 | 60 | 2 | 60.0 | 40.8 (40.1–41.5) | 26.9 (24.9–29.0) | 22.3 (22.2–22.4) | 6.3 (5.8–6.8) | 13.0 | 60 (53–66) | 47 (43–51) |
| explore | 3840x2160 | 120 | 2 | 77.6 (77.5–77.8) | 33.2 (32.4–34.1) | 17.8 (15.5–20.0) | 26.1 (26.0–26.2) | 10.3 | 12.0 | 110 | 52 |
| explore | 3840x2160 | 144 | 2 | 77.5 (77.3–77.6) | 35.7 (34.3–37.1) | 24.3 (18.8–29.7) | 25.9 (25.7–26.0) | 10.3 (10.0–10.6) | 12.1 (12.0–12.2) | 110 | 52 (51–53) |
| fight (fight) | 1920x1080 | 30 | 1 | 30.0 | 24.1 | 14.9 | 35.6 | 1.4 | 9.5 | 111 | 26 |
| fight (menus) | 1920x1080 | 30 | 1 | 30.0 | 27.4 | 26.9 | 35.2 | 1.3 | 8.7 | 103 | 20 |
| fight (fight) | 1920x1080 | 60 | 1 | 59.9 | 36.2 | 18.6 | 20.5 | 3.7 | 7.2 | 41 | 37 |
| fight (menus) | 1920x1080 | 60 | 1 | 60.0 | 47.2 | 40.8 | 20.3 | 3.9 | 6.5 | 35 | 34 |
| fight (fight) | 1920x1080 | 120 | 1 | 110.0 | 67.4 | 56.6 | 14.0 | 1.2 | 7.1 | 106 | 81 |
| fight (menus) | 1920x1080 | 120 | 1 | 120.0 | 85.4 | 61.7 | 11.0 | 1.6 | 6.5 | 95 | 78 |
| fight (fight) | 1920x1080 | 144 | 1 | 141.8 | 62.5 | 23.5 | 11.1 | 1.7 | 5.4 | 75 | 59 |
| fight (menus) | 1920x1080 | 144 | 1 | 142.8 | 87.0 | 58.8 | 10.5 | 1.5 | 5.4 | 77 | 60 |
| fight (fight) | 2560x1440 | 30 | 1 | 30.0 | 24.6 | 13.0 | 35.4 | 1.3 | 7.4 | 97 | 12 |
| fight (menus) | 2560x1440 | 30 | 1 | 30.0 | 28.8 | 28.7 | 34.6 | 1.0 | 7.4 | 107 | 10 |
| fight (fight) | 2560x1440 | 60 | 1 | 59.9 | 34.2 | 16.8 | 21.8 | 5.2 | 8.3 | 67 | 52 |
| fight (menus) | 2560x1440 | 60 | 1 | 60.0 | 43.4 | 39.0 | 21.7 | 5.5 | 8.0 | 64 | 51 |
| fight (fight) | 2560x1440 | 120 | 1 | 106.7 | 63.6 | 45.1 | 14.4 | 1.4 | 7.3 | 107 | 80 |
| fight (menus) | 2560x1440 | 120 | 1 | 118.2 | 77.2 | 71.7 | 12.4 | 0.7 | 6.7 | 110 | 88 |
| fight (fight) | 2560x1440 | 144 | 1 | 110.3 | 59.9 | 29.5 | 14.3 | 1.4 | 7.1 | 107 | 80 |
| fight (menus) | 2560x1440 | 144 | 1 | 118.9 | 80.3 | 70.2 | 11.2 | 0.8 | 6.7 | 111 | 88 |
| fight (fight) | 3840x2160 | 30 | 2 | 30.0 | 27.5 (26.8–28.2) | 22.0 (17.0–27.0) | 35.0 (34.8–35.2) | 1.1 (1.1–1.2) | 16.5 (16.2–16.8) | 109 | 14 (14–15) |
| fight (menus) | 3840x2160 | 30 | 2 | 30.0 | 28.2 (28.1–28.3) | 27.8 | 35.0 (34.8–35.1) | 1.1 (1.0–1.2) | 14.9 (13.7–16.0) | 108 | 12 |
| fight (fight) | 3840x2160 | 60 | 2 | 60.0 | 41.2 (39.2–43.2) | 29.1 (21.2–37.0) | 21.6 (21.4–21.8) | 4.9 (4.7–5.1) | 13.2 | 65 (63–66) | 51 |
| fight (menus) | 3840x2160 | 60 | 2 | 60.0 | 44.8 (44.6–45.0) | 37.7 (35.0–40.5) | 20.1 (19.8–20.4) | 4.2 (3.7–4.6) | 13.3 (13.1–13.5) | 57 (53–60) | 49 (46–51) |
| fight (fight) | 3840x2160 | 120 | 2 | 79.9 (73.1–86.7) | 36.9 (34.6–39.3) | 23.3 (21.5–25.2) | 24.9 (22.6–27.2) | 13.2 (11.9–14.6) | 11.7 (10.8–12.7) | 104 (99–108) | 40 (39–40) |
| fight (menus) | 3840x2160 | 120 | 2 | 84.5 (82.1–86.9) | 41.0 (37.8–44.2) | 32.4 (21.3–43.5) | 22.9 (22.4–23.3) | 12.3 (11.6–13.1) | 11.0 (10.9–11.2) | 102 (99–105) | 41 |
| fight (fight) | 3840x2160 | 144 | 2 | 78.5 (71.7–85.4) | 37.5 (35.2–39.9) | 30.5 (26.3–34.6) | 25.6 (23.0–28.1) | 11.5 (10.7–12.4) | 12.0 (10.9–13.2) | 107 (103–110) | 47 (40–54) |
| fight (menus) | 3840x2160 | 144 | 2 | 77.0 (71.7–82.2) | 39.5 (36.4–42.5) | 38.9 (35.7–42.1) | 25.3 (23.3–27.3) | 13.5 | 12.1 (11.1–13.1) | 107 (106–108) | 43 (39–47) |
| menu | 3840x2160 | 30 | 2 | 30.0 | 25.3 (25.2–25.3) | 24.6 (24.5–24.7) | 38.9 (38.9–39.0) | 3.6 | 4.2 | 50 (48–52) | 12 |
| menu | 3840x2160 | 120 | 2 | 32.9 (32.8–33.1) | 26.3 | 25.8 | 37.4 (37.3–37.5) | 8.7 (8.4–8.9) | 4.1 | 19 | 16 |

| Workload | Output | Limit | boot to title s | world load s | peak RSS MB | peak VRAM over idle MB | pipelines created / presentations skipped (measured window) |
|---|---|---|---|---|---|---|---|
| explore | 1920x1080 | 30 | 11.1 | 10.2 | 1415 | 1938 | 6 / 3 |
| explore | 1920x1080 | 60 | 11.1 | 10.2 | 1415 | 1945 | 59 / 14 |
| explore | 1920x1080 | 120 | 11.1 | 10.2 | 1400 | 1936 | 24 / 7 |
| explore | 1920x1080 | 144 | 11.1 | 10.2 | 1406 | 1937 | 24 / 7 |
| explore | 2560x1440 | 30 | 11.1 | 10.1 | 1404 | 1937 | 24 / 6 |
| explore | 2560x1440 | 60 | 11.1 | 10.2 | 1402 | 1936 | 12 / 6 |
| explore | 2560x1440 | 120 | 11.1 | 10.2 | 1400 | 1938 | 8 / 5 |
| explore | 2560x1440 | 144 | 11.1 | 10.2 | 1402 | 1937 | 6 / 3 |
| explore | 3840x2160 | 30 | 11.1 | 10.2 | 1404 | 2969 | 27 (26–28) / 8 (7–8) |
| explore | 3840x2160 | 60 | 11.1 | 10.2 | 1403 | 3003 (2999–3007) | 3 (0–6) / 2 (0–3) |
| explore | 3840x2160 | 120 | 11.1 | 10.2 | 1409 (1405–1412) | 3019 (3005–3034) | 48 (39–57) / 13 (11–15) |
| explore | 3840x2160 | 144 | 11.1 | 10.2 | 1414 (1412–1416) | 3008 (3005–3010) | 18 (8–27) / 8 (5–11) |
| fight | 1920x1080 | 30 | 11.2 | n/a | 1400 | 1941 | 22 / 6 |
| fight | 1920x1080 | 60 | 11.2 | n/a | 1403 | 1942 | 20 / 5 |
| fight | 1920x1080 | 120 | 11.2 | n/a | 1385 | 1942 | 0 / 0 |
| fight | 1920x1080 | 144 | 11.2 | n/a | 1396 | 1941 | 42 / 15 |
| fight | 2560x1440 | 30 | 11.2 | n/a | 1402 | 1941 | 22 / 6 |
| fight | 2560x1440 | 60 | 11.2 | n/a | 1396 | 1942 | 28 / 5 |
| fight | 2560x1440 | 120 | 11.2 | n/a | 1385 | 1954 | 6 / 3 |
| fight | 2560x1440 | 144 | 11.2 | n/a | 1392 | 1942 | 14 / 4 |
| fight | 3840x2160 | 30 | 11.3 | n/a | 1389 (1388–1390) | 3249 (3233–3265) | 3 (0–6) / 2 (0–3) |
| fight | 3840x2160 | 60 | 11.2 | n/a | 1387 (1384–1390) | 3249 (3234–3265) | 5 (0–10) / 2 (0–3) |
| fight | 3840x2160 | 120 | 11.3 | n/a | 1395 (1394–1396) | 3268 (3267–3268) | 15 (6–24) / 4 (2–7) |
| fight | 3840x2160 | 144 | 11.2 | n/a | 1389 (1385–1393) | 3300 (3299–3301) | 2 (0–4) / 0 (0–1) |
| menu | 3840x2160 | 30 | 10.6 | n/a | 1080 | 2769 | 0 / 0 |
| menu | 3840x2160 | 120 | 10.6 | n/a | 1080 | 2801 | 0 / 0 |


**Final (warm cache):**

| Workload | Output | Limit | n | avg fps | 1 % low | 0.1 % low | p99 ms | jitter ms | GPU ms/frame | CPU guest % | CPU GPU-cmd % |
|---|---|---|---|---|---|---|---|---|---|---|---|
| explore | 1920x1080 | 30 | 1 | 30.0 | 28.0 | 24.7 | 34.9 | 1.1 | 7.5 | 113 | 16 |
| explore | 1920x1080 | 60 | 1 | 59.8 | 42.0 | 22.9 | 20.3 | 2.7 | 7.2 | 62 | 49 |
| explore | 1920x1080 | 120 | 1 | 114.1 | 56.3 | 24.2 | 14.0 | 1.6 | 6.4 | 92 | 68 |
| explore | 1920x1080 | 144 | 1 | 100.0 | 47.3 | 37.9 | 19.8 | 1.8 | 6.7 | 105 | 77 |
| explore | 2560x1440 | 30 | 1 | 30.0 | 27.9 | 26.4 | 35.2 | 1.1 | 8.0 | 114 | 21 |
| explore | 2560x1440 | 60 | 1 | 59.9 | 41.9 | 22.5 | 20.3 | 2.9 | 7.9 | 57 | 46 |
| explore | 2560x1440 | 120 | 1 | 110.1 | 48.0 | 34.2 | 19.1 | 1.5 | 6.5 | 90 | 67 |
| explore | 2560x1440 | 144 | 1 | 118.5 | 48.5 | 35.8 | 19.0 | 1.6 | 6.1 | 101 | 77 |
| explore | 3840x2160 | 30 | 2 | 30.0 | 28.0 (27.9–28.1) | 26.5 (26.4–26.7) | 35.0 | 1.1 | 13.6 | 114 | 21 (20–23) |
| explore | 3840x2160 | 60 | 2 | 59.9 | 44.2 (43.3–45.1) | 29.3 (25.3–33.3) | 20.5 (20.3–20.6) | 3.2 (2.8–3.6) | 12.7 (12.5–12.8) | 64 (62–66) | 49 (48–51) |
| explore | 3840x2160 | 120 | 2 | 90.1 (87.8–92.4) | 43.0 (42.5–43.5) | 34.9 (32.4–37.3) | 22.3 | 5.4 (5.3–5.4) | 10.1 (9.9–10.3) | 104 (103–105) | 62 (62–63) |
| explore | 3840x2160 | 144 | 2 | 91.0 (88.4–93.5) | 42.3 (41.6–43.1) | 31.7 (29.5–33.8) | 22.2 (22.1–22.3) | 6.8 (6.5–7.1) | 10.2 (9.9–10.5) | 108 | 59 (58–60) |
| fight (fight) | 1920x1080 | 30 | 1 | 30.0 | 27.4 | 25.3 | 35.2 | 1.1 | 8.0 | 110 | 22 |
| fight (menus) | 1920x1080 | 30 | 1 | 30.0 | 28.3 | 27.4 | 34.9 | 1.0 | 7.9 | 108 | 19 |
| fight (fight) | 1920x1080 | 60 | 1 | 59.8 | 35.5 | 17.8 | 19.7 | 2.5 | 6.6 | 42 | 38 |
| fight (menus) | 1920x1080 | 60 | 1 | 59.9 | 39.9 | 21.7 | 19.6 | 2.2 | 6.1 | 42 | 39 |
| fight (fight) | 1920x1080 | 120 | 1 | 102.9 | 63.7 | 51.7 | 14.4 | 1.8 | 7.1 | 103 | 78 |
| fight (menus) | 1920x1080 | 120 | 1 | 119.8 | 85.3 | 72.2 | 10.5 | 1.7 | 6.3 | 99 | 79 |
| fight (fight) | 1920x1080 | 144 | 1 | 106.9 | 61.1 | 37.4 | 14.3 | 1.4 | 7.0 | 108 | 80 |
| fight (menus) | 1920x1080 | 144 | 1 | 117.1 | 77.1 | 75.1 | 12.6 | 1.0 | 6.5 | 111 | 88 |
| fight (fight) | 2560x1440 | 30 | 1 | 30.0 | 26.2 | 14.5 | 34.8 | 1.1 | 6.3 | 109 | 13 |
| fight (menus) | 2560x1440 | 30 | 1 | 30.0 | 28.7 | 28.5 | 34.7 | 0.9 | 5.8 | 107 | 12 |
| fight (fight) | 2560x1440 | 60 | 1 | 60.0 | 42.3 | 23.0 | 20.4 | 3.7 | 7.9 | 66 | 52 |
| fight (menus) | 2560x1440 | 60 | 1 | 60.0 | 49.3 | 46.1 | 19.2 | 2.5 | 8.3 | 62 | 53 |
| fight (fight) | 2560x1440 | 120 | 1 | 101.5 | 64.0 | 49.6 | 14.4 | 2.0 | 7.4 | 104 | 77 |
| fight (menus) | 2560x1440 | 120 | 1 | 115.4 | 70.3 | 51.4 | 12.9 | 1.1 | 6.5 | 110 | 87 |
| fight (fight) | 2560x1440 | 144 | 1 | 104.9 | 59.9 | 39.9 | 14.8 | 1.5 | 7.1 | 106 | 80 |
| fight (menus) | 2560x1440 | 144 | 1 | 122.7 | 76.1 | 63.0 | 11.6 | 1.2 | 6.2 | 102 | 83 |
| fight (fight) | 3840x2160 | 30 | 2 | 30.0 | 25.1 (24.7–25.5) | 14.0 (12.3–15.8) | 35.0 | 1.3 | 13.8 (13.8–13.9) | 110 | 20 (16–25) |
| fight (menus) | 3840x2160 | 30 | 2 | 30.0 | 28.7 (28.5–28.8) | 28.5 (28.4–28.7) | 34.7 (34.6–34.9) | 1.0 | 12.0 (11.3–12.8) | 108 | 13 |
| fight (fight) | 3840x2160 | 60 | 2 | 59.8 | 38.9 (36.6–41.1) | 17.5 (16.5–18.5) | 19.9 (19.9–20.0) | 2.8 | 12.2 (11.9–12.5) | 50 (49–51) | 42 (41–43) |
| fight (menus) | 3840x2160 | 60 | 2 | 60.0 | 49.1 (48.5–49.7) | 45.6 (44.2–47.0) | 19.4 (19.3–19.6) | 2.6 (1.9–3.2) | 12.2 (11.9–12.5) | 49 (41–58) | 42 (38–46) |
| fight (fight) | 3840x2160 | 120 | 2 | 86.2 (86.1–86.3) | 40.2 (39.3–41.1) | 29.9 (25.5–34.3) | 23.6 | 5.1 | 10.9 | 105 (105–106) | 65 |
| fight (menus) | 3840x2160 | 120 | 2 | 84.5 (82.7–86.2) | 42.6 (41.5–43.7) | 41.9 (41.1–42.7) | 23.3 (22.7–23.9) | 7.5 (6.9–8.1) | 11.0 (10.8–11.2) | 106 (105–107) | 60 (57–63) |
| fight (fight) | 3840x2160 | 144 | 2 | 86.0 (85.9–86.1) | 40.6 (39.2–42.0) | 32.8 (24.9–40.7) | 23.5 (23.4–23.6) | 5.6 (5.6–5.7) | 10.9 | 108 | 65 |
| fight (menus) | 3840x2160 | 144 | 2 | 84.2 (82.2–86.1) | 42.9 (41.8–44.1) | 42.4 (41.3–43.5) | 23.1 (22.5–23.7) | 7.9 (7.1–8.8) | 11.0 (10.8–11.2) | 107 (106–108) | 60 (57–63) |
| menu | 3840x2160 | 30 | 2 | 30.0 | 25.4 (25.3–25.4) | 24.6 | 38.8 (38.7–38.9) | 3.6 | 3.6 | 50 (49–51) | 12 |
| menu | 3840x2160 | 120 | 2 | 33.9 (33.5–34.4) | 26.5 (26.4–26.6) | 26.0 | 37.4 (37.2–37.5) | 10.2 (9.5–11.0) | 3.6 | 19 | 16 |

| Workload | Output | Limit | boot to title s | world load s | peak RSS MB | peak VRAM over idle MB | pipelines created / presentations skipped (measured window) |
|---|---|---|---|---|---|---|---|
| explore | 1920x1080 | 30 | 11.1 | 10.1 | 1415 | 1937 | 8 / 5 |
| explore | 1920x1080 | 60 | 11.1 | 10.2 | 1417 | 1939 | 16 / 9 |
| explore | 1920x1080 | 120 | 11.2 | 10.1 | 1408 | 1939 | 25 / 10 |
| explore | 1920x1080 | 144 | 11.1 | 10.1 | 1403 | 1938 | 6 / 4 |
| explore | 2560x1440 | 30 | 11.1 | 10.1 | 1408 | 1936 | 10 / 5 |
| explore | 2560x1440 | 60 | 11.1 | 10.1 | 1400 | 1936 | 2 / 2 |
| explore | 2560x1440 | 120 | 11.1 | 10.1 | 1408 | 2160 | 10 / 6 |
| explore | 2560x1440 | 144 | 11.1 | 10.1 | 1418 | 1924 | 10 / 6 |
| explore | 3840x2160 | 30 | 11.1 | 10.2 | 1408 (1405–1411) | 2985 (2968–3002) | 9 (0–18) / 2 (0–5) |
| explore | 3840x2160 | 60 | 11.2 | 10.2 | 1405 | 2999 | 6 (4–8) / 4 (3–5) |
| explore | 3840x2160 | 120 | 11.2 | 10.2 | 1404 (1402–1406) | 3002 (3000–3005) | 8 (4–12) / 4 (3–6) |
| explore | 3840x2160 | 144 | 11.2 | 10.2 | 1406 (1405–1406) | 3000 (2999–3002) | 9 (6–12) / 6 (4–7) |
| fight | 1920x1080 | 30 | 11.3 | n/a | 1384 | 2001 | 0 / 0 |
| fight | 1920x1080 | 60 | 11.3 | n/a | 1396 | 1975 | 20 / 6 |
| fight | 1920x1080 | 120 | 11.3 | n/a | 1386 | 1980 | 0 / 0 |
| fight | 1920x1080 | 144 | 11.3 | n/a | 1390 | 1980 | 8 / 3 |
| fight | 2560x1440 | 30 | 11.3 | n/a | 1394 | 1948 | 14 / 4 |
| fight | 2560x1440 | 60 | 11.3 | n/a | 1391 | 2976 | 10 / 3 |
| fight | 2560x1440 | 120 | 11.3 | n/a | 1385 | 1936 | 2 / 2 |
| fight | 2560x1440 | 144 | 11.3 | n/a | 1385 | 2669 | 4 / 2 |
| fight | 3840x2160 | 30 | 11.3 | n/a | 1397 | 3250 (3234–3266) | 28 (25–30) / 6 (5–8) |
| fight | 3840x2160 | 60 | 11.3 | n/a | 1398 | 3268 (3266–3271) | 22 (18–26) / 6 |
| fight | 3840x2160 | 120 | 11.3 | n/a | 1389 (1386–1392) | 3267 (3266–3268) | 13 (4–22) / 4 (2–7) |
| fight | 3840x2160 | 144 | 11.3 | n/a | 1390 (1387–1393) | 3251 (3235–3267) | 11 (0–22) / 4 (0–7) |
| menu | 3840x2160 | 30 | 10.6 | n/a | 1081 (1080–1082) | 2806 (2804–2809) | 0 / 0 |
| menu | 3840x2160 | 120 | 10.6 | n/a | 1079 (1078–1080) | 2795 (2788–2802) | 0 / 0 |


**Empty pipeline cache (first visit), baseline then final:**

| Workload | Output | Limit | n | avg fps | 1 % low | 0.1 % low | p99 ms | jitter ms | GPU ms/frame | CPU guest % | CPU GPU-cmd % |
|---|---|---|---|---|---|---|---|---|---|---|---|
| explore | 3840x2160 | 30 | 1 | 29.9 | 18.1 | 11.7 | 36.3 | 1.7 | 17.0 | 112 | 19 |
| explore | 3840x2160 | 120 | 1 | 73.9 | 28.8 | 14.2 | 26.7 | 13.4 | 12.6 | 109 | 43 |
| fight (fight) | 3840x2160 | 30 | 1 | 30.0 | 23.6 | 13.4 | 35.6 | 1.4 | 16.3 | 109 | 15 |
| fight (menus) | 3840x2160 | 30 | 1 | 30.0 | 28.2 | 28.0 | 35.3 | 1.2 | 14.9 | 107 | 14 |
| fight (fight) | 3840x2160 | 120 | 1 | 71.4 | 32.4 | 19.0 | 28.1 | 10.0 | 13.1 | 110 | 56 |
| fight (menus) | 3840x2160 | 120 | 1 | 71.6 | 36.0 | 35.5 | 27.4 | 13.4 | 13.1 | 109 | 48 |
| menu | 3840x2160 | 30 | 1 | 30.0 | 25.3 | 24.4 | 39.1 | 3.5 | 3.9 | 52 | 11 |

| Workload | Output | Limit | boot to title s | world load s | peak RSS MB | peak VRAM over idle MB | pipelines created / presentations skipped (measured window) |
|---|---|---|---|---|---|---|---|
| explore | 3840x2160 | 30 | 10.6 | 10.2 | 1394 | 3033 | 107 / 27 |
| explore | 3840x2160 | 120 | 10.6 | 10.1 | 1398 | 3066 | 107 / 27 |
| fight | 3840x2160 | 30 | 10.6 | n/a | 1376 | 3043 | 40 / 13 |
| fight | 3840x2160 | 120 | 10.6 | n/a | 1376 | 3096 | 28 / 9 |
| menu | 3840x2160 | 30 | 10.6 | n/a | 1076 | 2801 | 0 / 0 |


| Workload | Output | Limit | n | avg fps | 1 % low | 0.1 % low | p99 ms | jitter ms | GPU ms/frame | CPU guest % | CPU GPU-cmd % |
|---|---|---|---|---|---|---|---|---|---|---|---|
| explore | 3840x2160 | 30 | 1 | 30.0 | 18.4 | 12.4 | 37.2 | 1.7 | 13.6 | 113 | 22 |
| explore | 3840x2160 | 120 | 1 | 83.9 | 32.6 | 14.2 | 22.5 | 5.3 | 10.6 | 105 | 65 |
| fight (fight) | 3840x2160 | 30 | 1 | 30.0 | 22.2 | 12.8 | 35.4 | 1.5 | 14.4 | 106 | 21 |
| fight (menus) | 3840x2160 | 30 | 1 | 30.0 | 28.1 | 27.4 | 35.0 | 1.1 | 15.2 | 108 | 21 |
| fight (fight) | 3840x2160 | 120 | 1 | 85.9 | 37.9 | 19.9 | 23.5 | 4.8 | 10.9 | 105 | 67 |
| fight (menus) | 3840x2160 | 120 | 1 | 86.3 | 43.8 | 43.2 | 22.6 | 8.1 | 10.8 | 106 | 57 |
| menu | 3840x2160 | 30 | 1 | 30.0 | 25.4 | 24.8 | 39.0 | 3.6 | 3.8 | 49 | 12 |

| Workload | Output | Limit | boot to title s | world load s | peak RSS MB | peak VRAM over idle MB | pipelines created / presentations skipped (measured window) |
|---|---|---|---|---|---|---|---|
| explore | 3840x2160 | 30 | 10.6 | 10.3 | 1395 | 3031 | 115 / 30 |
| explore | 3840x2160 | 120 | 10.6 | 10.2 | 1396 | 3032 | 125 / 30 |
| fight | 3840x2160 | 30 | 10.6 | n/a | 1367 | 3041 | 54 / 14 |
| fight | 3840x2160 | 120 | 10.6 | n/a | 1368 | 3044 | 28 / 9 |
| menu | 3840x2160 | 30 | 10.6 | n/a | 1078 | 2801 | 0 / 0 |


Summary of the matrix:
- 3840x2160 at 120 / 144: 77.5-77.6 -> 90.1-91.0 fps in exploration, 78.5-79.9 -> 86.0-86.2 in the fight; jitter
  about halved (10.3-13.5 -> 5.1-7.9 ms); 1 % lows +4 to +29 %; 0.1 % lows up to doubled.
- 60 fps: held at 60.0 everywhere (as before); jitter 3.7-6.7 -> 2.2-3.7 ms, p99 20.1-22.7 -> 19.4-20.5 ms.
- 30 fps: unchanged frame times (the cap); the GPU works less per frame (4K: 16.2 -> 13.6 ms); 0.1 % lows at 4K
  20.6 -> 26.5 (exploration).
- 2560x1440 / 1920x1080 at 120 / 144: these configurations are limited by the CPU side (7.6) and their single runs vary
  more than the changes: see 7.5.1.

#### 7.5.1 The CPU-bound outputs: the transfer change's cost

At 2560x1440 and 1920x1080 above 120 fps the GPU is not the limit (6-7 ms per frame); the single matrix runs there
moved by more than the changes (for instance exploration at 1920x1080 / 144: 114.7 -> 100.0 fps; the same build and
configuration gave 117.5 and 108.1 in two runs). To isolate 7.4.1's CPU cost, the final build ran 2560x1440 at 144
with `--execute_unclipped_draw_vs_on_cpu` off and on (n = 2 each):

| Window | avg fps off -> on | 1 % low | 0.1 % low | p99 | GPU ms per frame |
|---|---|---|---|---|---|
| exploration | 105.3 -> 112.8 (on: 108.1 and 117.5) | 45.1 -> 44.4 | 29.6 -> 23.2 | 20.0 -> 19.3 ms | 7.1 -> 6.3 |
| fight | 105.4 -> 104.2 | 58.9 -> 57.4 | 34.7 -> 30.6 | 14.6 -> 14.7 ms | 7.5 -> 7.3 |
| status / inventory screens (10 s) | 114.2 -> 109.6 | 74.0 -> 66.4 | 66.1 -> 54.4 | 12.8 -> 13.8 ms | 6.8 -> 6.9 |

**Corrected in M9 (section 8.4):** the interpretation above did not hold. The CPU vertex shader estimates cost
0.008 ms per frame (10 draws, 30 indices: measured with patch 0012's counters), and with 5 interleaved runs each the
option is equal or better at 2560x1440 / 144: the status and inventory screens' 1 % low 82.8 (off) -> 85.0 (on),
0.1 % low 59.7 -> 69.0; the fight's 1 % low 63.6 -> 66.2. The differences in the table above (n = 2) were run-to-run
variation. The option stays on at every output size.
- Memory, VRAM, load and boot times: unchanged within a few MB and 0.1 s. No run crashed; no run was invalid (78 matrix
  runs, plus the A/B runs).


### 7.6 Frame pacing and high frame rates

How the port separates simulation and rendering is unchanged from M6D (docs/HIGH_FPS.md): the game runs exactly 30
ticks per second on a host clock (simulation, animation, physics, AI, scripts, audio, the camera's simulation and
input are all per tick); frames between ticks are render-only builds from the tick's retained data with the camera,
the player, world objects, skeletal poses, physics-driven parts and effect shaders interpolated (K39); network timing
is not involved (co-op is a developer option, K56). M8 changed only when the frames between ticks are drawn
(7.4.2) and added an opt-in adaptive rate (7.4.3); the simulation rate is never raised.

Checked with the existing tests on the final build (build/logs/m8-regression-1.log): the timing tests (30 ticks per
second at 60/120/240/unlimited and with VSync off, the K44 recovery at 60 and 120 and their fault controls, six
minutes at 60), the presentation tests (interpolation on the path between ticks, isolation of the game state for 600
ticks, state equal to the 30 fps run, pose and physics-part checks, HUD and effect clocks, discontinuities), the
render tests (frames complete at 1x, 4K and 32:9, K34, K46) and the camera tests (mouse, stick, orbit, distance,
field of view, recentring, also at 120): all pass. The gameplay tests' fight and loot at 30, 120 and Render Native
pass. Failures: K74 (pre-existing, the same with the baseline build) and one intermittent route-timing failure.

Target 120 fps at 3840x2160: **not reached** on the RX 9070 XT. The final build delivers about 90 fps in Shadow
Depths and about 86 in the fight (GPU-bound: 10 ms of GPU time per frame; the draws 4.5-5.4 ms, texture loads from
resolves 2.4-3.1 ms, resolves 1.3-1.9 ms, render target transfers 1.3-1.4 ms). At 2560x1440 (and 1920x1080, the same
frame) a 120 limit gives 101-110 fps: there the GPU command processor thread and the game's render thread are the
limit (the render thread waits for ring space about 30 % of its time), not the GPU. A 60 limit holds 60.0 at every
output size with the jitter halved by 7.4.2.


### 7.7 Stability

`tools/stability.py --tag final --preset m8-frozen-final` (the final build; summary in build/stability/final/):
**21 of 21 runs passed**, and the repeated pressure run (22 of 22) (exit 0 by the timed exit, no crash report, the harness's run, script and log checks, no
unexpected `[error]` line; the warnings are the sandbox's: no pointer lock, no controller database, the first-sight
pipeline skips).

| Stage | Runs | What it did | Result |
|---|---|---|---|
| boot | 5 | cold start, renderer initialisation, title, main menu, timed shutdown | pass; RSS 1,091-1,093 MB at the end of each |
| boot120 | 2 | the same with a 120 limit | pass; 1,089-1,091 MB |
| world | 3 | CONTINUE (world load, 10.1 s) and 140 s of play at 120 | pass; RSS 1,404-1,419 MB, +16-27 MB after the first quarter (the area's later loads) |
| save_load | 2 x 2 | load the fixture, status / equipment / inventory / quest screens, save; a new process loads that save and checks again | pass (process health; disposable copies of the fixture) |
| unload_reload | 1 | play at 120, the Quit save, the title, a new game and its autosaves (with K46's fault injection) | pass; RSS 1,413 MB (+138 MB: the second area loaded after the title) |
| framerate | 2 | the frame-rate limit changed 36 times in 200 s of play (30, 60, 90, 120, 144, 165, 240, unlimited): every change applied (`[kuf-pacing] ... frame rate x -> y`, 36 of 36) | pass |
| resize | 2 | 24 window resizes in play at 120 (16:9, 21:9, 4:3, 5:4, 32:9): every resize handled (`window resized`, 24 of 24). Offscreen the simulated output size stays, so the drawn aspect stays 16:9: this exercises the resize path, not the widescreen change | pass |
| pressure | 1 + 1 | world load and play at 120 under the scope's `memory.high`. The first run used 1,300 MB, above what the game's cgroup is charged (about 1.1 GB: anonymous 360 MB and guest shared memory 726 MB; its RSS reads 1.4 GB, part of it charged elsewhere), so it applied no pressure (same fps as without). The second (`--memory-high 800M`) did: the kernel held the game's RSS at 1,087 MB (about 320 MB reclaimed or swapped out) and the run played as without: 105.9 fps, p99 18.6 ms, 1 % low 37.9, world load 10.3 s (10.1 without) | pass (both); the 800 MB run under real reclaim |
| long | 1 | 30 minutes of play at 120 fps in Shadow Depths (walks and turns every 30 s, enemies as they come) | pass; RSS 1,396 MB at the end, **+4 MB after the first quarter (0.17 MB per minute)**: no leak in play |

Not covered (and why): repeated renderer initialisation within one process (the SDK creates the renderer once per
process; each boot run re-initialises it); real display-mode switches (offscreen runs have no display; the display
tests cover the setting's logic); multiplayer (not allowed in M8); memory exhaustion (deliberately not attempted).

### 7.8 Remaining limitations and next steps

| Item | Evidence | Next |
|---|---|---|
| 4K at 120 is GPU-bound at about 90 fps | 10 ms GPU per frame; draws 4.5-5.4 ms | GPU work per frame: texture loads from resolves (2.4-3.1 ms for 11 per frame) and resolves (1.3-1.9 ms) are the largest parts besides the draws; both are SDK paths (scaled resolve buffer, load shaders) |
| 1440p at 120 limited by the command processor and render thread | 1,000-1,300 steps dropped as render busy per 140 s; the render thread waits for ring space | CPU work per frame in the GPU command processor (register writes are a third of it; a cheaper unknown-register check gave no measurable gain, 7.4.4), translation and submission on separate threads |
| Loading is paced by the game tick | world load 10.1 s, 3.1 s with ticks uncapped | a fast-loading mode: identify the game's loading state; uncapped ticks only then; check scripts, intro, music, logos |
| Frames at uneven intervals when the GPU cannot keep up | 4K at 120: jitter 5-6 ms | `--kuf_frame_rate_adaptive` (opt-in; a menu row needs the player's decision) |
| First-time pipeline hitches (K26) | cold cache: 27 skipped presentations in 140 s | strategies in section 5 |
| Title and menus at 33 fps above a 30 limit (K47) | menu workload 33 fps at 120 | the movie player's wait (reverse engineering) |
| Co-op host crash on a guest leaving (K72) | static analysis | the texture slot release; tests in isolation; a two-instance test at 30 fps (the player's permission) |
| Pre-existing test route failures (K74) | the same with the baseline build | rewrite the routes with K66 |

### 7.9 Reproducing

```sh
ninja -C build/linux-amd64-relwithdebinfo -j8
# freeze a build (optional, lets the tree be rebuilt while the matrix runs)
mkdir -p build/m8-frozen-x/bin && cp -a build/linux-amd64-relwithdebinfo/bin/{kuf_cod,librexgpu-xenosrd.so,librexruntimerd.so,libTracyClientrd.so} build/m8-frozen-x/bin/
tools/bench_m8_matrix.sh <tag> all m8-frozen-x                          # 39 runs, about 2.5 h
python3 -I tools/bench.py report --tag <tag> --compare <other tag> --markdown out.md
python3 -I tools/bench.py run --tag x --scenario tests/perf/bench_explore.toml --res 3840x2160 --fps 120 \
    --game-arg=--gpu_timestamp_sections=true                            # one run with the GPU sections
python3 -I tools/stability.py --tag <tag> --preset m8-frozen-x          # about 2 h
python3 -I tools/menu_test.py --run-prefix eqA --game-arg=--kuf_display_size=3840x2160 \
    --game-arg=--kuf_pc_settings=display.render_scale=0 tests/perf/render_equivalence.toml   # render equivalence
```

## 8. M9: deep GPU, Vulkan and ReXGlue optimisation (2026-11-01)

Branch `m9/deep-optimization` (from `m8/performance` 61a061f). ReXGlue SDK v0.10.0 (c94f5eb) with the port's patches
0001-0013. Renderer: the Vulkan backend with host render targets (the FBO path), dynamic rendering, asynchronous
pipeline creation with the persistent pipeline cache, resolution scaling by whole multiples (3840x2160 = 3x3 of the
game's 1280x720; 1920x1080 and 2560x1440 both 2x2). Same machine and sandbox as section 7.

### 8.1 Method

- **Interleaved runs.** Every comparison alternates the two variants run by run (A, B, A, B, ...), 5 runs each,
  from frozen copies of the builds (`build/m8-frozen-final`, `build/m9-frozen-*`), with the warm pipeline cache of a
  cold run (section 7.3). Results are medians with the range [min-max]. A difference is claimed only where the
  ranges separate or the medians move by clearly more than the spread.
- **GPU state** is sampled with every run (`tools/bench.py`: shader clock, junction temperature, power under load):
  3,288-3,291 MHz, 76-77 C, 249-256 W in every 4K run, so clocks and temperature did not vary between runs. The
  day-to-day level did: the same M8 build measured 90.1 fps (section 7, n = 2) and 83.7-92.3 (n = 5, today, in three
  series), which is why only interleaved series are compared.
- **New instruments** (SDK patch 0012, off unless asked): per-frame Vulkan command counts (from the deferred command
  buffer: draws, render passes, pipeline and descriptor binds, barriers by kind, copies, dispatches); a per-pass GPU
  timeline of GPU-traced frames (`--gpu_trace_pass_timing`: a timestamp at every render pass begin with the pass's
  render targets and draw count); GPU idle time between submissions; the texture loads' copies into images as their
  own GPU section; the CPU time of the vertex shader extent estimates.

### 8.2 Where a 4K frame's GPU time went (the M8 build)

3840x2160, 120 limit, Shadow Depths; four consecutive traced frames (a tick frame and its render-only frames):

| Per frame | |
|---|---|
| GPU time | 10.9-11.3 ms in the traced frames (10.4-10.5 ms median over 140 s) |
| draws | 807 (139 non-indexed, 565-605 indexed), in 203-250 render passes |
| pipeline binds / descriptor set binds / push constants | 250-260 / 1,040-1,140 / 71-73 |
| pipeline barriers | 660-840 (buffer barriers 665-840, image barriers 75-77) |
| buffer copies / buffer-to-image copies / dispatches | 420-550 / 11 / 38-39 |
| GPU idle between submissions | 0.9-2.0 ms (7-16 % of the time), about 6.5 submissions |

| GPU section | ms per frame | What |
|---|---|---|
| draws | 5.3 | the game's passes at 9x the pixels: the shadow / depth pass (d0:006038E0) 1.84 ms, 180 draws in 137 render passes; the scene (d0:006082D0 c0:00C08000) 1.47 ms, 215 draws; the depth pass 0.88 ms, 177 draws |
| render target transfers | 1.4 | 16 per frame: the game reinterpreting the same EDRAM depth range as 4x MSAA and 1x (007082D0 <-> 006082D0) and colour pairs; genuine Xbox 360 aliasing after K73 |
| resolves | 1.3 | 11-12 per frame into the resolution-scaled resolve buffer |
| texture loads | 2.9 | the 11 resolve results sampled as textures, every frame: the HDR scene 3840x2160 (16-bit float RGBA, 66 MB) twice, depth read as a float texture 3840x2160 twice, an RGBA8 copy, the shadow maps' float copies (1536^2, 3072^2), bloom targets; each untiled into a staging buffer (compute) and copied into the image (11 buffer-to-image copies); 0.8 ms for each HDR load |

### 8.3 Optimisations tried

| Change | Evidence | Result |
|---|---|---|
| **Render-to-texture results loaded straight into their images** (SDK patch 0013, `--vulkan_texture_load_direct_to_image`, on by default) | 5+5 interleaved on one build: 87.3 -> 101.5 fps, GPU 10.58 -> 8.58 ms, p99 22.3 -> 19.6 ms, jitter 6.3 -> 3.2 ms, 1 % low 43.1 -> 45.2; buffer-to-image copies 11 -> 6.2 per frame (8.6 for M8 vs M9) | **kept** |
| Upload look-ahead (upload the next rewritten pages with a draw's request, 256 KB) | copies 421 -> 302, barriers 664 -> 516, render passes 204 -> 184 per frame, but GPU 10.35 -> 10.62 ms and 86.6 -> 82.7 fps (n = 1 each): the extra data costs more than the barriers | reverted |
| One submission per frame (`vulkan_submit_on_primary_buffer_end=false`) | 5+5: 87.9 -> 74.7 fps, p99 23.5 -> 26.3 ms, GPU 10.27 -> 10.66 ms: the GPU waits for the whole frame | rejected (setting unchanged) |
| A submission every 128 draws (experiment) | 5+5: 85.5 -> 84.0 fps, 1 % low 40.0 -> 36.1, GPU 10.50 -> 10.40 ms; idle unchanged | reverted |
| `execute_unclipped_draw_vs_on_cpu` per resolution (M8's 1440p trade-off) | CPU cost of the estimates 0.008 ms per frame; 5+5 at 1440p/144: equal or better with it on (8.4) | no change needed; M8's 7.5.1 corrected |

Barriers and render pass breaks are not a significant cost on this GPU (the look-ahead removed 150 barriers per frame
without a gain). Idle between submissions stays at about 0.9 ms per frame whatever the frame pacing (also with an
unlimited frame rate) and the submission size: it is the command processor's translation feeding the GPU, not the
pacing.

**8.3.1 The direct texture loads.** The texture cache loads a texture made from a resolution-scaled resolve every
time the game renders it again. Before: a compute shader read the scaled resolve data (Xenos tiling, endian swaps),
wrote it linearly into a staging buffer, and `vkCmdCopyBufferToImage` copied that into the image, four passes over up
to 66 MB per texture at 4K. Now, for resolution-scaled textures of one 2D slice and one mip level whose host format the
loader writes bit for bit (RGBA16 formats; RGBA8, RGB10A2, RG16, R32 formats), the image is created with storage usage
and an integer view (`R16G16B16A16_UINT` or `R32_UINT`, listed with `VkImageFormatListCreateInfo`), and variants of the
64 bpb, 32 bpb and depth float load shaders write the same bits into the image. The variants are derived from the
SDK's shipped SPIR-V by `tools/spirv_load_to_image.py`: every instruction that reads and converts the data is kept;
the two stores into the staging buffer become guarded image writes (`spirv-val` clean, core storage formats only, no
new device features). Textures take a new usage (`kComputeWrite`, GENERAL layout) and are transitioned from it like any
other usage. Other textures, and any texture without a free descriptor (a pool of 1,024), take the staging path.
SDK functions changed: `VulkanTextureCache::CreateTexture`, `LoadTextureDataFromResidentMemoryImpl`, `Initialize`,
`Shutdown`, `GetTextureUsageMasks`, `VulkanTexture::~VulkanTexture`; added `GetDirectLoadViewFormat`,
`PrepareDirectLoad`.

### 8.4 1440p: the extent estimate is not a trade-off

2560x1440, 144 limit, the fight workload, 5+5 interleaved, `execute_unclipped_draw_vs_on_cpu` on / off:

| Window | avg fps on / off | 1 % low | 0.1 % low | GPU ms |
|---|---|---|---|---|
| status / inventory screens | 127.9 / 125.5 | 85.0 / 82.8 | 69.0 / 59.7 | 6.0 / 6.4 |
| fight | 109.3 / 108.6 | 66.2 / 63.6 | 36.8 / 41.1 | 7.1 / 7.2 |

The estimates themselves cost 0.008 ms of CPU time per frame (10 draws, 30 indices). No resolution-based switching is
needed.

### 8.5 Visual validation

- `tests/perf/render_equivalence.toml` (11 captures at fixed ticks: title, save list, play, walk, turn, status,
  inventory, back in play, pause, resumed) at native 4K, direct loads off and on: the 8 gameplay captures
  **bit-identical** (largest difference 0) when both runs entered gameplay at the same tick. One run entered at tick
  1386 instead of 1388 and differed (up to 94 levels): the route's intro skip, not the rendering, which is why
  `render.direct_load_check` checks the anchor first. The menu captures over the title movie differ by up to 10
  levels between any two runs (the movie's timing), with or without the change.
- Even with the same anchor, two runs of the **same** build and setting are not always identical: the M8 build's
  staging-only runs m8-eq4k-A and -B differ in play_turn, back_in_play and resumed by at most 2 levels (mean 0.0001),
  while -A and -C are identical; the M9 build's staging runs show the same two end states. The first off / on pair of
  the ctest showed exactly this pattern (2 levels in those three captures). So the test requires the direct run to be
  bit-identical to a staging run with the same anchor, not to the first one.
- Each shader variant alone: 64 bpb and depth float bit-identical; 32 bpb within the title-movie noise.
- New ctest `render.direct_load` (`tests/render/direct_load_pair.py`): staging and direct runs alternate (up to 3
  each); every pair with the same anchor is checked by `direct_load_check.py` (direct loads used in the one run and
  not the other, gameplay captures identical, difference 0) and the test passes on the first identical pair.
- Vulkan validation (`--vulkan_validation_enabled=true`, the Khronos layer): the same four messages with direct loads
  off and on, pre-existing in the SDK's draw path (K75); none from the new path.

### 8.6 Results: M8 and M9

The final builds, frozen: M8 = build/m8-frozen-final (99fdccb), M9 = build/m9-frozen-final (ad2609b; later M9
commits change only tests and docs). Each build ran its own cold run first (tags `m9f-cold8`, `m9f-cold9`) and every
measured run started from a copy of that build's cache (a cache seeded by another build's cold run left the M9
build 18 graphics pipelines short, each a 40-105 ms first-use hitch that set the 0.1 % low in the A/B series).
Workload `bench_explore` (Shadow Depths, 140 s), runs interleaved M8, M9, M8, M9, ... (tags `m9f-m8`, `m9f-m9`), 5
each per size. GPU under load at 4K: 3,270-3,286 MHz, junction 77-78 C, 256-268 W in all 10 runs; at 1440p and
1080p the GPU is not fully loaded and its clock follows the load (1,540-3,299 MHz, 57-73 C), the same for both builds.

**The milestone table** (3840x2160, 120 limit; the M8 column as reported in section 7, n = 2 on 2026-10-31; the M9
column the median of 5 runs, range in brackets):

| Metric | M8 | M9 |
|---|---|---|
| Avg FPS | 90.1 | 95.5 [92.3-102.4] |
| 1% low | 43.0 | 46.2 [41.2-47.2] |
| 0.1% low | 34.9 | 29.1 [21.1-33.5] |
| GPU frame time | 10.12 ms | 8.83 ms [8.6-9.5] |
| Jitter | 5.4 ms | 3.2 ms [3.1-4.0] |
| Peak memory | 1,404 MB | 1,408 MB [1,402-1,418] |

The M8 column was measured on another day with another cache; the comparison that counts is the same day's
interleaved one, both builds from their own caches:

| 3840x2160 / 120, n = 5 + 5 | M8 build | M9 build | Change |
|---|---|---|---|
| Avg FPS | 91.2 [87.5-93.3] | 95.5 [92.3-102.4] | +4.7 % |
| 1 % low | 41.5 [39.0-43.5] | 46.2 [41.2-47.2] | +11 % |
| 0.1 % low | 26.1 [21.5-36.0] | 29.1 [21.1-33.5] | no measurable change (ranges overlap) |
| p99 frame time | 22.2 ms [22.1-22.3] | 19.8 ms [19.7-20.2] | -11 % (ranges separate) |
| Jitter | 5.6 ms [5.2-5.7] | 3.2 ms [3.1-4.0] | -42 % (ranges separate) |
| GPU time per frame | 10.07 ms [9.9-10.4] | 8.83 ms [8.6-9.5] | -12 % (ranges separate) |
| Peak RSS | 1,408 MB | 1,408 MB | none |

The earlier direct-load A/B on one build (section 8.3, the same cache for both) measured 87.3 -> 101.5 fps and
10.58 -> 8.58 ms; the day-to-day level of this workload moves by up to 8 % (section 8.1), the GPU time per frame and
p99 much less. The 0.1 % lows are set by about 13 frames in 140 s and vary 21-36 within either
build; neither build is better there.

At the other sizes the GPU is not the limit (the game's command processor thread is: about 77 % of a core at 1440p):

| n = 5 + 5 (1440p: two series, 10 + 10) | M8 avg / 1 % / 0.1 % low | M9 avg / 1 % / 0.1 % low | GPU ms M8 / M9 |
|---|---|---|---|
| 2560x1440, 144 limit | 117.4 [108-127] / 48.2 / 34.0 | 114.4 [94-127] / 48.9 / 37.6 | 6.28 / 6.05 |
| 1920x1080, 120 limit | 104.4 [102-106] / 47.9 / 36.6 | 104.0 [102-109] / 47.7 / 36.0 | 6.75 / 6.40 |

No measurable difference at 1440p or 1080p apart from less GPU time. At 1440p the first series showed M9 4.6 % slower
(118.5 / 113.0) and a second, three-way series (M8 / M9 with direct loads off / M9) 4.3 % faster (115.4 / 115.9 /
120.4), so the CPU-bound workload's run-to-run spread (about 5 %) covers both; the pooled medians are in the table.
Where the 1.2 ms went (one run each, M9 build, 4K / 120, `--gpu_timestamp_sections`, 10 s intervals after the
first): texture loads 1.75-1.78 ms plus 1.05-1.09 ms of copies into images (11 per frame) with the staging buffer;
1.47-1.53 ms and no copies into images with direct loads. Draws, transfers (1.3-1.4 ms, 16 per frame) and resolves
(1.25-1.35 ms, 11 per frame) unchanged. Runs `m8-m9sec-{off,on}-3840x2160-120-r1-bench_explore`.

**120 fps at 4K is not reached** (95-101 fps, GPU-bound at 8.8 ms per frame plus 0.9 ms of idle between
submissions).

### 8.7 Stability and regression

**Stability** (`tools/stability.py --tag m9 --preset m9-frozen-final`, build/stability/m9/): **21 of 21 runs
passed**, the same stages as section 7.7: boot 5 + 2 (RSS 1,091-1,093 MB), world loads and play 3 (1,406-1,422 MB),
save and load 2 x 2, unload and reload (1,412 MB, +135 MB: the second area; M8 +138), frame-rate switching 2,
window resizing 2, play under `memory.high` 800 MB (held at 1,087-1,097 MB), and 30 minutes of play at 120 fps:
1,396 MB at the end, +7 MB after the first quarter (0.29 MB per minute; M8 +4 MB, 0.17): no leak.

**Regression** (the final candidate, ctest labels timing, presentation, render, camera, gameplay and the
`display.render_*` comparisons; build/logs/m9-regression-{1,2}.log): 115 of 116 passed at once. The one failure was
the new direct-load check's route anchor (gameplay entered at 1387 and 1388), which led to the pair driver
(section 8.5); `render.direct_load` then passed (build/logs/m9-direct-load-pair-2.log). **The four K74 tests repaired**
(`gameplay.new_game`, `new_game_120`, `controller_gameplay`, `camera.camera_cutscene`, and with them
`presentation.discontinuities_play`): stale routes since M7, not the code (KNOWN_ISSUES K74, resolved): the spare
confirmation press for the storage question M7 now answers, and Escape (not Space) to leave the tutorial hint, a
menu context since a359b0e. Assertions unchanged; all five pass.

**Vulkan validation** (section 8.5): no message from the new path; four pre-existing SDK messages (K75).

**Files and saves:** `tools/verify_game_dir.sh`: the game folder's 138 files unchanged; none of the 53 fixture,
manual and backup save files in build/ changed since M9 began; no user save folder exists outside the sandbox;
`tools/check_tracked.py` clean.

### 8.8 Remaining limitations

| Item | Evidence | Next |
|---|---|---|
| 4K at 120 not reached | GPU-bound: 8.8 ms per frame after M9 (10.1 in M8) | the draws (5.3 ms at 9x the pixels) and resolves (1.3 ms) |
| GPU idle between submissions | 0.9 ms per frame at 4K | the command processor's translation (one thread) feeding the GPU: translate ahead / split translation and submission |
| Remaining transfers | 1.4 ms per frame, the game's 4x / 1x reinterpretation of one EDRAM range | only removable where a transfer's data is never read (needs a per-frame read / write analysis) |
| The shadow pass's many render passes | 137 passes for 180 draws (uploads between draws) | no measurable gain from fewer barriers on this GPU |
| First-use pipeline hitches | with the cache of an older configuration, direct loads needed 18 more pipeline variants | the persistent cache holds them after the first encounter |
| Pre-existing validation messages | K75 | SDK draw path: dynamic stencil state, unused dynamic rendering attachments |

### 8.9 Reproducing

```sh
python3 -I tools/bench.py run --tag <cold> --preset <frozen build> --scenario tests/perf/bench_explore.toml \
    --res 3840x2160 --fps 120                                     # each build's own cold run first
python3 -I tools/bench.py run --tag <tag> --preset <frozen build> --warm <cold> --first-rep <n> \
    --scenario tests/perf/bench_explore.toml --res 3840x2160 --fps=120   # alternate the builds, n = 1..5
    --game-arg=--vulkan_texture_load_direct_to_image=false        # the staging path, for an A/B on one build
    --game-arg=--gpu_timestamp_sections=true                      # the GPU sections
    --game-arg=--gpu_trace_pass_timing=true                       # with a traced frame: the per-pass timeline
ctest --test-dir build/linux-amd64-relwithdebinfo -R '^render\.direct_load$'   # direct loads draw the same frames
python3 -I tools/spirv_load_to_image.py <texture_load_*_cs.h> <out.spvasm> --bits 64|32   # the shader variants
```
