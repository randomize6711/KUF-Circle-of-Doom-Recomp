# Input (M4)

Keyboard, mouse and controllers for the original menus, translated into what the Xbox 360 game expects: an
Xbox 360 gamepad for guest user 0. No game code is patched; the menus, their sounds and animations are the
game's own.

## How the game reads input
- Only through `XamInputGetState` (plus `XamInputGetCapabilities` and `XamInputSetState` for vibration).
  It never calls `XamInputGetKeystroke`.
- Once per frame, from one place: `sub_825EAD58` polls users 0–3, then edge-detects 14 buttons (in the order
  A, B, X, Y, LB, RB, D-pad up/down/left/right, Start, Back, LS, RS; table at `0x82034EB0`) and both triggers
  (threshold 30). A press is a released→pressed change between two polls; the active screen's handler gets a
  press and a release event. Holding the D-pad auto-repeats in the game's menus (the game's own behaviour).
- "Press START": the input object (`r3` of `sub_825EAD58`) has a "waiting for START" byte at +48. It is set
  on the title screen and cleared by the first START press, whose user index is stored at +44. The title
  screen accepts **only Start**; A, and A pressed in the same frame as Start, do not pass it.

So the right place to translate is the XAM input layer, not individual menus.

## Architecture
```
 SDL window events ──> KufInputDriver ──> KeyboardPad (pure)  ─┐
 (keys, mouse, focus)    (src/input)       bindings, latching   │  InputSystem (SDK): merges devices per guest
                                                                 ├─ user (buttons OR, sticks larger magnitude)
 SDL gamepads ───────> SDLInputDriver (SDK, patch 0003) ────────┘  ──> XamInputGetState ──> sub_825EAD58
                                                       NOP fallback
 game_input_hooks.cpp: reads the "waiting for START" byte each frame (read-only) ──> KeyboardPad accept key
```
- `src/input/input_factory.cpp` replaces the SDK's default input system (`OnPreSetup`): SDL controllers,
  our keyboard/mouse driver instead of the SDK's MnK driver (shooter defaults, mouse only as a camera
  stick), and the SDK's NOP fallback. Assignment: controller N → guest user N; keyboard/mouse → user 0.
- `KeyboardPad` turns host events into the gamepad *state* the game polls:
  - held key = held button; OS key repeat is ignored (one press per physical press);
  - a tap shorter than a frame is latched for one poll, so it is never lost;
  - several keys on one control: released when all are up;
  - window focus loss releases everything (no stuck keys); key-ups are accepted even without focus;
  - one D-pad press per mouse-wheel detent (fractional touchpad scrolling accumulates), with a released
    poll between presses so each is its own edge;
  - **accept** (Enter, left click): Start while the game waits for START, A everywhere else. The meaning
    is fixed when the press is first seen and kept until release, so holding Enter across the screen
    change cannot turn into a second action (a delayed A answered the storage prompt in an experiment).
- While a runtime overlay (F3/F4/F7/Backtick) has the input, the game sees a neutral pad.

## Contexts
The game itself tells which of three situations it is in (game_input_hooks.cpp, read-only):
- **Press START**: the input object's "waiting for START" byte (+48) is set (title screen).
- **Gameplay**: user 0's input-target handle (+8) is valid, i.e. the player character has control. This
  includes the tutorial's hint boxes after their ~30-frame opening animation.
- **Menu**: everything else: main menus, dialogs and the in-game status/options menus (pause). In co-op the world
  keeps running under the in-game menus and the target stays valid, so a menu also counts when the UI's focused
  control (0x82ABBB90) is a live control of an in-game menu scene (status, items, options, quests, map, Settings and
  the PC pages, Controls, message boxes, trade, fusion, abilities, level-up points, game over, act clear;
  `src/input/game_context.cpp`). Gameplay overlays that take the focus during play (the letterbox 0x023, gauges,
  minimap, remote players' panels) do not. `--kuf_menu_focus_context=false` turns this off (the target alone).

Backspace is not a menu control in any context but gameplay (`Bindings::Set` drops it from menu and Press START
bindings, also when a setting names it). **Text fields** (the multiplayer menus' lobby name, password, invite code):
while one is edited the driver's text entry (`src/input/text_entry.cpp`) turns the platform's text input on (SDL
text events: the only source of characters, so nothing is typed twice; received by a listener above the SDK's ImGui
drawer, window input layer 64, which marks every character event handled even with no overlay open; printable ASCII
and Latin-1, so Å Ä Ö and the like, kept as UTF-8, drawn as Latin-1) and takes every key-down (Ctrl+C copies, never a
password): Backspace, Delete,
Left/Right, Home/End, Ctrl+A, Ctrl+V (one line of the clipboard), Enter (done), Escape (cancel); no key reaches the
game or the menu meanwhile, and the pointer's hover does not move the selection (a click ends the edit and acts on
the row under it).

Keys and mouse buttons take their meaning from the context **when pressed** and keep it until released.
Mouse look is active only in gameplay; in menus the pointer is free. A context change starts the camera from rest
(pending mouse movement dropped) and drops queued wheel steps. Losing the window's focus releases the pointer and
every key; regaining it captures the pointer again only in gameplay.

## Keyboard (defaults)
| Gamepad | Menus | Gameplay (the game's action) |
|---|---|---|
| A | **Enter**, Space, left click | **Left click** (use A weapon), Enter |
| B | **Escape**, right click (**never Backspace**: it only deletes text in the menus' text fields) | **Space** (use B ability: Dash) |
| X | X | **Right click** (use X weapon), X |
| Y | Y | **E** (pick up items, detection), Y |
| Start | F1, Pause; **Enter / left click at "Press START"** | **Tab** (status menu: equipment, inventory), F1, Pause |
| Back | Tab | **Escape** (options menu: pause, Sleep, Quests, Controls, Settings, Quit) |
| LB / RB | Q, Page Up / E, Page Down | Q, 1 / R, 2 (use item; hold: inventory) |
| LT / RT | Z / C | Z, mouse button 4 (aiming view) / F, C, mouse button 5 (RT ability) |
| D-pad | **Arrow keys** | arrow keys (co-op ally targeting) |
| Left stick | W A S D | **W A S D** (move) |
| Right stick | I J K L | **mouse**, I J K L (camera) |
| Stick clicks | Shift / middle click | Shift / **middle click**, V (centre camera) |

In the status menu (a menu context) RB is Page Down or E: Inventory; LB is Page Up or Q: Resistances. On the
tutorial's hint boxes (a menu context since M7's a359b0e: in-game menus count as menus while the world runs) Enter or
Space confirms and Escape goes back; the game then offers "End tutorial?" (Enter: yes).

**Rebinding in the game (M7 feature 5):** PC Settings > **Key Bindings** lists the 24 gameplay actions with their
keys (eight rows at a time; Up / Down past the edge scroll). Select a row, then press the key or mouse button for it
(Escape cancels; 10 seconds without input cancels; F3, F4, F7 and Backtick stay with the runtime's overlays). The key
becomes that action's only key; an action that had it loses it, and one left without keys takes the changed action's
old keys (a swap), so nothing is left unbound. PC Settings > **Controller Buttons** lists the twelve buttons the
game's actions use, with the game's own button icons: Select a row, then press the controller button that should do
it (any key cancels); the two buttons exchange actions, so every button keeps one. Both apply **in gameplay only**:
menus keep Enter / Escape / arrows and A / B / D-pad (src/input/pad_remap_driver.cpp wraps the SDL controller driver;
the D-pad and sticks are not remapped). Saved in pc_settings.toml as `[gameplay_keys]` (control name = key names)
and `[controller_buttons]` (button = the physical button acting as it), only what differs from the defaults;
Restore Defaults clears both. The command-line options below still win over the file for one run.

Rebinding (command line): `--kuf_bind_<control>=<keys>` (menus), `--kuf_bind_game_<control>=<keys>` (gameplay),
`--kuf_bind_prompt_start=<keys>` (keys that press only Start at "Press START"); comma-separated, empty =
default. Controls: `a b x y left_shoulder right_shoulder back start left_thumb right_thumb dpad_up dpad_down
dpad_left dpad_right left_trigger right_trigger lstick_up lstick_down lstick_left lstick_right rstick_up
rstick_down rstick_left rstick_right`. Key names: `Return Escape Space Tab Backspace Up Down Left Right PageUp
PageDown Home End Insert Delete A`–`Z` `0`–`9` `F1`–`F24` `Numpad0`–`Numpad9` `NumpadEnter` (same key as
Return) `Shift Control Alt Pause` and more (`src/ui/keybinds.cpp` in the SDK), plus `MouseLeft MouseRight
MouseMiddle MouseX1 MouseX2`. Unknown names stop the game at start with an error. F3, F4, F7 and Backtick
belong to the runtime's overlays. `--kuf_keyboard=false` turns keyboard and mouse off. (M4's
`--kuf_bind_accept` is replaced by the contexts.)

## Mouse
| Mouse | Menus | Gameplay |
|---|---|---|
| Movement | pointer (free) | **camera** (pointer captured) |
| Wheel | one menu step per notch | D-pad (no single-player use) |
| Left click | confirm (Start at "Press START") | A weapon (primary attack) |
| Right click | back (B) | X weapon (secondary attack) |
| Middle click | right stick click | centre the camera |

**Mouse look** (`--kuf_mouse_look`, on by default). **With the modern camera (M6B, PC Settings > Modern Camera
On, the default)** the pointer's relative motion goes to the camera directly (`KufInputDriver::TakeMouseLook`):
no stick emulation, no acceleration, 0.025 degrees per count per Mouse Horizontal / Vertical step, its own
Mouse Y-Axis option; docs/CAMERA.md section 4, docs/SETTINGS.md. With Modern Camera Off, the M5A mapping below
applies (`--kuf_mouse_sensitivity`, `--kuf_mouse_invert_y`): the camera turns in proportion to mouse movement.
Measured in M5A: the game turns the camera only above a
right-stick deflection of about 12,900 and then roughly linearly (about 0.00194 screen px per frame per unit,
up to about 39 px per frame at full deflection). Mouse movement therefore starts at that dead zone and adds
260 units per pixel per frame: 20 px of movement per frame turns the view about 10 px per frame, steadily,
up to the game's maximum turn rate. The game's own Camera Normal/Inverted setting applies as for a pad.
Without a pointer lock (the offscreen sandbox) mouse look is unavailable and logged once per gameplay entry.

The pointer is mapped onto the guest image through the presenter's fit (16:9, letterboxed or
pillarboxed), so positions stay correct after resizing (`WindowToGuest`, tested). Since M6C the menus and HUD of
any window shape are drawn in exactly that centred 16:9 box (docs/DISPLAY.md), so the same mapping gives their
coordinates at every resolution and aspect (`display.*` tests check that the two agree; outside the box, on the
side bars of an ultrawide window, the pointer is outside the menus). **Pointer in the menus (M7):** moving the mouse over a row of the game's own menus selects it, and a left click
confirms the row under the pointer (`--kuf_menu_pointer`, on by default; src/runtime/menu_pointer.cpp,
docs/MENU_ARCHITECTURE.md 13). The port reads the focused control and its scene's buttons (or the focused list box's
rows) from the game's UI objects and finds the row under the pointer. **Since the M7 UX fixes** the steps to it are
made at the next game tick, all at once, by handing the focused control the same input event a D-pad press gives it
(its own handler, so the menus keep their cursor sound, bounds and rules; unavailable rows are skipped or refused as
with the keyboard): the highlight follows the pointer within one tick (33 ms, `--kuf_trace_menu_pointer` logs the
steps and the time since the movement). The first version pressed a virtual D-pad once per row, a press and a release
poll each at the game's 30 polls per second (67 ms per row, 400 ms over six rows): that was the sluggishness. A menu
whose handler does not move (none seen) still gets the virtual presses. A click selects the row under it and
confirms with A in the same tick; a click on no row is a plain A (dialogs), as before, except beside a message box
(scene 0x025: the Quit Game and storage questions), where it does nothing (UX fix 6). The pointer itself is the
system cursor (no smoothing, acceleration or dead zone); the game's highlight changes at its 30 Hz menu tick. Hover acts only when the
mouse moves, so it never fights the keyboard or a controller. List boxes (the options list, the language list) select
the visible row under the pointer; the wheel still scrolls them. Unknown layouts (no focused button or list box)
keep the plain M4 behaviour.

## Button prompts (M7 feature 6)
The game's texts show controller icons with markup (`@(A)Select  @(B)Back`, MENU_ARCHITECTURE 2.6). While the
keyboard and mouse are in use, texts the game looks up from then on show the bound keys instead: `[Enter] Select
[Esc] Back`, `[Arrows] Move`, `[WASD]` for the left stick, `[Mouse]` for the camera in gameplay, rebound keys as
bound (src/input/button_prompts.cpp, unit-tested; src/runtime/prompt_text.cpp rewrites the looked-up text, one
guest copy per distinct text). The device is the one used last: any key, mouse button, wheel or mouse movement means the
keyboard; a controller button, trigger or pushed stick means the controller. Before any input: the keyboard and mouse
(M7 UI follow-up: a connected controller used to mean controller icons and the Xbox selection arrow on the first
screens, K67); the first controller input switches.

**The selection arrow (M7 UI follow-up, src/runtime/menu_pointer.cpp):** the game's Xbox arrow beside the selected
row shows only while a controller is the device in use (also beside the title list's rows, which the game draws
without it); with the keyboard and mouse the game's highlight (and the pointer's hover) show the selection. The
decision is made in the game's own arrow update (sub_82612DE0) each time it runs, so no frame shows a stale arrow:
not after startup, a menu change, a submenu or a device switch (`--kuf_selection_arrow_auto=false`: the game's
arrow always). Texts already on screen keep their
prompts until the game looks them up again (the next selection move or screen). `--kuf_button_prompts=auto`
(default) `|controller|keyboard` fixes the choice; `--kuf_trace_prompts` logs each rewritten text.
Gameplay texts use the gameplay keys; menus, dialogs and the title the menu keys. Icons without a key (an unbound
action) and other markup stay as they are. Not covered: texts the game builds without the text lookup (none seen
with icons so far), and prompts drawn as images rather than markup.

**The Controls page (M7 UI polish, src/runtime/controls_screen.cpp):** with the keyboard and mouse in use the in-game
Controls page drops the controller picture and lists each action with its gameplay keys in front ("[Mouse R]  Use X
Weapon"; the keys bound now, so a rebinding shows at once), the co-op Target Ally block on the right; with a
controller it is the game's own page. It switches while open when the device changes (a key or mouse button, a
controller button or a pushed stick; resting sticks and small mouse jitter do not count: a stick must pass 12,000 of
32,767). No picture of a keyboard is drawn: the game has none, and the list is the "visually consistent alternative"
in the game's own fonts.

**The pointer on other screens (M7 UI polish):** the character slots and the game type screen work with the mouse
(click a slot to load it, hover selects, Continue Solo Game with a click; the greyed co-op rows cannot be selected,
their hidden twins no longer count as available); a click on the selected row is the game's own Select to that
control (not a key press); Difficulty is a value row: a click steps it with the game's own Right (and back to the
first value from the last). A virtual press is used only when a step through the control's handler does not move the
selection, and stops as soon as a press does not either.

## Controllers
- SDL's gamepad API (the SDK's SDL driver): Xbox, PlayStation, Nintendo and other SDL-supported pads,
  with SDL's mapping database. Connection and disconnection are handled at any time; a reconnected pad is a
  new device and gets the lowest free guest user (the first pad is user 0, shared with the keyboard).
- Default face buttons are **positional** (bottom = A, right = B), as on an Xbox pad and in SDL.
  `--pad_face_buttons=label` (SDK patch 0003) maps by the printed letter instead, for Nintendo-style pads
  (the button printed A confirms). PlayStation-style pads are always positional (cross = A, circle = B).
- D-pad and left stick both navigate the menus (the game reads both); Start, A, B as on the console.
- Gameplay uses the pad 1:1 as on the console (the game's Controls page): left stick move, right stick
  camera (click: centre), A/X weapons, B ability (Dash), Y pick up, LB/RB items (hold: inventory), LT aim,
  RT ability, Start status menu, Back options menu.
- Unplugging a pad while a button is held releases it (verified: no runaway auto-repeat).
- The game never shows a "reconnect controller" message: keyboard/mouse (and the NOP fallback) keep guest
  user 0 connected, so the keyboard keeps working when a pad is unplugged.
- Sandbox: no input devices by default. `tools/run_sandboxed.sh --pad /dev/input/by-id/<...>-event-joystick`
  binds exactly one controller node after checking that the kernel registered it only as a joystick (no
  keyboard or mouse handler), and prints its identity. The by-id link is stable across event renumbering.
  An Xbox Elite Series 2 (USB `045e:0b00`) works through SDL's evdev path with that one node (no hidraw,
  no udev; no hotplug inside the sandbox). Automated tests use SDL virtual gamepads (no device access).
- Controller input reaches the game only while its window has focus (SDL's default), like the keyboard.

## Testing
- `kuf_input_tests` (ctest `input.*`): translation, press/release, latching, repeat, focus, wheel, accept,
  bindings, window mapping (letterbox/pillarbox/resize), script parser.
- `tools/menu_test.py tests/menu/*.toml` (ctest `-L menu`, opt-in): sandboxed runs with `--kuf_input_script`
  (keyboard, mouse, focus, SDL virtual gamepads, and real SDL events pushed into the window, including a
  resize), checked by the highlighted menu row per capture, log lines and local references.
- Script files (M5B): `--kuf_input_script_file=<path>` reads the same items from a file, one or more per
  line, with blank lines and `#` comment lines (a `#` inside an item, as in `@menu#2+60`, is part of it).
  Long scenarios need this: one command-line argument is limited to 128 KiB. `tools/run_sandboxed.sh
  --input-script <file>` copies the file into the run directory; scenario TOMLs use `script_file = "..."`.
  `repeat ... until <context> press <Key>` repeats a held press (4 frames); its release always follows,
  even when the context is reached in between. The same holds for any release a repeat schedules (`up`, `pad up`,
  `pad axis <axis> 0`, `sdl key|button up`): when the repeat stops, its pending releases still apply once, so a
  repeat never leaves an input held (K42). Menu confirmations in the save scenarios use `press`: some
  dialogs missed a single-frame `tap`.
- Script time (M6D K38): numbers and anchors count game ticks, not presented frames. An input item at N is applied
  on the game thread when tick N + 1 starts, before that tick reads input (the virtual pad's joysticks are updated
  there too), so exactly that tick sees it at any frame rate; capture, trace and freeze run after the frame showing
  tick N. Logged as `[kuf-input-script] tick N: ...`; contexts as `[kuf-input] context: gameplay (tick T, frame F)`.
  The `input_ticks` check (tools/menu_test.py) verifies one tick per item in the camera tests.
- `--kuf_audio_meter`: logs output levels per 100 ms (diagnostic). `--kuf_audio_pace`: real-time audio
  pacing for silent sandbox runs (K14).

## Manual test (windowed, sound, controller)
```sh
tools/run_sandboxed.sh -t 600 -n m4-manual --gpu --window --audio \
  --pad /dev/input/by-id/usb-Microsoft_Controller_30324D4930303333393433333434-event-joystick \
  -- --kuf_exit_after_seconds=590
```
Grants: the Wayland socket, the session's PulseAudio socket (PipeWire), one controller node. Still no
network, read-only game folder, writes only to `build/runs/m4-manual/`, bounded logs, 10-minute hard limit.
