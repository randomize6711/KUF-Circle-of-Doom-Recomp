# Performance (M6A)

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
