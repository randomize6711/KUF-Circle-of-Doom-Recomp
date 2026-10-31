# PC Settings

The PC port's own settings are part of the game's Settings menus; there is no launcher, overlay or separate
program. They are stored in `pc_settings.toml` next to the port's other user data, never in the game's saves.
Implemented in M6B (camera), M6C (display) and M6D D5 (frame rate); later milestones add graphics rows the same way.

## In the game

**M7 menu overhaul (not yet tested in the game: docs/KNOWN_ISSUES.md K63):** there is no separate PC Settings any
more. Both Settings menus open on one category list, by function, and the game's own eight options live in those
categories next to the port's:

- **In game**: options menu (Back / Escape) > Settings.
- **Title screen**: SETTINGS. (M7 UX fix 5: the console's **Change Storage Device** row is gone, as is its question
  after Press START, "Do you want to select a storage device now?", which the port answers Yes for the player, and
  the notice "Your game progress will not be saved if the storage device is removed during play.", which the port
  confirms whenever the game posts it: saves always go to the port's own folder, `content_root` / the user data
  directory, as before; the save system, its atomic writes and its genuine error messages are unchanged.
  `--kuf_storage_row=true` shows the row again, `--kuf_storage_prompt=ask` the question and the notice; the test
  scripts ask.)

| Category | Rows (game: the game's own option, its widget and its code) |
|---|---|
| Display | Mode, Resolution (with the refresh rate in Fullscreen), Monitor, Widescreen, then VSync, FPS, Interpolation, World Objects, Animation, Effects, Render (resolution scale), Brightness (game); Apply (last; the 15-second confirmation) |
| Camera | Distance, Field of View, Vertical Range, Recentering, Modern Camera |
| Audio | SFX, Music, Voice (game) |
| Mouse & Keyboard | Mouse Horizontal, Mouse Vertical, Mouse Y-Axis, Key Bindings (opens the page) |
| Controller | Stick Sensitivity, Stick Dead Zone, Controller Buttons (opens the page), Camera Normal / Inverted (game: the controller's vertical axis), Vibration (game) |
| Gameplay | Prompts, Skin (game), Blood (game) |
| Restore Defaults | (below the list) |

The M7 UI follow-up merged the short-lived Graphics category into Display (display setup first, then rendering and
the frame rate; the headings it had at first read as empty rows and were removed). Rows stand one row spacing apart
from the first on every page (the layout's own rows are not evenly spaced). The game's selector arrows (Skin) and
toggle halves (Blood, Camera, Vibration) take clicks: the row is selected and gets the game's Left / Right. Brightness is the game's own option, shown in the list as a value ("Brightness:
50%") and changed by the game's own code (its row made the selected one for that event): Left / Right step it,
Select steps forward and wraps. Nothing is duplicated and no setting moved its storage: the game's options are still the game's (saved with its
options when Settings closes, as before), the port's are still in `pc_settings.toml` under the same keys, so values
from earlier builds stay. Key Bindings and Controller Buttons are plain entries that open their page ("Key Bindings >": no slider; before,
the slot's slider showed beside them); Back returns to them. **Scrollbar (M7):** Display (13 entries), Key Bindings
(24) and Controller Buttons (12) show eight rows with a scrollbar beside them (the game's slider track and bar
images, upright): click the track to show that part, drag the thumb; the wheel, Up / Down and a controller move the
selection and the list follows it. Pages that fit show none. It never moves a settings slider, and a slider's drag
never moves it. **Version:** the title SETTINGS shows the installed build in its bottom-right corner ("Version
0.7.0-dev.157+g... | Built 2026-10-31 02:23 UTC": cmake/kuf_version.cmake; the time is the commit's for a clean tree,
SOURCE_DATE_EPOCH when set). No placeholder rows (no Bloom: the port has no such setting); the controller has no
response-curve row (the port has no such setting). On each page the rows stand one under the other: a game row is
moved up from its slot together with its widget (src/runtime/pc_settings_menu.cpp `PlaceRows`;
`--kuf_settings_compact_rows=false` leaves the slots where they are). Select opens a category; Back goes from a page
to the categories (that category selected), from Key Bindings / Controller Buttons to their page, and from the
categories closes Settings (the game's own Back: its options saved if changed; the PC file written if changed).
`--kuf_pc_settings_menu=false` gives the game's Settings exactly.

Before the overhaul (M7 feature 3 to the UI polish) the menus had a **PC Settings** row below the game's own rows,
opening a category page with **Controls**, **Camera**, **Display**, **Key Bindings**, **Controller Buttons** and
**Restore Defaults**; the tables below list the PC rows by their old pages. **Descriptions (M7 UX fix
4; placed below the list in the UI polish):** a region of its own below the last row (or Restore Defaults) and above
the button help, as wide as the rows, explains the selected row (what it does, and its cost or compatibility where it
has one) in the game's small left-aligned font; hovering a row selects it, so the description follows the pointer,
the keyboard and a controller alike. It never overlaps a row, a slider or the panel's logo (a text control cloned
from the help line; src/runtime/pc_settings_menu.cpp `Describe`; `--kuf_descriptions=false` leaves it out). Every page uses the game's own panel, rows, fonts, highlight, sliders, button help and menu sounds, and works
with a controller (D-pad, left stick, A, B), the keyboard (arrow keys, Enter, Escape) and the mouse (hover selects a
row, left click = select, right click = back, wheel = up/down), as the game's own Settings rows. Values change at once
(Left / Right); rows that show their value in the label also step with Select (forward, wrapping), so the mouse alone
can set them. **Sliders and the mouse (M7 UX fix 2):** a left click on a slider's track or knob (the game's volume and
brightness sliders and the PC sliders alike) selects its row and sets the value under the pointer; holding the button
drags it, releasing ends the drag (no other row is selected meanwhile). The value is reached by the menu's own
Left / Right steps (src/runtime/pc_settings_menu.cpp PointerSlider), so each slider keeps its range, step and what a
change does (volumes and the camera at once). `menu.sliders` tests clicks and drags on both kinds.

**Controls**:

| Row | Widget | Values | Default |
|---|---|---|---|
| Mouse Horizontal | slider | 20 steps: 1-10 in steps of 0.025 degrees per mouse count (0.025 to 0.25), 11-20 each 19.6 % more (0.30 to 1.5) | step 4 (0.1) |
| Mouse Vertical | slider | 20 steps, as above | step 4 (0.1) |
| Stick Sensitivity | slider | 20 steps: 1-10 in steps of 15 degrees per second at full deflection (15 to 150), 11-20 each 14.9 % more (172 to 600); vertical 65 % of that | step 5 (75; the original turns about 69) |
| Stick Dead Zone | slider | 0 to 50 % of the stick's travel, steps of 5 | 25 % |
| Mouse Y-Axis | label | Normal / Inverted (Left Normal, Right Inverted) | Normal |
| Prompts (M7 UX fix 4) | label | Auto (the device used last), Keyboard (the bound keys), Controller (the game's icons): docs/INPUT.md "Button prompts"; `--kuf_button_prompts` overrides it for a run | Auto |

**Camera**:

| Row | Widget | Values | Default |
|---|---|---|---|
| Distance | slider (value in the label) | 50 to 200 % of the game's own camera distance, steps of 10 | 100 % (the game's own) |
| Field of View | slider (value in the label) | the game's own (shown as 58), then 60 to 110 degrees in steps of 5: horizontal at 16:9 | the game's own (57.6 degrees horizontally at 16:9) |
| Vertical Range | label | 45, 60, 75 or 85 degrees above and below level | 85 |
| Recentering | label | On / Off: after a second without vertical input the view eases back to the game's default height | Off |
| Modern Camera | label | On / Off: Off is the original console camera exactly (including its mouse handling from M5A) | On |

Distance and Field of View apply at once (the game behind the paused menu shows them). The sensitivity steps up to 10
are those of the first M6B build, so saved values keep their speed; above 10 each step is a fixed factor more, so the
fine steps stay at the low end.

**Display** (M6C; the frame rate (M6D D5) and VSync joined it in the M7 UX fixes; details: docs/DISPLAY.md,
docs/HIGH_FPS.md section 21; Render and Brightness in the list since the M7 UI follow-up). Thirteen entries, eight
shown at a time: Up / Down past the edge, or the wheel, scroll. The
labels show the values:

| Row | Values | Default |
|---|---|---|
| Mode | Windowed / Borderless / Fullscreen (exclusive; emulated by SDL on Wayland) | Borderless |
| Resolution | windowed: the monitor's mode sizes that fit, plus 1280x720; borderless: the desktop's (fixed); fullscreen: the monitor's modes with their refresh rates | the desktop's |
| Monitor | the connected monitors (by name) | the primary one |
| Widescreen | On: the 3D view fills any screen shape (Hor+ / Vert+), HUD and menus in a centred 16:9 box; Off: the original 16:9 image, pillarboxed | On |
| VSync | On: frames are presented at the display's refresh (a FIFO swapchain: no tearing, can add latency; frames above the refresh rate are not shown); Off: presented as they come (immediate: lowest latency, can tear). Presentation only: the game's 30 ticks per second are unchanged. Takes effect at once (the swapchain is recreated through the presenter's resize path; SDK patch 0011) | Off |
| FPS | 30 (Original), 60, 90, 120, 144, 165, 240, Unlimited: the most frames drawn per second; the game itself always runs 30 ticks per second. Above 30 the frames between ticks are drawn from the tick's render data and, with Interpolation, show the moment between the ticks. A limit, not a promise: the hardware may draw fewer (native 4K and the title screen's movie cost most) | 30 (Original) |
| Interpolation | On: the camera and the player character move smoothly between ticks; Off: each frame between ticks repeats its tick | On |
| World Objects | On: enemies, other characters and items also move between ticks | On |
| Animation | On: the characters' limbs (skeletal poses) and the parts hanging from them also move between ticks | On |
| Effects | On: grass, mist and the other effects timed by the frame timer also move between ticks | On |
| Render | the render resolution: Native (the game's 1280x720 frame rendered at the smallest whole multiple that is at least the output, e.g. 3840x2160 on a 4K screen) or 1x-4x; `*` while a restart is needed for it (docs/DISPLAY.md section 1) | Native |
| Apply | (the last row since the M7 UI polish; greyed while Mode, Resolution and Monitor are as in effect: Select does nothing then) applies Mode, Resolution and Monitor, then asks "Keep?": Select keeps; Back, leaving the page or 15 s without an answer restore the previous display | |

Widescreen and VSync apply at once; Mode, Resolution and Monitor only with Apply and Keep; Render is saved at once and
takes effect at the next start. The frame-rate rows apply at once, also during play, without a restart (one tick
without frames between ticks while it switches); World Objects, Animation and Effects act only with Interpolation On
and FPS above 30. Select steps any row forward (wrapping), so the mouse alone can set them. The SDK's own `--vsync` is
something else: the guest vblank rate (docs/HIGH_FPS.md).

**Restore Defaults** sets Controls (with Prompts), Camera, the frame rate, Key Bindings and Controller Buttons to their
defaults (the display rows stay: they change the window and have their own Apply and confirmation).

The controller's vertical axis follows the game's own **Camera Normal/Inverted** option (in the game's Settings
page), as on the console; the mouse has its own option on the PC page.

What the modern camera does: docs/CAMERA.md.

## Title menu rows (M7 feature 4)

The title list has two rows of the port's below CREDITS (the list moved up two rows so that it ends where it did; the
game's own rows are unchanged; src/runtime/title_menu.cpp):

- **CHECK FOR UPDATES** (M7 updater, src/runtime/title_menu.cpp, texts in update_status.cpp): runs the installed
  package's updater (`kuf-update --json check`, docs/STEAM_DECK.md section 4) on its own thread (the row: CHECKING FOR
  UPDATES...), then shows the result in the game's own message box: the installed version, the available one with its
  release date and notes (shortened), and Install / Close; or "No update available" (installed and latest), "The
  update did not complete" (the updater's reason; nothing changed), "Updates are not set up" (not an installed
  package, or no release source configured). Install downloads with progress on the row (DOWNLOADING 45%, VERIFYING
  THE UPDATE...); selecting the row meanwhile asks whether to cancel (the updater stops before it changes anything).
  The new version is unpacked beside the running one and switched to atomically; the running game is not touched.
  Then "Version x.y.z is installed. Restart now?": Restart closes the game cleanly (as Quit Game, after any save) and
  the launcher starts the new version (KUF_RESTART_FILE); Later keeps playing the old one until the next start.
- **QUIT GAME** (M7 UX fix 6): asks "Quit Game? Are you sure you want to quit the game?" in the game's own message
  box. Yes (A / Enter / a click on the box) closes the game as closing the window does, once no save is in progress
  and with the PC settings written; No (B / Escape / right click) returns to the list. A click beside the box does
  nothing.

Controller, keyboard and mouse work as on the game's rows (hover selects, click confirms). Invite Friend is not there:
no invitation service exists yet (docs/SESSION_STATE.md, feature 7). `--kuf_title_menu_rows=false` leaves the title
list as the game has it; `--kuf_updater=<command>` sets the updater (default: `$KUF_UPDATER`, set by the launcher of
an installed package). Tested by `menu.title_rows` with a fake updater (tests/menu/fake_updater.sh).

## The file

`pc_settings.toml` in the user data directory (the SDK's user folder: on Linux `$XDG_DATA_HOME/kuf_cod/` or
`~/.local/share/kuf_cod/`, or `--user_data_root`). Nothing is written until a value is changed in the game.

```toml
version = 1

[camera]
modern_camera = true
vertical_range_degrees = 85
auto_recenter = false
distance_percent = 100
field_of_view_degrees = 0

[mouse]
sensitivity_horizontal = 4
sensitivity_vertical = 4
invert_y = false

[controller]
camera_sensitivity = 5
camera_deadzone_percent = 25

[display]
mode = 'borderless'
width = 0
height = 0
refresh_millihertz = 0
monitor = ''
widescreen = true
render_scale = 0

[frame_rate]
limit_fps = 30
interpolation = true
interpolate_objects = true
interpolate_animation = true
interpolate_effects = true

# Only when changed (M7): gameplay keys per control, and the controller's buttons in gameplay
# (the button = the physical button acting as it; always pairs that exchange).
[gameplay_keys]
a = "G"

[controller_buttons]
a = "x"
x = "a"
```

- `field_of_view_degrees = 0` is the game's own field of view; the sensitivity levels are 1 to 20 (files from
  the first M6B build, with 1 to 10 and without the two new keys, are read unchanged: the new keys get their
  defaults).
- `[display]` (M6C): `mode` windowed / borderless / fullscreen; `width` / `height` the window or mode size (0 =
  the desktop's; both or neither); `refresh_millihertz` (0 = the desktop's); `monitor` the monitor's name ('' =
  primary; one that is not connected falls back to the primary one and the name is kept); `widescreen`;
  `render_scale` (M6C follow-up) 0 = native, 1-4 = that multiple of 1280x720. An M6B file has no `[display]`: it
  is read with the defaults and gets the table when next written; an M6C file without `render_scale` renders
  natively.
- `[frame_rate]` (M6D D5): `limit_fps` 30 = the original (one frame per tick), 31-480 = at most that many frames
  per second (the menu offers 60, 90, 120, 144, 165, 240; another value in the file is used and shown as its
  number), 0 = unlimited; the four switches as the menu's rows. A file without `[frame_rate]` (M6B, M6C) stays at
  30 fps.
- **Command line over the file** (frame rate): `--kuf_frame_rate`, `--kuf_frame_interpolation`,
  `--kuf_frame_interpolation_objects`, `_bones` and `_effects` win over the saved values for that run, each flag on
  its own (the others come from the file); the log names them, the menu shows the values in effect, and the file
  keeps its own frame-rate values when it is written for another change. Changing a frame-rate row in the menu makes
  it a normal setting again (saved).
- **Validation**: every value is type- and range-checked; an invalid value is replaced by its default (a
  warning in the log); the valid ones are used.
- **Damaged file**: a file that is not valid TOML is renamed to `pc_settings.toml.bad-<time>` and the defaults
  are used; the game starts normally.
- **Unknown keys and tables** are kept when the file is rewritten (comments are not: the file says so).
- **Writing** is atomic: a temporary file, flushed, renamed over the old one, the directory flushed; the
  previous version is kept as `pc_settings.toml.bak`.
- **Tests and diagnostics** can override values for one run: `--kuf_pc_settings="camera.modern_camera=false,
  mouse.sensitivity_horizontal=6"`. The file is not changed unless the value is then changed in the menu.
- `--kuf_pc_settings_menu=false` gives the game's own Settings menus exactly (no categories, no PC rows).

## Implementation

| Part | Where |
|---|---|
| File: parse, validate, serialize, atomic write | `src/runtime/pc_settings.cpp` (pure; `settings.*` tests) |
| The settings of the running game | `src/runtime/pc_settings_runtime.cpp` |
| The extra row in the menu layouts (in memory) | `src/runtime/ui_layout.cpp` (`ui.*` tests) |
| The PC page in both Settings menus | `src/runtime/pc_settings_menu.cpp` (docs/MENU_ARCHITECTURE.md) |
| The modern camera | `src/runtime/modern_camera.cpp`, `src/runtime/camera_math.cpp` (`camera.*` tests) |
| The display (M6C) | `src/runtime/display.cpp`, `display_window.cpp`, `display_math.cpp` (`display.*` tests, docs/DISPLAY.md) |

Tests: `tests/settings/*.toml` (menus with keyboard and controller, title and in-game, both pages, persistence
across a restart, Restore Defaults, damaged and invalid files) and `tests/camera/*.toml` (docs/TESTING.md).

Verified for the M6B final verification (every item a test, all passing in the full regression):

| What | Test |
|---|---|
| Camera Distance and Field of View survive a restart | `pc_settings_view` writes 130 % / 75, `pc_settings_view_persist` starts with them (camera at 130 %, projection 75 degrees) |
| Expanded sensitivity survives a restart | `pc_settings_sensitivity` sets 20/20/20 in the menu, `pc_settings_sensitivity_persist` loads it and the camera uses it (100 mouse counts = 149.8 degrees) |
| Restore Defaults | `pc_settings_persist`, `pc_settings_view_persist`, `pc_settings_sensitivity_persist` (all keys back to the defaults, saved) |
| Files from earlier builds | unit test: a file of the first M6B build keeps its values, the new keys get their defaults |
| Invalid and damaged files | unit tests (each invalid value replaced with a warning; malformed TOML refused), `pc_settings_invalid`, `pc_settings_malformed` |
| Unknown keys kept | unit test (unknown keys and tables written back) |
| Game saves unchanged | `expect_saves_unchanged` in every in-game settings run (character files and GlobalSetting byte-identical) |
| Title and in-game menus | `pc_settings_title`, `pc_settings_view_title` (title SETTINGS); `pc_settings_keyboard`, `pc_settings_controller`, `pc_settings_view`, `pc_settings_sensitivity` (in-game) |
| Frame rate (M6D D5) | `frame_rate_menu` (keyboard), `frame_rate_controller`, `frame_rate_mouse_title` (mouse, title), `frame_rate_persist` (restart), `frame_rate_switch` (live switching, the game's state unchanged), `frame_rate_cli` (command line over the file), `frame_rate_invalid`, `frame_rate_display`, `frame_rate_k44_60`, `frame_rate_save`; docs/HIGH_FPS.md 21.7 |

Text: the PC labels are English only for now; the game's other languages (French, German, Italian, Spanish,
Japanese, Korean, Chinese) need the port's own translations and a check of the fonts' characters
(docs/KNOWN_ISSUES.md).
