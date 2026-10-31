# Original menu architecture (M6A)

How the game's own menus are built, how its Settings menus work, and how PC settings are added to them so that
they look, sound and navigate like the original. Sources: the game's shipped data (read from the archives into
`build/analysis/`, never changed or committed), the recompiled code's disassembly, runtime traces
(`--kuf_trace_menu`, `--kuf_trace_ui_objects`, src/runtime/menu_trace.cpp) and sandboxed runs with captures
(`tests/menu_arch/*.toml`). No leaked source.

Legend: **[V]** verified (trace, capture, byte comparison or a working change), **[S]** from static analysis
only, **[H]** hypothesis. Section 12 lists the evidence per conclusion.

## 1. Original menu architecture

- Menus are **native game code driving data-driven layouts**: the "BlueSide UI" toolkit. Each screen is a
  *scene* described in a layout file (`d:\data\Menu\Ui*.uitb`, 24 files in `mainres.pak`) with its controls
  (buttons, images, texts, list boxes) at fixed positions. Game code looks the controls up by numeric ID and
  implements all behaviour: which rows exist, what Left/Right does, what is saved. **[V]**
- **Lua does not build or run the menus.** The 120 shipped `.lua` scripts use the UI only for in-world dialogue
  choices (`lb.AddSelection`, `Ask`), trading screens and text lookups (`S(n)`). **[V]** (all scripts searched)
- A **menu object** (C++ class with a vtable) owns several scenes, binds their controls by ID into fixed fields
  and receives input as messages for the scene that has focus. **[V]**
- There are **two Settings menus**, implemented by different code: the title menu's SETTINGS
  (`SN_TITLE_OPTION`, 9 rows including Change Storage Device) and the in-game options menu's Settings
  (`SN_PROFILE_OPTION_CONFIG`, the same 8 option rows; opened with Back in play: Sleep, Change Regions, View
  Quests, Controls, Settings, Quit). The rows are **fixed in code**: each menu has an array of row buttons and a
  selected-row index with hard-coded bounds. **[V]**
- The original options live in the save package's `GlobalSetting` file and are written through the game's own
  save message when Settings is left after a change (section 7). **[V]**
- **The original menu system can be extended at run time** (section 8, demonstrated): controls added to a
  scene's layout in memory are created, drawn and destroyed by the game itself, and the original highlight,
  sound and toggle code can drive them. **[V]**

## 2. Functions, structures and layout resources

### 2.1 Resources (game data, read only)

| Resource | Content |
|---|---|
| `Menu\Ui*.uitb` | Layouts, one or more scenes per file (format: 2.2) |
| `Menu\BsUiControlID.h`, `BsUiClassID.txt` / `.cib` | Control IDs and names (`SN_` scene, `BT_` button, `LB_` list box, `TT_` text, `IM_` image): 69 scenes, 278 buttons, 16 list boxes, 403 texts, 2,986 images |
| `Menu\BsUiUVList.txt` / `.uvb`, `BsUiUVID.h` | Named rectangles (UVs) in the UI textures (`_TEX_UNIT_*` = `Interface\COD_*.dds`) |
| `FontAttr\<LANG>\BsUiFontAttr_<LANG>.txt` | 95 named font presets (`FA_BUTTON_L_SEL`, ...): markup strings |
| `Sox\COD_systemText_<LANG>.vsc` | Localized UI strings by number (8 languages, numbers up to 1910) |
| `Sound\SoundRes\GlobalCueInfo.txt` | Sound cues; UI sounds are the `11_Interface` bank (10) |

Control IDs are `0x0SSS T 0II`-shaped: scene number, then the type (0 scene, 1 button, 2 list box, 4 text,
5 image), then an index (e.g. `0x2701005` = `BT_PROFILE_OPTION_CONFIG_ON`). **[V]**

### 2.2 Layout file format ("BlueSide UI Export File v0.001")

Verified by a parser that reads all 24 files to their exact length, with every control ID known and every
scene's records matching its placement list (`src/runtime/ui_layout.cpp`, tests `ui.*`). Big-endian. **[V]**

| Part | Size | Fields |
|---|---|---|
| File header | 44 | magic, version `v0.001`, +40 scene count |
| Scene record (type 0) | 100 + 12 n | +4 ID, +8/+12 width/height, +84 alpha (255), +88 n = child count, +92/+96 two parameters the scene instance takes as defaults (e.g. 400 / 0), then n placements (control ID, x, y) |
| Control records | | exactly the scene's n children, in placement order, right after the placements |
| Button (1) | 136 | +4 ID, +8/+12 size, +16 flags, +20 text number (-1: set by code), +24/+28 text margins, +32/+36/+40 font presets selected/normal/greyed, +44 toggle flag, +48/+52 UVs of a toggle half selected/normal (`_UV_UI_BOX_SEL_CC` / `_UV_UI_BOX_CC`), +84 alpha, +100..+132 nine state images (three states x left/centre/right) |
| List box (2) | 180, or 308 if +172 = 1 | +8/+12 size, +16 flags, +24/+28 margins, +32..+40 row images, +92 visible rows, +136..+168 row template images, +172 extension block present |
| Text (4) | 92 | +8/+12 size, +16 flags, +20 text number (-1: set by code), +40 font preset, +84 alpha |
| Image (5) | 380 | +8/+12 size, +16 flags, +44 visible, +56 UV ID, +68/+80 orientation (mirrored left arrow: 2/1), +84 alpha, +356/+364 small flags, +376 owning button (its state images), +92..+344 exporter leftovers (uninitialised memory) |

Positions are the placement's x/y in a 1280 x 720 space, absolute (not relative to the scene). Drawing order
is the record order. **[V]** (the proof-of-concept row draws where its placement says, behind the highlight
when inserted before it)

### 2.3 Loading and instantiation

| Function | Role |
|---|---|
| `sub_82617998` | Picks the layout file by menu kind and loads it through the UI manager (global at `0x82AC0000-18636`) |
| `sub_826221C8` -> `sub_82621D58(loader, path)` | Reads the whole file (`sub_82190C60`), checks the magic, reads the scenes; scenes and control *definitions* go into an ID map at `loader+4` |
| `sub_82621470` | One scene: the 100-byte header, a 120-byte scene object (`sub_8261E6F8`), the placements into a vector at `scene+80` (`sub_826211F8`) |
| `sub_826211F8` | The scene's controls: reads each record's type and dispatches to the readers (button `sub_8261FD58`, list box `sub_8261FE68`, text `sub_82620308`, image `sub_82620450`; types 3 and 6 exist in code but not in the data) |
| `sub_82621E90` / `sub_8261D988` | Creates a scene *instance* with live controls from the definitions (scene header +92/+96 as defaults) |
| `sub_82624570(node, id)` | Control lookup in a scene instance (`+8` ID, `+40` children); 1,195 call sites, every menu binds its controls with it |

The definitions are freed after instantiation (their memory is reused at once). Live controls are created when
the menu opens its scenes (the options menu creates all five of its scenes when it opens: message 0x64 per
scene) and destroyed when it closes (message 0x65; the objects' vtables fall back to the base class and their
fields are cleared). Reopening Settings inside the open options menu reuses the same instances. **[V]**

### 2.4 Live control object (instances)

| Offset | Content |
|---|---|
| +0 | vtable: button `0x8203AD08`, image `0x8203ADB8`, text `0x8203B278`; base `0x8203ABF0` after destruction |
| +8, +12 | ID, type |
| +16, +20, +24, +28 | x, y, width, height (1280 x 720 space); vtable slot 5 sets them (`SetRect`) |
| +44, +48 | owner (scene), shared UI state |
| +56 | state: 2 / 3 as set by `sub_82623850(control, 1, 0 / 1)` (the selected row's own button is hidden; the highlight shows its label) |
| +72 | toggle half: selected when it points at the highlight button, 0 otherwise |
| +80, +84 | text number, prepared string (pointer) |
| +100..+140 | the button's state image instances |

**[V]** (`--kuf_trace_ui_objects` diffs across inputs; the proof of concept writes +72 and works)

### 2.5 The Settings menu objects

**In-game options menu** (`SN_PROFILE_OPTION*`): event handler `sub_82663FA0(menu, scene, event, 0x44C)` for all
its scenes, called by the toolkit's dispatcher (`lr 82622244` for input, `82622348` for scene events). Fields:
`+44` the Settings scene instance, `+60` the highlight button (`BT_PROFILE_OPTION_CONFIG_ON`), `+64..+92` the 8
row buttons, `+128..+148` the toggle halves (camera, blood, vibration), `+152..+160` skin text and arrows,
`+164` selected row (0-7), `+172..+188` the values (volumes, brightness as floats, skin index). Functions:
`sub_82661CD0` show the selected row (all rows normal, highlight moved to the row, the row's label copied into
the highlight, the row hidden), `sub_82663328(menu, right)` change the selected row's value,
`sub_82661DA0` / `sub_82661E80` update a slider / the skin selector, `sub_82662E98` bind the controls,
`sub_82663E40` open. **[V]**

**Title menu** (vtable `0x8203E658`, `SN_TITLE_*`): scenes registered in `sub_8267A358` (`+44` SN_TITLE_OPTION),
input `sub_8267A5E8` -> `sub_826791D0` (main list) / `sub_826799A0` (Settings, Language), show values
`sub_82677158`, bind `sub_826785B8`; shared slider/toggle helpers `sub_82676AB8`, `sub_82676C38`, `sub_82676B98`.
**[V]** (trace) The in-game handler has been read in detail; the title one only traced.

### 2.6 Messages, sounds, text

- Game actions are posted with `sub_825D5560(message, ...)`: `0x41128` writes GlobalSetting (the options
  save, also the second step of every game save, docs/SAVES.md). **[V]**
- `sub_8261B488(menu, cue)` plays a UI cue (`sub_826E5140(sound manager, cue, 10)`, bank `11_Interface`). **[V]**
- `sub_826CDBF0(text manager, number, args...)` returns the localized string for a text number (narrow,
  NUL-terminated, with markup, e.g. `@(offsety=-2)Vibration`), formatted with the arguments.
  `sub_82624CB8(control, string, 0)` sets a control's text. **[V]**

## 3. Existing UI widgets

There are no generic widget classes: every "widget" is a convention of plain controls that the menu code
drives. **[V]**

| Widget | Controls (layout) | Input | Value update and drawing | Created / destroyed |
|---|---|---|---|---|
| Row | label button (285 x 32, fonts `FA_BUTTON_L_*`) + frame images `IM_.._L/C/R` + its state images `_OFF_L/C/R` | Up/Down move the selection (cue 4), bounds in code | selection: the highlight button `BT_.._ON` (own "ON" frame images) is moved onto the row and shows the row's label; the row's own button is hidden | with the scene instance |
| Slider (volumes, brightness) | `IM_.._GAUGE` (404 x 14), `_BAR` (400 x 10), `_TIP` (2 x 10) | Left/Right: `sub_82663328` steps the float (volume 0-100 by 10, brightness 0-1 by 0.1) | bar and tip placed by `sub_82661DA0` (bar start + width x value) | same |
| Toggle (Camera, Blood, Vibration) | two small buttons `_ON` / `_OFF` (200 x 22, `FA_BUTTON_SMALL_C_*`, UV pair) | Left: first half, Right: second | the selected half's +72 = highlight | same |
| Selector (Skin) | text `TT_.._SKIN_NAME` (text set by code) + arrow images `_ARROW_L/R` | Left/Right step an index (bounds from the game) | `sub_82661E80`: the number formatted into the text, arrows shown only when a step is possible | same |
| Action (Change Storage Device, title only) | a wide label button | Select | opens a message-box scene | same |
| Button help | text `TT_.._BTNINFO` with icon markup (`@(Dpad_Small)Move/Adjust  @(B)Back`) | | static | same |
| List (options list, language) | list box or buttons | Up/Down/Select | | same |

Capabilities and limits (verified): the rows of a Settings scene are a fixed array of 8 (in game) / 9
(title) in the menu object; the selected index is bounded in code; the panel is sized for exactly these rows.
Adding entries therefore needs (a) controls in the layout and (b) code for their input: the proof of concept
provides both (section 8). Numeric values exist only as sliders (no numeric text widget in Settings, but the
skin selector shows a number through a text control).

## 4. Input and navigation

- Input reaches a menu as messages: event struct `+4` message (1100 = 0x44C input, 0x64 scene created, 0x65
  destroyed), `+12` input code: 0 Select (A), 1 Back (B), 6 Up, 7 Down, 8 Left, 9 Right. **[V]**
- The codes come from the game's input layer (it polls `XamInputGetState` once per frame, edge-detects
  buttons, sticks as directions), so controller D-pad and left stick, and the port's keyboard bindings (which
  produce a virtual pad, docs/INPUT.md) all arrive identically. **[V]** (the proof of concept works the same
  with the keyboard and an SDL virtual pad)
- In-game Settings: Up/Down stop at the first/last row (no wrap); Down does not move onto a disabled row
  (the handler checks the next row button's state and undoes the step); Left/Right change
  the value at once (no cue); Back posts the save message only if a value changed, plays cue 0xF and returns
  to the options list (which keeps the Settings row selected). Reopening keeps the last selected row. **[V]**
- Cues: 0x11 options menu opens, 4 cursor move, 0xB select, 0xF back. **[V]** (call arguments); their names
  in `GlobalCueInfo.txt` are not mapped (the bank's file order does not match the indices). **[H]**
- Mouse: the original menus have no pointer support; the port's mouse buttons act as A/B (M4). **[V]**

## 5. Rendering and visual presentation

- Fonts: bitmap fonts `Interface\Font\Font_0_<ENG|EURO|JPN|KOR|TCH>.36t` (+ glyph tables) and `Font_Special`
  (button icons). Style by markup presets, e.g. `FA_BUTTON_L_SEL`; text 227,226,214 (selected / normal differ
  in alpha), greyed 74,72,51. **[V]**
- Frames, gauges and arrows: UV rectangles of the UI textures (e.g. `_UV_UI_TEXTLINE_OFF_L/C/R` row frames,
  `_UV_UI_TEXTLINE_ON_*` highlight, `_UV_UI_BOX_SEL_CC` / `_UV_UI_BOX_CC` toggle halves). **[V]**
- Geometry (in-game Settings): panel 428..1200 x 115..573 (nine-slice of `_UV_UI_S_*`), rows 46 px apart
  (label at x 461, value area from x 755), button help at y 517. Title SETTINGS: full screen, rows from y 135,
  Change Storage Device at 548, help at 634. **[V]**
- Animation: scenes slide in/out (scene parameters; the options list slides back in after Settings); the
  highlight's label pulses. **[V]** (captures)
- All layout coordinates are in a fixed 1280 x 720 space with 16:9 assumed; how this space is mapped to other
  resolutions and aspect ratios is M6C work. **[S]**

## 6. Localization and text handling

- Every label is a number into the per-language tables (`COD_systemText_<LANG>.vsc`: entries of number,
  length, string, optional key), selected by the game language. Strings carry their own markup
  (`@(offsety=-2)SFX Volume`, `@(scale=0.8,0.8)@(offsety=-4)Normal`). **[V]**
- Settings labels: 1308 SFX Volume, 1309 Music Volume, 1310 Voice Volume, 1311 Brightness, 1312 Skin, 1693
  Camera (1692 Normal, 1691 Inverted), 1690 Blood, 1882 Vibration; options list 1300-1302. **[V]**
- New labels: the text lookup can return the port's own strings for numbers the game does not use (the proof
  of concept uses 0xFF00). The port needs its own table with the 8 languages (written by the project, not
  copied), using only characters the game's fonts contain. **[V]** (mechanism), **[H]** (font coverage for all
  languages not yet checked).

## 7. Settings persistence

### 7.1 The original options (GlobalSetting)

`GlobalSetting` (231 bytes) in the save package next to the character files (docs/SAVES.md); written by the
options save message, only when Settings is left after a change. Without a storage device the options live
only in memory. Offsets verified by controlled changes (one or two values per run, compared with the fixture
and with a run that opened Settings without a change, which leaves the file identical): **[V]**

| Offset | Content | Verified by |
|---|---|---|
| 20 | Camera: 0 Normal, 1 Inverted (u8) | title SETTINGS |
| 208 | SFX volume (big-endian float, 0-100, steps of 10) | title SETTINGS |
| 212 | Music volume (float; 90 by default) | default value |
| 216 | Voice volume (float) | default value |
| 224 | Skin (u8 index) | in-game Settings |
| 225 | Blood: 0 Off, nonzero On (u8; fresh profiles hold varying nonzero values: the default is not normalised) | in-game Settings, title runs |
| 226 | Brightness (big-endian float at an unaligned offset, 0.5 by default, steps of 0.1) | in-game Settings |
| 230 | Vibration: 1 On, 0 Off (u8) | in-game Settings |

### 7.2 PC settings: a separate file (design)

PC-specific values are never written into GlobalSetting (its layout is the game's, saves move between
installations, and the console never knew these values).

- **File**: `pc_settings.toml` in the port's user data directory (the directory that holds `user/` data and
  the shader cache: the SDK's user folder, on Linux `$XDG_DATA_HOME/<app>/` or `~/.local/share/<app>/`,
  overridable by `--user_data_root`), outside the
  save packages and the game folder. One table per area: `[display]`, `[graphics]`, `[camera]`, `[controls]`,
  `[framerate]`; `version = 1` at the top.
- **Defaults** reproduce the original console behaviour; a missing file means all defaults (nothing is
  written until the player changes something).
- **Loading**: every value is type- and range-checked; an invalid value is replaced by its default and
  logged; a file that cannot be parsed is renamed to `pc_settings.toml.bad-<time>` and defaults are used. A bad
  file never stops the game and never touches saves.
- **Unknown keys and tables** (from a newer version, or added by hand) are kept as read and written back
  unchanged; known keys are rewritten in place, so comments and order are preserved where the TOML library
  allows it (toml++, already part of the SDK, round-trips tables and values but not comments; the file's
  header says so).
- **Writing**: atomically, as the port already does for saves (temporary file in the same directory, `fsync`,
  rename over the old file, `fsync` of the directory); the previous version kept as `pc_settings.toml.bak`.
  Written when the player leaves Settings after a PC value changed (the moment the game saves its own
  options), not on every Left/Right.
- **Applying**: each value is applied live where possible (camera, sensitivity, vsync, frame limit) or on the
  next start (resolution/display mode if the renderer needs it; the menu says so in its help text).
- **Command-line options** (`--kuf_...`) stay for tests and override the file for that run only; they are not
  needed for ordinary settings.
- Settings that are not implemented are not shown and not written.

## 8. Recommended extension mechanism

### 8.1 Options evaluated

| Method | How | Reliability | Maintainability | Compatibility | Complexity |
|---|---|---|---|---|---|
| 1. Edit menu data or scripts on disk | change `.uitb`/text tables in the archives | low: modifies the player's game files | poor (binary, exporter leftovers) | one game version | high |
| 2. Extend the layout **in memory** | when the game reads a layout file, give it an extended copy (`ui_layout.cpp`); the game creates, draws and destroys the new controls itself | high: the game's own loader, controls and renderer; the file on disk is untouched | good: a small, tested transform per scene | per layout version (the transform checks the structure and leaves an unexpected file unchanged) | medium |
| 3. Isolated hooks in the menu code | `REX_HOOK_RAW` on the Settings event handler: handle input for the new rows, call the original for everything else; reuse the original highlight, toggle and sound code | high: runs only in the Settings scenes, original rows untouched | good: one module | per game version (addresses in code) | medium |
| 4. Own drawing (overlay, ImGui, separate window) | | excluded by the M6 plan | | | |

**Recommendation: 2 + 3, as demonstrated by the proof of concept.** New rows are cloned from the scene's own
rows (same frames, fonts, toggles, sliders), placed in the layout in memory, and driven by a hook that reuses
the original highlight code (`sub_82661CD0`), cursor cue and toggle display. Values are applied by the port and
stored in `pc_settings.toml`. No separate GUI framework, overlay or launcher.

### 8.2 Proof of concept (M6A) and the implementation (M6B)

M6A demonstrated the mechanism with one toggle row in the in-game Settings (`--kuf_pc_settings_menu_poc`, off by
default; verified with keyboard and controller, original rows and GlobalSetting unaffected). M6B replaced it with
the real PC Settings (`--kuf_pc_settings_menu`, on by default; docs/SETTINGS.md), in **both** Settings menus:

- **Layout** (`ui_layout.cpp`): in game, an action row cloned from Vibration's frame and label (not its toggle)
  one row lower, the panel grown by a row, its bottom and the button help moved down; in the title SETTINGS,
  Change Storage Device's frame and label cloned one row lower (the full-screen panel already reaches there).
  New IDs `0x2701080`/`0x2705080..85` and `0x4101080`/`0x4105080..85`; label text number 0xFF00 ("PC
  Settings"). Unit tests `ui.*`.
- **One implementation, two menus** (`pc_settings_menu.cpp`, a `Spec` per menu): in-game event handler
  `sub_82663FA0` (menu: scene +44, rows +64, row +164, highlight +60, toggles +128.., selector +152/156/160,
  slider values +176/172/180/184; `sub_82661CD0`, `sub_82661DA0`, `sub_82661E80`); title SETTINGS input
  `sub_82678280(menu, event)` (rows +160, row +268, highlight +156, toggles +228.., selector +252/256/260,
  slider values +280/276/284/288, help text +264 set by the highlight code; `sub_82676C38`, `sub_82676AB8`,
  `sub_82676B98`; row 8 = Change Storage Device, selectable only with a storage device: its state bit 0x10).
- **Navigation**: Down on the last row the cursor can reach (in the title: Change Storage Device if available)
  selects PC Settings (the original highlight code with its button in a row slot, cue 4); Select opens the PC
  page (cue 0xB); Back leaves through the original code.
- **PC page**: the eight rows relabelled (set-text, the controls keep their own copy; the original labels kept
  and restored), values drawn with the menu's own slider/selector/toggle code; PC Settings becomes Restore
  Defaults; in the title, Change Storage Device is shown unavailable (state 16) while the page is open. All
  input on the page is handled by the port; Back restores the game's labels and values (sliders from the menu's
  value fields, the selector by its own function, the toggle halves as saved), selects PC Settings, writes
  `pc_settings.toml` if changed, cue 0xF.
- **Verified** (sandboxed, tests/settings): keyboard, controller (D-pad, left stick, A/B), every widget, the
  title route past an unavailable Change Storage Device, help text with Select on the action rows, values
  kept across reopening and across a restart, Restore Defaults; the game's own saves (GlobalSetting, character
  files) unchanged by PC changes (`expect_saves_unchanged`).

## 9. Technical limitations

- **Space**: the in-game panel holds its 8 rows; one more row fits by growing the panel (to y 619 of 720). More
  rows are pages: the PC page reuses the eight rows in place (section 8.2); more categories need a category
  list first (section 10).
- **Fixed rows in code**: the original handlers bound the selection to their rows, so every new row needs
  handling in the hook (the PoC pattern). The two Settings menus have separate code (title: `sub_826799A0`,
  in game: `sub_82663FA0`), so each needs its own integration.
- **Addresses** (functions, object offsets) are specific to the v1 executable; a title update needs them
  re-verified (version key, docs/ARCHITECTURE.md). The layout transform refuses unexpected layouts.
- **Text**: new labels need the port's own translations (the PC labels are English only so far); the English
  font has only ASCII (no degree sign: "85 Degrees"); glyph coverage for the other languages is to be checked
  (JPN/KOR/TCH fonts are separate).
- **Value widgets**: only sliders, toggles and a numeric selector exist; a list-like choice (e.g. resolutions)
  is a selector (number or text in a text control, arrows), which the skin row shows is possible.
- **1280 x 720 layout space**: scaling and aspect handling come from the renderer (M6C).
- **Cue names**: indices known, names not mapped.

## 10. PC settings menu structure (M6B: implemented for the camera)

In both Settings menus, one more row at the bottom, **PC Settings** (an action row), opens the PC page in place
(section 8.2). With eight value rows per page (four sliders, a selector, three toggles, as the game's own page)
the camera settings fill it exactly:

| Row | Widget | Milestone |
|---|---|---|
| Mouse Horizontal / Mouse Vertical (sensitivity) | sliders | M6B (done) |
| Stick Sensitivity | slider | M6B (done) |
| Stick Dead Zone | slider | M6B (done) |
| Vertical Range | selector | M6B (done) |
| Mouse Y-Axis (Normal / Inverted) | toggle | M6B (done); the controller follows the game's own Camera row |
| Camera Recentering (On / Off) | toggle | M6B (done) |
| Modern Camera (On / Off) | toggle | M6B (done) |
| Restore Defaults | action (the PC Settings row) | M6B (done) |

**M7 (feature 3): the category list.** PC Settings now opens a category page (Controls, Camera, Display, Frame Rate,
Restore Defaults); each category is one page in the same eight rows (sliders where the game's slots have them, other
values in the row labels with the widgets hidden; docs/SETTINGS.md). The scrolling pages below were replaced by it.

Before M7, the later rows were further pages reached by scrolling down (Up scrolls back), not a category list:
Distance and Field of View (M6B), the display page (M6C) and the frame-rate page with its advanced page (M6D D5:
FPS, Interpolation; World Objects, Animation, Effects; docs/SETTINGS.md, docs/HIGH_FPS.md section 21). These later
pages show their values in the row labels (the rows' widgets hidden) and step them with Left / Right; the frame-rate
rows also step with Select, so the mouse can set them. The plan as first written:

Later milestones need more rows than one page holds. Planned: the PC Settings row opens a short category list
(Camera, Display, Performance, Graphics) built the same way, each category a page of up to eight rows: Display
(resolution, display mode, refresh rate, aspect: M6C), field of view (M6D), frame-rate target and VSync (M6E),
anti-aliasing and filtering (M6F). Selectors with text (as Vertical Range shows) cover lists such as
resolutions.

## 11. Camera settings integration (M6B: done)

The camera rows apply at once (pc_settings.h `Update`; the camera reads the settings every frame) and are written
to `pc_settings.toml` on Back. What they do: docs/CAMERA.md section 4; the file: docs/SETTINGS.md. Invert Y for
the controller stays the original Camera row, so console saves and the original menu agree.

## 12. Evidence

| Conclusion | Evidence |
|---|---|
| Layout format (2.2) | `ui_layout.cpp` parses all 24 files exactly; `ui.*` tests (synthetic layouts); dumps of `SN_PROFILE_OPTION_CONFIG` and `SN_TITLE_OPTION` agree with the screen (positions, labels, fonts) |
| Loader and readers (2.3) | disassembly of `sub_82621D58`, `sub_82621470`, `sub_826211F8`, `sub_82621588`; reader hooks (`--kuf_trace_ui_objects`) |
| Live objects, lifecycle (2.3, 2.4) | `--kuf_trace_ui_objects` run `arch-ingame_settings_{a,b}`: per-input diffs (highlight y follows the rows, +56 states, +72 toggles), objects reset on close |
| Settings handler, codes, bounds (2.5, 4) | disassembly of `sub_82663FA0`, `sub_82661CD0`, `sub_82663328`; event struct logged per input |
| Cues, save on Back (4, 7.1) | traces `arch-ingame_settings_*` and `arch-title_settings_*` (helper calls) |
| GlobalSetting offsets (7.1) | byte comparisons: `arch-title_settings_{trace,baseline}`, `arch-ingame_settings_{baseline,a,b}` against the fixture |
| Text lookup (2.6, 6) | logged result of text 1882; the PoC label |
| Runtime extension works (8.2) | runs `poc-pc_settings_poc_keyboard`, `poc-pc_settings_poc_controller` (captures at each step), `pocorig-ingame_settings_{a,b}` (original rows unchanged), GlobalSetting comparisons |
| Lua not involved (1) | all 120 scripts searched |

Scenarios: `tests/menu_arch/` (not registered with ctest: they need the local campaign fixture or are traces).

## 13. Focus, scenes and list boxes (M7: the menu pointer)

Found for the mouse pointer (src/runtime/menu_pointer.cpp, `--kuf_trace_menu_pointer`; runs `m7-ptr_trace`,
`m7b-ptr_trace`, `m7-listbox`, `m7-hover*`). **[V]** unless marked.

| What | Where |
|---|---|
| The focused control (receives input) | global `0x82ABBB90`; compared with each control by the shared vtable slot 7 (`sub_82624598`) |
| Input path | a control's handler (vtable slot 1: button `sub_826259C8`, list box at `0x82627e0c`) acts first (a list box moves its selection), then forwards to the scene: `sub_826221D8(manager, control + 48, control + 4, 0x44C, 0, code)`; the manager is the global at `0x82AC0000 - 18636` |
| A control's scene | control `+48`: a scene record, `+0` the scene ID (e.g. `0x03900000`), `+8` the scene's root control node, then 16-byte entries (control ID, scene record, control, root) |
| Children of a node | a list at node `+32`: head node `+36`, count `+40`; list node `+0` next, `+8` the child |
| Button vtable `0x8203AD08` | slot 1 event handler (scene events 0x64 create / 0x65 destroy, input), slot 5 SetRect, slot 7 focus/state query; slots 6 and 10 were not called in menus (drawing is elsewhere) |
| State `+56` | bit 0x1 shown, 0x10 unavailable (greyed: LEADERBOARD 0x93), 0x80 seen on the title list's rows **[S]** meaning unknown |
| Selection | the title list moves the focus with the selection (the focused control is the selected row); the Settings menus and the character slots keep the focus on a highlight button moved onto the selected row, whose own button is hidden (state without 0x1) |
| List box (type 2) | `+376` row width, `+380` row height (Language list 27), `+384` visible rows, `+392`/`+396` the item vector, `+404` the first visible row, `+408` the selected index; `sub_82626EE8(list, index)` sets the selection and scrolls by one row to keep it visible |

Scenes seen: `0x039` title list (NEW GAME, CONTINUE, LEADERBOARD greyed, SETTINGS, LANGUAGE, PROLOGUE, CREDITS:
500 x 32 at x 390 from y 482, every 28 px), `0x041` title SETTINGS (value rows 285 x 32 at x 176 from y 135, every
46; Change Storage Device and PC Settings 905 wide), `0x02E` character slots (648 x 130), `0x025` message box (one
button 434 x 68), `0x02F` / `0x029` in-game dialogs, `0x013` the in-game options list box (127,47 380 x 216),
`0x03D` the language list box, `0x003` the status menu (tabs 50 x 42 at y 208, rows 540 x 34, small buttons in a
grid), `0x023` a 40-button screen (1098 x 30 rows).

## 14. Findings of the M7 UI polish (2026-10-31) [V]

`--kuf_trace_scene_dump` logs, when the focused scene, row or label changes, every control of the scene (type, ID,
rectangle, state, vtable, text) and the focused control's first 64 words.

| Finding | Detail |
|---|---|
| Selection arrow | the toolkit draws the arrow beside the focused button unless its state has bit 0x80: the title list's seven rows carry it (0x83, greyed 0x93), the port's added rows did not, so only those showed the arrow. `--kuf_title_arrow` (default on) clears the bit while the list is focused (src/runtime/title_menu.cpp) |
| Highlight pulse | the selection highlight's label pulses (bright to dim and back); captures taken at fixed ticks see either phase: compare brightness over several frames, not one |
| Scene objects | a scene *instance* (vtable `0x8203BD28`): +4 its record, +8 its ID; a control's +48 is the scene *record* (+0 the ID). Menu objects keep their scene instances among their first fields (the in-game options menu: +0x20..+0x30) |
| Controls page | scene `0x028` of UiProfileOption.uitb: the controller picture `0x280500B`, 13 icon texts (button-name texts 1000-1012: A, B, X, Y, LB, RB, Start, Back, LT, RT, left stick, right stick, D-pad) at x 702, 50 wide, the actions at x 750; it never takes the focus (the options list keeps it) |
| Game type screen | scene `0x02F`: each row twice (a shown button and a hidden twin; the twins are the focusable ones), the co-op rows unavailable (0x13), Difficulty at x 548 (set in from the list's x 528). On entry the game shows "You cannot play co-op ... Xbox LIVE Gold" (a notice box) |
| Message boxes | message `0x42205` opens a question (data: text[1024], +1024 owner, +1028 kind 1, +1032 / +1036 the answers for A / B), `0x42208` a one-button notice; the box (sub_826601E8) stores the answer at box +148 and posts `0x42207` (close) and `0x42206` (answered, data = the box, owner at +152). One button: A answers with +1032, B with +1036 (kind 1) |
| Control lookup side effect | looking up the title SETTINGS help text (`sub_82624570(scene, 0x4104002)`) after the highlight code made the highlight's label dim: the description layout uses the help line's layout position instead |
| Text controls | vtable slot 5 is the toolkit's base SetRect `sub_82624018(control, x, y, w, h)` (buttons' `sub_82625AC8` calls it first) |


## 15. Findings of the M7 UI follow-up (2026-10-31; from the recompiled code, not yet seen in the game)

- **Selection arrow:** `sub_82612DE0(ui manager)` (called once per update from `sub_82619160`) reads the focused
  control (0x82ABBB90); if it is shown, has no state bit 0x80 and is a button or a list box, the arrow is placed at
  its left (x - 15) and shown (`sub_8260F878` / `sub_8260FFF0`, the control kept at manager+1592), otherwise hidden
  (`sub_8260F9A0`). `sub_82622400` is the toolkit's SetFocus (messages 106 lose / 105 gain to the controls).
- **Co-op gate:** `sub_82669770` (no arguments) checks privilege 254 with `sub_827EACE0` (XUserCheckPrivilege
  wrapper) and then `sub_827E9128(pad)` (a sign-in bit); each failure posts 0x42205 (kind 0) with text 1687 / 1730
  through `sub_825D5A88` and returns 0. Callers (`sub_82669D28`, `sub_82669DD8`, `sub_8266B018`, `sub_8266C4A0`,
  the action paths `sub_8266A028`, `sub_8266A3F8`, `sub_8266BB60`) grey the four online co-op rows on 0 and abort
  co-op actions; the 5-tick poll `sub_82668698` repeats the test without a post. The rows are texts 840 / 841 / 842 /
  848 (Host Private, Host Public, Quick Match, Co-op Lobby); no split-screen or system-link path was found.
- **Status menu (inventory):** handler `sub_82673B08(menu, scene, event)`; scenes at +32 (0x003 equipment), +36
  (0x005), +40 (0x008), +44 (0x009 resistances), +48 (0x010, row 0), +52 (0x03E); +196 the tab (0-2), +376 the
  selected row (+380 the last; row 0 only if byte +660), +216 + 4 row the rows' buttons (rows 1-4: slots 0x301007-0A),
  +236..+256 the tab buttons 0x301001-06, +372 the footer 0x304006. Codes on scene 0x003: Up / Down rows, Left / Right
  (and 14 / 15) the tab, 0 Select by the row's action (+88 of its button: type, parameter; the item list or a
  notice), 2 X Auto-equip (asks first with a question box), 3 Y Details, 4 LB Resistances, 5 RB Inventory, 1 / 10
  Back. Item list ("Change Item", scene 0x004): handler `sub_82653058`, list box 0x402001 (paging at its ends: +464 the
  page), 0 Equip, 2 Destroy (asks first), 3 Details, 1 Back; footer 0x404001.
- Every button reaches these handlers as a 0x44C event code; the button's input handler `sub_826259C8` forwards any
  code to `sub_826221D8`, so the pointer's `Dispatch(code)` is the game's own input path for each of them.

## 16. Shared causes found in the M7 UI follow-up (2026-10-31)

Several reported problems came from the same few shared mechanisms; each is now fixed where it is shared:

- **Acting a tick late on state the game draws in the same tick.** The arrow (the "no arrow" bit set at the next
  tick's start), the storage boxes (answered at the next tick's start) and the highlight on a page change (placed
  before the rows moved) were all drawn once in their stale state. Decisions now happen where the game makes its own:
  the arrow in its update (sub_82612DE0), the storage question in the UI update before the draw (the same hook), the
  rows placed before the highlight is set; the notice is not posted at all.
- **Positions from the layout's slots instead of the shown order.** The layouts' rows are not evenly spaced; pages now
  place row i from the first row by one row spacing (pc_settings_menu.cpp PlaceRows), Restore Defaults below them.
- **A pointer fallback that pressed A.** A click not on a row was turned into A ("dialogs without buttons"), which
  confirmed modal dialogs from outside clicks; and a click left pending when a menu closed became A in gameplay. Both
  removed (A only where nothing has the focus; the leftover click dropped outside the start prompt); the level-up
  dialog takes all clicks inside its scene.
- **Key names in icon-sized boxes.** The prompt rewrite (one-icon texts become "[Key]") is right for help bars and
  hints, wrong for boxes sized for one glyph (the Controls page, the equipment screen's slot icons): those screens
  replace or empty them per device (controls_screen.cpp, menu_mouse.cpp StatusPrompts).
- **Widgets the pointer did not know.** Only buttons of the focused control's group were rows; the selector's arrows
  and the toggles' halves are now hit-tested from their own controls (PointerWidget).
