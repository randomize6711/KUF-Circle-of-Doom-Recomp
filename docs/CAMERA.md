# Camera

How the original third-person camera works, where its restrictions come from (M6A investigation, M6B root
cause), and the modern camera of M6B (`src/runtime/modern_camera.cpp`, `src/runtime/camera_math.cpp`). Sources:
the game's camera data (read from the archives into `build/analysis/v1/camera/`, never committed), the
disassembly, the debug camera editor's field names in the executable, and sandboxed runs with diagnostics
(`--kuf_trace_camera`, `--kuf_camera_poke`, `--kuf_camera_watch`, `--kuf_trace_modern_camera`, gdb watchpoints
through `tools/menu_test.py --gdb`). No leaked source.

Legend as in MENU_ARCHITECTURE.md: **[V]** verified, **[S]** static analysis only, **[H]** hypothesis.

## 1. Camera definitions are data

- Every camera situation is a numbered camera in data files `d:\data\camera\*.cam` ("GenericCamVer3.0"): one
  file per player character (`Cellin.cam` for Celine, `Kendal.cam`, ...), `CommonEvent.cam` (events), `ani.cam`.
  The numbers are named in `CODCamDef.h` (shipped with the data): 0 action default ("player-dependent
  camera"), 1 air attack, 2 back, 3 left/right, 4-17 per transformation (default / air), 18-19 charge, 30
  battle, 50 dream; 200+ common cameras (player died, revive, boss intros, conversations). **[V]**
- Loaded by `sub_8252D0C8(set, first, count, path)`, parser `sub_8252C740` (3.0). Per camera `{key count,
  keys}` at `set+84 + 8 * index`; per key 348 bytes, read field by field: 21 words, 4 bytes (+84 .. +87), a
  word (+88), and two 128-byte blocks at +92 and +220. **[V]**
- Key fields used here (labels from the debug editor, offsets verified by tracing): +12 smoothing frames, +85
  FreeCam Enable, +87 Collision, **+92 horizontal free-camera mode, +96 vertical mode**, +100 horizontal limit
  on, +220/+224 horizontal limits, **+228/+232 vertical limits**, +236/+240 rate factors. **[V]**
- Every character's gameplay cameras use horizontal mode 1 (rate) and vertical mode 3 (absolute) with
  vertical limits -0.05..0.18 rad (some -0.2..0.2); air attack cameras use rate modes for both; the
  transformation cameras horizontal mode 2 or 3 with limits. **[V]** (all eight files parsed)

## 2. The running camera

The gameplay camera object (vtable `0x82020FB0`; in the test fixture at `0x44BF2ED0`) is updated once per frame
by `sub_82524B60` (the camera manager, at camera + 672):

| Step | Function | What it does |
|---|---|---|
| input | `sub_82524A38` | both sticks through `sub_82523D78` (radial dead zone 12,800, scaled to 0..1; an axis below 0.1 becomes 0 for x but **0.1** for y, a game bug); right stick to +2068/+2072, y negated when the game's Camera option is Inverted (+2892) |
| free camera | `sub_825232B8` | integrates +2068/+2072 into the yaw/pitch offsets +2076/+2080 by the key's modes (copied to +180/+184) and limits (+2088..+2100) |
| camera | `sub_8252C040` | key evaluation; smoothing (`sub_8252B460`, 1/(frames+1) per frame towards the key's eye and target offsets); `sub_8252B8A0`: the virtual geometry step `sub_825239F8(camera, eye out, target out, eye in, target in)`, then the **position smoothing**: the eye and target kept at +496/+508 move 1/n of the way to the new ones per frame (n = +548 / +552, 4 for Celine's camera; off when +520 is set; a change within the constant at `0x8200099C` is taken whole); then the virtual **collision step** `sub_82521710` when the key's Collision flag is set (+586): a ray down from the eye keeps it game parameter 248 (150 units) above the ground, a ray up for ceilings, and the eye's height is smoothed through a global (`0x82A805CC`: 30 % of the way per frame for changes up to 1000); then the camera rows (+16 right, +32 up, +48 forward, +64 position) |
| copy | `sub_82524B60` | the rows to the manager (+176), which the game reads: the renderer's view (`sub_825246D0` -> `sub_821A2CA0`), camera selection (`sub_825274E0`), the player character's code (`sub_82474D80`), saves (`sub_826EB598`) and more |

Event and scripted cameras (cut scenes, conversations, boss introductions) take another branch of
`sub_82524B60`: no input and no free-camera step. **[V]** (gdb read/write watchpoints, call traces, the field
watcher)

## 3. Root cause of the restrictions (M6B)

| Restriction | Cause | Status |
|---|---|---|
| Hardly any vertical look (+10 degrees up, nothing down) | vertical mode 3 is **absolute**: the pitch follows the stick position times the key's limit (-0.05..0.18 rad), so a full push gives only the limit; down is almost nothing | **[V]** |
| Snap back to the default height on release | the same mode 3: with the stick centred the target pitch is 0, and +2080 eases to it over the key's smoothing frames | **[V]** (field watch: +0.18 while held, geometric decay after release) |
| Slight upward drift while turning | the stick reader's bug: a y below 0.1 is made 0.1 | **[V]** |
| Slow, soft turning (about 69 degrees per second at full deflection; 5.2 s per turn) | the horizontal rate (key factor 0.1 rad per frame) goes through +2076 and the bake into +1680, but the smoothing pulls the next frame's camera most of the way back: only about 40 % of every turn stays, spread over several frames | **[V]** (a 90-degree impulse kept 36 degrees) |
| Big dead zone | 12,800 of 32,767 (39 %) | **[V]** |
| Mouse look | the port's M5A mapping turned mouse movement into a right-stick deflection, so it inherited all of the above | **[V]** |

## 4. The modern camera (M6B)

**Status: manually approved (2026-10-23).** The user played the final M6B build (orbit, Camera Distance, Field
of View, the expanded sensitivity, the PC page) and approved the camera's behaviour as it is. It is the
reference implementation: the orbit mathematics, pivot and framing, vertical and horizontal rotation, distance
and collision behaviour, mouse and controller response, sensitivity curves and ranges, recentring and every
default stay as they are; later milestones must not change them (the camera tests and their fault injection,
`camera.checks_fault_injection`, guard them).

### Orbit correction: root cause

The first M6B camera stopped orbiting when looking up and tilted in place; the character left the view.
Measured (`camera_orbit`, every frame): the aim off the character up to 87.5 degrees, the eye never lower
than 219.9 units above the character's feet. Three causes, each found by measuring the stages separately:

1. The port's own `ApplyPitch` held the eye 40 units above the camera target and then rotated the view around
   the eye (by design then; it was the tilt).
2. The game's collision keeps the eye 150 units above the ground below it (parameter 248, only used there):
   with the tilt removed, the orbit still stopped at about 24 degrees up, 88 units from the pivot.
3. The game's position smoothing (+496/+508) and the collision's height smoothing moved the camera towards
   the orbit along straight lines, 25 % and 30 % per frame: the camera lagged the orbit by several frames,
   and a fast vertical move cut through near the character (73.7 units from the pivot, 5.9 degrees off it,
   measured with the collision switched off: the position smoothing alone).

No other step overwrites the final transform: the rows are built from the collision's output, and the port
restores the game's unpitched rows only for the game's own readers, after the view has been taken for the
renderer (below).

On when PC Settings > Modern Camera is On (the default); with it Off none of this acts (the original camera
exactly). It takes over only the gameplay cameras: free camera on, horizontal mode 1 or 2, vertical mode 3, and
only on the gameplay path (not event cameras). Other cameras keep the game's handling, with the mouse added as
stick input.

- **Stick**: the right stick is read here instead of the game's reader (`sub_82523D78`, right stick only): a
  radial dead zone (PC Settings, default 25 %), then (deflection beyond it)^1.5, direction kept (no axis bug).
  Speed per second of real time (frame-rate independent; with the M6D fixed tick each update is one tick, 1/30 s,
  K43): Stick Sensitivity steps 1-10 are 15 degrees per
  second each (15 to 150), steps 11-20 each 14.9 % more (172 to 600 at step 20; `camera_math.h`
  StickRadiansPerSecond), horizontally; 65 % of that vertically. The game's Camera Normal/Inverted option still inverts it.
- **Mouse**: in gameplay the pointer is captured (relative mode) and its counts go to the camera directly
  (`KufInputDriver::TakeMouseLook`), no stick emulation and no acceleration: Mouse Horizontal / Vertical steps
  1-10 are 0.025 degrees per count each (0.025 to 0.25), steps 11-20 each 19.6 % more (0.30 to 1.5 at step 20;
  MouseRadiansPerCount). The steps up to 10 are those of the first M6B build (saved settings keep their speed;
  the fine steps stay at the low end). Mouse Y-Axis inverts the mouse only. Outside gameplay the pointer is free
  (M5A).
- **Horizontal**: the camera keeps the view direction it wants. Each frame, before the geometry step, it sets
  the game's own turn (+2076) to the whole remaining difference between that direction and the one the game is
  about to show. The game's geometry then shows exactly that direction, and its own state (the bake into +1680)
  converges behind it; the camera's position follows through the position smoothing below, with the game's
  own lag. So the turn is 1:1 with the input and continuous through 360 degrees. Once the game has taken in the turn (the correction stays below 0.003 rad for
  10 frames) and no new turn comes, the camera is the game's own again: it trails behind the moving character as
  the original does (a held direction changed where scripted walks lead: `gameplay.combat_loot` missed the
  lizardmen). The game's pull-back is never larger than the last correction; a bigger
  difference is the game turning its own camera (the right-stick click "centre the camera behind you", which
  the tutorial requires, or a camera change), and then the game's direction and default height are taken.
- **Vertical**: until the player moves the view vertically it is the game's own (its cameras frame scenes
  differently, and scripted routes depend on it). After a vertical input the camera keeps the view's angle above
  the horizon (held absolute, so it does not drift as the game's camera settles), limited to +/- Vertical
  Range. The game's +2080 stays 0. With Camera Recentering On, after 1 s without vertical input the angle eases
  back to the game's default view (time constant 0.35 s), which then is the game's own again.
- **Orbit** (`ApplyOrbit`, camera_math.cpp; after the game's geometry step): the eye orbits the **pivot**, the
  game's own camera target (the key's target offset above the character's feet, +464: 180 units for Celine,
  just above her head; it follows the character as in the original and is what the original camera looks at),
  to that angle, and the view is always aimed at the pivot. Looking down the eye rises over the character to a
  top-down view; looking up it goes **below the pivot** and the character is seen from below. The distance is
  the game's own until the eye nears the ground: its depth below the pivot is limited softly (exponential knee,
  60 units wide) to 30 units above the character's feet, and from there further upward look brings the camera
  in along the ground (distance = allowed depth / sin(angle)), as in an MMO camera, never closer than 80
  units. Position and orientation come from the same eye and pivot every frame, so they cannot disagree.
- **Position smoothing**: the game smooths the camera position along a straight line after the geometry step
  (section 2). Applied to the orbit it lagged behind every vertical move and on fast moves cut a chord past
  the character; the same happened horizontally at high sensitivity (a 150-degree turn in one frame swung the
  camera to 227 units from the character, `camera_sensitivity_max`). So for the modern camera the smoothing is
  done here, by the game's rule and frame counts (read from the camera), on the game's own unpitched eye and
  target, but around the target: the eye's offset from the (smoothed) target moves the same fraction per frame
  in direction, horizontal distance and height (`SmoothAround`). Without a turn that is exactly the game's
  straight line; a turn lags as much as before but travels around the character. The orbit is placed around
  the smoothed eye and target, and the game's smoothing is given the result as its state, so it leaves it
  unchanged. The camera still trails the character as before.
- **Distance** (PC Settings > Distance, 50-200 %, default 100 % = the game's own): the game's camera offset
  from the pivot is scaled before the orbit, so the vertical orbit, its ground limit and the pivot are those of
  the approved camera at any distance. A new setting is eased in (time constant 0.15 s), so it previews at once
  without a jump. Range: at 50 % (354 units here) the character fills the lower middle of the view (closer
  shows only her back and head); at 200 % (1415 units) she is small but clear and the collision acts often in
  the forest; the soft ground limit and the minimum orbit distance (80) need no change.
- **Collision feedback**: the collision step also pulls the game's baked eye (+1680/+1688) halfway towards a
  global (0x82ABAA70) that holds the eye it was given (measured: the same eye with or without a hit). For the
  game's own camera that is no pull; with the orbit it pulled the game's camera towards ours, so the game's
  distance drifted with the view angle (707 -> about 605 units held 12 degrees up, the "pull-in under the
  trees" of the first build: there was no hit) and a longer distance would feed on itself. The pull is now
  made in the game's terms (the fraction of the way to our eye the global holds, applied to the game's own
  eye), which is no pull without scenery, as for the game's own camera. The distance therefore stays the same
  at every view angle.
- **Obstruction**: the game's collision moves the eye out of the scenery at once, in towards the character,
  sideways around a trunk or up from the ground, and lets it go back as soon as the way is clear. A larger
  correction is still taken at once (the camera never shows the inside of the scenery), but a correction that
  is no longer needed is let go gradually: the correction (eye out - eye in) eases towards the new one (time
  constant 0.3 s), plus at once whatever of the new one the eased one does not cover; it is kept relative to
  the camera's arm, so it turns with the view. Measured while turning (2 degrees per frame) at 200 % in Shadow
  Depths: the largest increase of the distance in one frame 48.6 units (before: 346, a snap back out), at 50 %
  7.8.
- **Collision**: the pitched camera goes into the game's own collision step (it runs after the geometry), so
  the scenery still pushes the camera in (measured in `camera_distance_far`: the forest's trunks pull a 1415-unit
  camera in to as little as 260 units while turning). Two of its settings are relaxed while the view is pitched: its ground clearance
  (parameter 248, 150 units, which kept every low orbit 150 units up) becomes 30 units for the modern camera,
  and its height smoothing is undone during vertical input and 0.5 s after it (the unsmoothed height is
  recovered exactly from the global's change), so the eye follows the orbit instead of lagging behind it.
  Walking over uneven ground keeps the game's height smoothing. If the collision brings the camera closer
  than 140 units while looking up, further upward input is eased off (`LimitUpward`), and below 80 the view
  backs down.
- **What the game sees**: after the camera update, the camera rows (+16..) are turned back by the vertical angle
  that was added (about the camera's right axis; the position becomes the game's own eye, moved in as far as
  the collision moved ours). The game's systems (camera selection, the character, saves) therefore see the
  orientation they were made for, with the player's horizontal direction; only the view handed to the renderer
  (`sub_825246D0`, for that call) is pitched. Without this, the camera selection switched keys when the view
  pointed up.
- **Events**: after an event camera the modern camera starts again from the game's view (direction and
  height).
- **Field of view** (PC Settings > Field of View; default the game's own): the game's projection
  (`sub_826FC108`, a D3DX-style perspective matrix: y scale cot(fov / 2), x scale that / aspect) takes a
  **vertical** angle from the camera key (+60 in the key, smoothed into X+528; Celine's gameplay camera 0.6 rad =
  34.4 degrees vertical, 57.6 horizontal at 16:9) and the viewport's aspect (+728 / +732). `sub_825246D0` hands
  X+528 to `sub_821EA080`, which stores it in the view's per-frame slot (double-buffered) and derives a
  draw-distance term from it. After that call (from the gameplay path only: return address 0x8252476C, not the
  event cameras' `sub_82524530`), the slot gets the player's angle: the setting is the horizontal angle at
  16:9 of the gameplay camera, and each key's own angle is scaled by the same factor (tan of the half angles),
  so cameras the game made narrower stay narrower. The vertical angle is what is set, so a wider screen sees
  more at the sides and nothing is stretched (Hor+; ready for M6C's ultrawide). The draw-distance term keeps the
  game's value; the projection, the culling and the shadow pass read the slot, so they agree. HUD and menus use
  an orthographic projection (`sub_826FC368`) that does not read it. Measured: at 60 and 110 every projection
  built in gameplay has exactly the setting's angle at aspect 1.7778, scales as computed; the title scene's own
  cameras (0.5 rad, pi/4) are not changed. With Modern Camera Off nothing acts.
- **Handover to the gameplay angle (M6B verification)**: the slot is written by the game side with the index
  at 0x82AE760C (toggled each game frame by `sub_821F4CD8`) and read by the render side (`sub_821E9ED0`, its
  projection) with its own index at 0x82AE7610 (toggled by the render side, `sub_821A3C88`): the render side
  draws each game frame with the angle written for that frame, one game frame behind the game side. When
  gameplay starts, the frame written before the switch (the loading view, 0.5 rad) is still drawn with its own
  angle; the frame written with the gameplay angle is drawn with the setting. The original game does the same
  (Modern Camera Off: 0.5 rad, then 0.6). `--kuf_trace_projection` logs the handover (which write each render
  projection reads) and the FOV tests check it. So there is no frame drawn with a wrong field of view, and
  nothing to fix (writing the setting into the other slot early would change a frame the game draws with its
  own camera).
- **Simulation**: the game's update and the 30 fps simulation are unchanged; only camera values are set.

Tests (sandboxed, `--kuf_trace_modern_camera` gives the view's pitch and yaw five times a second and the
collision's distance ratio; tools/menu_test.py `log_value` checks):

| Test | Checks |
|---|---|
| `camera.*` unit tests | stick shaping (dead zone, curve, direction), speeds, orbit (above and below the pivot, aimed at it, soft ground limit, continuity at 0.05-degree steps, limits), basis restore, recentring |
| `camera_distance_near`, `camera_distance_default`, `camera_distance_far` | 50 %, 100 % and 200 %: the orbit distance (353.9 / 707.9 / 1415.7), full turns at the default height, low and high; aimed at the pivot, above the ground, the scenery in the way (collision), released gradually (largest increase per frame bounded: 15 / 45 / 60; measured 7.8 / 37.9 / 48.6), held still back on the orbit |
| `camera_sensitivity_max`, `camera_sensitivity_min` | levels 20 and 1: mouse 100 counts = 150 / 2.5 degrees, 40 up = 60 / 1; stick half way 1 s = 115.5 degrees (max); full 8 frames; a steady turn without jitter (change of the per-frame movement 0.66 / 0.01) and without swinging close to the character |
| `camera_fov_min`, `camera_fov_max` | 60 and 110 degrees: every projection built in gameplay (once a second, `--kuf_trace_projection`) has the angle and the viewport's aspect, matrix scales as computed; at the handover every render frame written with the gameplay angle is drawn with the setting, the frame before with its own 0.5 rad |
| `camera_orbit` | every frame: slow sweeps (1 degree per frame) to +85 and -85 and back, jumps, fast flicks over the whole range, the right stick; aim at the pivot within 0.5 degrees, the rendered eye on the orbit within 1 unit, never closer than 140 units, eye at least 25 units above the feet, eye movement per frame and its change bounded on slow moves, limits reached |
| `camera_mouse` | 400 counts up = +40.00 degrees, held 3 s; limits +/-85.00; 900 counts right = +90.00; a full turn back to 0.00; the character walks where the camera looks (0.4 degrees off); looking down keeps the distance (within 15 units; measured -1.6); the right-stick click centres the camera (the game's 180-degree turn taken) |
| `camera_stick` (virtual pad) | full right 2 s = 150 degrees; 23 % (inside the dead zone) = 0; 50 % = 28.85 (expected 28.9); up 1 s = +48.7, held after release; full left back |
| `camera_recenter` (keyboard camera keys) | +48.75 in 1 s; still up 0.5 s after release; back to the default view after 3 s; 75 degrees per second turning |
| `camera_original` (Modern Camera Off) | the original's small vertical range and snap back, its own turn rate, mouse as the right stick; Distance 200 % and Field of View 110 set but not acting (707 units, 0.6 rad); the original's own handover (0.5 rad, then 0.6) |
| `camera.checks_fault_injection` | on copies of the camera tests' logs, 11 injected regressions are caught: an eye snap of 75 units, the aim 3 degrees off, the distance creeping in while looking down (707 -> 616), a collision release snapping back (300 / 346 units), no collision at 100 %, a stale or wrong field of view, the field of view acting with Modern Camera Off, the top mouse level as slow as the old one |
| `gameplay.*`, `campaign.*`, `savegame.*` | the existing combat, loot, movement, save and campaign tests, now with the modern camera on |

## 5. Limitations

- Looking up, the eye stops 30 units above the level of the character's feet (not the actual ground, which
  the game's collision still checks): on a slope falling away behind the character the camera could go lower
  but does not. Near the upper limit the camera comes in along the ground: at +85 degrees it is 150 units from
  the pivot, nearly under the character, and the character's legs fill much of the view.
- The pivot is the game's own camera target, just above the character's head: at moderate upward angles the
  character sits low in the frame. A lower pivot (chest) would frame it more centrally but would bring the
  ground limit, and the slide in, earlier.
- Under low ceilings and against walls the game's collision pulls the camera in (it did in every place
  tested), but not every interior has been visited.
- When the scenery comes between camera and character, the camera comes in at once, as the original does (it
  never shows the inside of a trunk); only going back out is gradual. With a far camera in the forest that is
  frequent (at 200 % a trunk passing behind the character moved the camera in by up to 740 units in a frame).
  The same for a rise of the ground behind a far, low camera (the view angle dropped 3.4 degrees in a frame).
- Further out, the slide in along the ground when looking up starts at a smaller angle (at 200 % at about the
  level view instead of about 8 degrees up) and is faster per degree: the same geometry as at 100 %.
- Field of View is defined at 16:9 and keeps the vertical angle on other aspect ratios (the math is tested for
  21:9); the port renders at 16:9 until M6C, so a wider screen has not been seen yet.
- At the highest mouse levels one count is up to 1.5 degrees: slow, precise mouse movement is coarser there
  (the low levels keep the fine steps).
- Camera selection (back/side cameras), auto-target turning and the collision use the game's own logic and can
  still move the camera as in the original.
- Cameras other than the gameplay ones (air attack, transformations) keep their original vertical behaviour
  (rate modes with their small limits); the mouse drives them as a stick.
- After cut scenes the view starts again from the game's own camera (as the original does).
- Mouse movement is taken as SDL's relative motion (raw, without system acceleration where SDL provides it).
