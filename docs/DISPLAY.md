# Display: resolutions, display modes, widescreen and ultrawide (M6C)

The game was made for one output: a 1280x720 frame at 16:9 on a television. The port draws it for any window
or monitor shape with the 3D world actually rendered for that shape (more of the world at the sides of a wider
screen, nothing stretched or cut off), keeps the HUD and menus in their original 16:9 proportions, and offers
the PC display modes in the game's own Settings (PC Settings > Display). Code: `src/runtime/display.cpp` (game
hooks), `src/runtime/display_window.cpp` (window, monitors, modes), `src/runtime/display_math.cpp` (pure math,
unit-tested); the menu page in `src/runtime/pc_settings_menu.cpp`.

## 1. How the image is made

The game draws one 1280x720 frame: its render targets, resolves, post-processing chain (an HDR scene target,
a quarter-size blurred copy, the composite) and its 2D layer are all built around that size, and it hard-codes
it in many places (viewports, resolve rectangles, the 2D renderer's `px / 1280`). The port does not change the
frame's size in the game; it changes **how many pixels each of the frame's pixels is rendered with**: the SDK
renders the whole frame at a whole multiple per axis (`draw_resolution_scale_x / _y`). At 3x3 every render target,
every resolve and every texture the game makes from a resolve is three times wider and taller, so the 3D scene,
its shadows and effects, the post-processing, the HUD, the menus and their text are all drawn at 3840x2160.

```
                                        render scale (Render: Native = the smallest multiple >= the output)
 3D views ── projection with the OUTPUT's aspect ──┐            │
                                                   ├─► frame 1280x720 x scale ─► presenter: shown on the output
 HUD, menus ── compressed into a centred 16:9 box ─┘   (e.g. 3840x2160)          (exact, or filtered down;
 full-screen layers (fades, pause dimming) ── left full width                    stretched for the aspect)
```

- **Render resolution** (PC Settings > Display > Render, `[display] render_scale`). **Native** (the default)
  takes, on each axis, the smallest multiple of the frame that is at least the output:

  | Output | Render scale | Rendered frame | Shown |
  |---|---|---|---|
  | 1280x720 | 1x1 | 1280x720 | exact |
  | 1920x1080 | 2x2 | 2560x1440 | filtered down to 1920x1080 (1.33x per axis) |
  | 2560x1440 | 2x2 | 2560x1440 | exact |
  | 3840x2160 | 3x3 | 3840x2160 | exact |
  | 3440x1440 (21:9) | 3x2 | 3840x1440 | filtered down horizontally (1.12x) |
  | 2560x1080 (21:9) | 2x2 | 2560x1440 | filtered down vertically (1.33x) |
  | 5120x1440 (32:9) | 4x2 | 5120x1440 | exact |
  | 1920x1200 (16:10), 1600x1200 (4:3) | 2x2 | 2560x1440 | filtered down |

  The scale must be whole: the SDK emulates the console's EDRAM in 80x16-sample tiles and every resolve and
  texture address in a scaled form, each guest pixel standing for exactly scale_x x scale_y host pixels. Where
  the output is not a whole multiple of 1280x720 (1080p is 1.5x), Native renders more pixels than the output has
  and the presenter filters the frame down (bilinear): never fewer rendered pixels than output pixels, in effect
  supersampling. **1x-4x** fix the scale (1x = the original 1280x720 frame, for weak GPUs; 3x at 1080p = 2x2
  supersampling). The render scale is chosen at start (`[kuf-display] start: ... render scale 3x2 (frame
  3840x1440); Render Native`) and a change takes effect at the next start: the row shows `*` while the running
  scale differs from what the setting needs for the current output (also after a display change that needs
  another scale). It cannot change in a running game: the game renders some data once and keeps it only in the
  scaled form (its lighting cube maps, built at level load from the environment), so switching scales would
  lose it.
- **Colours at scale** (KNOWN_ISSUES K31, fixed). The game builds four lighting cube maps at level load on the GPU:
  a pass projects the level's environment cube map into 64x64 targets, a second pass reduces those into a 4x4
  texture of lighting coefficients, and a third expands the coefficients into six 32x32 faces per cube. The
  reduction pass's pixel shader has no interpolated inputs: it reads the **pixel position** (Xenos `ps_param_gen`,
  "VPOS") and uses it as an index into its weight tables. The SDK gave that position at render scale 2 as
  `floor(host position) / 2`, e.g. 3.5 for the second host pixel of guest pixel 3; the hardware gives 3. Three of
  every four host pixels therefore fetched wrong weights (and the pass's own "outside the table" value, 1,0,0),
  the cube-face pass point-sampled them, and every lit surface had a strong red cast (green and blue light
  missing). SDK patch 0008 gives every host pixel the integer position of the guest pixel it covers; the cube maps
  are now bit-identical to the unscaled ones at every scale, and the frame's colours match (tests below). Found
  with the GPU trace of SDK patch 0009 (`--gpu_trace_frame`, per-draw and per-resolve logs and data dumps):
  the colour difference first appeared in the first resolve of the scene, whose only resolved inputs were the
  shadow maps (identical) and the cube maps (wrong), and from there back through the cube pipeline to the
  coefficient pass. The other shaders that read the pixel position use it for a shadow-filter dither; with the
  integer position they keep the original pattern (each guest pixel's pattern value over its host pixels).
- **3D views (Hor+ / Vert+)**. The game builds its perspective matrices with `sub_826FC108` (vertical angle,
  aspect) from three places: the render side's view update (`sub_821E9ED0`, return 0x821E9F74), the game side's
  rebuild (`sub_821E9FB0`, 0x821EA044) and a matrix from a view's stored aspect (0x82216C8C). Every one of these
  that has the game's 16:9 aspect gets the output's aspect instead, and the view's stored aspect (+744) is set to
  it, so drawing, culling (its scales come from the matrix) and the third matrix agree. Wider outputs keep the
  game's vertical angle (**Hor+**: 57.6 degrees horizontally at 16:9 become 72.9 at 21:9 and 95.5 at 32:9);
  narrower outputs (16:10, 4:3, 5:4) keep the 16:9 horizontal angle and see more above and below (**Vert+**), so
  the 16:9 view is always entirely visible. The presenter then stretches the frame to the output's shape, which
  makes the 3D image geometrically correct. The render scale does not change the projection, the depth range or
  the camera: the same matrices are rasterised with more pixels.
- **Field of View** (PC Settings, M6B) keeps its meaning: the value is the horizontal angle *at 16:9*, i.e. it
  sets the vertical angle; wider screens add to it (Field of View 90 is 126.9 degrees at 32:9). The approved
  camera is otherwise untouched.
- **HUD and menus**. The game's 2D renderer (`sub_8222C360`, called per 2D layer) converts each queued quad's
  pixel rectangle to clip space on the CPU, `x = (px / 1280 - 0.5) * 2`; its vertex shader passes positions
  through. A mid-asm hook where its two conversion paths meet (0x8222C9B4, `config/v1/codegen.toml`: the quad's
  four vertices at `r25 - 32`, stride 24) scales the quad about the centre by the 16:9 box's share of the output
  (x by 16/9 ÷ aspect on wider outputs, y by aspect ÷ 16/9 on narrower ones). After the presenter's stretch the
  HUD and menus appear at their original proportions in a centred 16:9 box: the menus, the HUD (gauges, item
  circles, minimap), message windows, the enemy name bar, the title, loading and status screens. A quad that
  spans the whole width (or height) is a full-screen layer (fades, the pause dimming, full-screen effects) and is
  left alone, so it still covers the whole output. Being clip-space geometry drawn into the scaled frame, the HUD
  and the menus are rasterised at the render resolution: text and gauges are as sharp as the 3D scene (their
  textures are the game's own, of fixed size); the 2D layer is not a separately scaled image.
- **Movies** (the title loop and the attract movie, the game's only two) are drawn by the game's movie player (the
  console SDK's WMV player behind a small command queue), not by the 2D renderer, over the whole frame. While one
  plays (hooks on the game's play and stop calls, `sub_826E90C0` / `sub_826E91F0`) the output is treated as 16:9:
  the presenter pillarboxes the frame and 2D quads are left as they are, so the movie and the title screen's
  "Press START" and menus over it keep the original picture. Widened again when the title scene stops the movie
  (logged: `[kuf-display] movie ...: shown at 16:9`, `movie stopped`, `presented ...`). The videos themselves keep
  their encoded resolution (scaled up into the frame).
- **Widescreen Off** (PC Settings > Display) draws exactly as the original (16:9 projections, HUD unscaled) and
  the presenter pillarboxes or letterboxes the 16:9 image (the SDK's `present_letterbox`); the render scale still
  applies.

What was measured, not assumed: captures at 21:9, 32:9, 16:10 and 4:3 (simulated outputs) show the 3D view
widened, the HUD and every menu tested in the box with correct proportions, and the pause dimming at full width;
a three-minute fight at 21:9 (enemy name bar, loot messages, damage, minimap) shows no misplaced element. A 2D
element that the game positions from a world point far from the screen centre would be drawn nearer the centre
by the box's factor; none was seen in the tested scenes (section 6).

## 2. The output

The output is the window's client area in pixels (`rex::ui::Window` actual physical size), followed on every
resize, scale change and restore: dragging a window to another shape, a monitor change, a compositor resize or an
Alt+Tab that changes the window take effect from the next frame (aspect, HUD box, pointer mapping), without a
restart. In offscreen test runs (no display) the output is simulated with `--kuf_display_size=WxH`
(`tools/run_sandboxed.sh` passes 1280x720 unless a run sets its own).

## 3. Display modes and resolutions

| Mode | What it is | Size |
|---|---|---|
| Windowed | a window centred on the chosen monitor; resizable, the game follows its size | the window size: each distinct size of the monitor's modes that fits its usable area (desktop less panels), plus 1280x720 |
| Borderless | a borderless window covering the monitor at its desktop resolution (SDL fullscreen-desktop) | the desktop's (fixed) |
| Fullscreen | exclusive fullscreen in one of the monitor's display modes (`SDL_SetWindowFullscreenMode`) | the monitor's modes with their refresh rates |

- **Resolutions are never a fixed list**: they are the modes the system reports for the monitor (SDL3), in pixels
  (HiDPI desktops report their scale; it is applied). Any shape works: 16:9 (1920x1080, 2560x1440, 3840x2160),
  16:10, 4:3, 5:4, 21:9 (2560x1080, 3440x1440, 5120x2160), 32:9 (3840x1080, 5120x1440).
- **Monitors** by name as the system reports it (Hyprland: the connector, e.g. `HDMI-A-1`). A saved monitor that
  is not connected falls back to the primary one (logged); its name stays in the file, so it is used again once
  connected.
- **Exclusive fullscreen on Wayland**: Wayland has no mode switching for applications; SDL emulates it (the
  window is fullscreen at the desktop resolution and the compositor scales the chosen mode's image up). It works
  and is reliable as a "render at this resolution, fill the screen" mode, but the monitor does not change mode
  and the refresh rate stays the desktop's. On X11 and Windows SDL switches the monitor's mode. Tiling
  compositors (Hyprland) may override a window's size: the game follows whatever size the window gets.
- **Focus and Alt+Tab**: input is released on focus loss (M4); SDL minimises an exclusive-fullscreen window
  when it loses focus (its default) and the output follows the restored window.

## 4. In the game: PC Settings > Display

The third page of the PC page: Down past Restore Defaults (second page: Distance, Field of View), then Down past
Field of View. Up from its first row goes back. In-game Settings and the title screen's SETTINGS both have it.
The labels show the values; Left/Right change them:

| Row | Values |
|---|---|
| Mode: Windowed / Borderless / Fullscreen | |
| Size | `Window: 1920x1080` (windowed sizes), `Desktop: 3840x2160` (fixed), `3440x1440 100Hz` (fullscreen modes); Right = larger |
| Monitor: N name | the connected monitors |
| Widescreen: On / Off | takes effect at once (projection, HUD box and presenter), no confirmation (Off = the original 16:9 image, pillarboxed) |
| Apply | `Apply *` when Mode, Size or Monitor differ from the display in effect |
| Render: Native / 1x / 2x / 3x / 4x | the render resolution (section 1), saved at once, in effect from the next start; `Render: Native *` while the running scale differs from what it needs |

**Apply** changes the display and the row asks **Keep? (A) Yes (B) No**: Select keeps it (saved when you leave
the PC page, like every PC setting); Back, leaving the page, or 15 seconds without an answer (a timer, so it
reverts even if the new mode shows nothing) restore the previous display. Changing the mode picks a matching size
(fullscreen: the desktop's mode; windowed: the largest window smaller than the desktop).

## 5. pc_settings.toml

```toml
[display]
mode = 'borderless'        # windowed | borderless | fullscreen
width = 0                  # windowed: window size; fullscreen: mode size; 0 = the desktop's (both or neither)
height = 0
refresh_millihertz = 0     # fullscreen: the mode's refresh rate; 0 = the desktop's
monitor = ''               # the monitor's name; '' = primary; a disconnected one falls back to primary
widescreen = true          # false: the original 16:9 image
render_scale = 0           # 0 = native (the smallest multiple of 1280x720 >= the output); 1-4 = that multiple
```

Validated like the M6B keys (an invalid value is logged and gets its default; a size with one side only is
ignored; text with control characters or over 256 bytes is refused); unknown keys are kept; an M6B file (no
`[display]`) loads with the defaults and gets the table when next saved; `--kuf_pc_settings=display.mode=windowed,...`
overrides for one run (text values without quotes). The game's own saves are never touched.

## 6. Limitations

- The render resolution is a whole multiple of 1280x720 per axis (section 1): outputs that are not one (1080p,
  3440x1440, 2560x1080, 16:10, 4:3) are rendered larger and filtered down by the presenter (bilinear), which
  costs more than an exactly native frame would. Exactly native: 1280x720, 2560x1440, 3840x2160, 5120x1440,
  3840x1440 and other whole multiples.
- The render resolution follows the display at the next start, not during play (section 1).
- Movies keep their encoded resolution; the game's own textures (HUD art, fonts, the world) keep theirs: native
  rendering makes edges, geometry, shadows and effects sharp, not the texture detail.
- The presenter's downscale is bilinear; offscreen test runs do not exercise the presenter's scaling to a real
  window (the captures are the rendered frame).
- 2D elements positioned from a world point (if the game has any far from the centre) would be drawn nearer the
  centre by the HUD box's factor; none seen in the tested gameplay (title, menus, status, inventory, a fight).
- Exclusive fullscreen is emulated on Wayland (section 3); real mode switches were not exercised on this machine.
- Real-hardware checks of window sizes, monitors and mode switches need the player's session: the automated
  tests run offscreen with simulated outputs and a simulated monitor (`--kuf_display_modes`).
- Movies are shown at 16:9, pillarboxed (or letterboxed) like the original: the title screen and its menus, which
  are drawn over the title-loop movie, are therefore at 16:9 too, and toggling Widescreen there shows no change.
