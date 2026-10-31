# Writes the frame-rate scenarios into tests/settings (argument: a directory for switch_ticks.txt, the gameplay ticks
# of the switch route for the state comparison's --ticks).
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk import *
FIX='saves_from_dir = "build/m6-saves/celine-shadows-depths-1"\n'
def walk(s, a, b, ticks=300):
    s.at(f'log mark {a}', 10); s.at('down W', ticks); s.at(f'log mark {b}', 10); s.at('up W', 60)

# 1. keyboard, in-game: every row and value, the two switch sequences, saved
s=S(); to_fps_ingame(s); s.at('capture fps_row')
s.wait(60)  # (M7 UX fix 3: FPS is no longer a page's first row: no Up here)
s.at('press Right'); s.at('log mark at_60', 300)
s.keys('Right', 2); s.at('log mark at_120', 300)
s.keys('Left', 3); s.at('log mark at_30', 300)
s.keys('Right', 3); s.at('log mark at_120b', 300)
s.keys('Right', 4); s.at('capture fps_unlimited'); s.at('log mark at_unlimited', 300)
s.keys('Left', 6); s.at('capture fps_60'); s.at('log mark at_60b', 300)
s.at('press Down'); s.at('press Right'); s.at('capture interpolation_off', 150); s.at('press Left')
s.at('press Down', 60); s.at('capture advanced')
s.at('press Up', 60); s.at('press Down', 60)
s.at('press Right'); s.at('press Down'); s.at('press Right'); s.at('press Left'); s.at('press Down'); s.at('press Right')
s.at('capture advanced_changed', 60)
leave_ingame(s); walk(s, 'play_a', 'play_b')
write('frame_rate_menu', '''# PC Settings > Frame Rate in-game with the keyboard (M6D D5, docs/HIGH_FPS.md section 21). CONTINUE from
# build/m6-saves/celine-shadows-depths-1 (read only); options menu, Settings, PC Settings; the categories (M7 feature
# 3): Down to Display, Return, Down five times to FPS (M7 UX fix 3; FPS: 30 (Original), Interpolation: On, World
# Objects, Animation, Effects: On). FPS Right: 60 (held 10 s), Right twice: 120, Left three times: 30, Right three times:
# 120, Right four times: Unlimited, Left six times: 60 (each value applied within a tick, the ones passed through
# too). Interpolation Right (Off) and Left (On); Down to World Objects; Up and Down; World Objects Off, Animation Off
# and On, Effects Off. Back to the categories, Back saves; gameplay at 60
# with the objects and effects not interpolated. frame_rate_check.py: every change reached the scheduler, the guest
# vblank followed, each held rate drawn, the game isolated. The next run (frame_rate_persist) starts from the file.''',
'description = "Frame-rate page: FPS 30 -> 60 -> 120 -> 30 and 120 -> Unlimited -> 60, interpolation rows, saved"\n'
f'seconds = {int((1500 + s.t) / 30) + 40}\n' + FIX + 'args = ["--kuf_profile=true"]\n', CONT + s.l,
'''capture_frames = []
expect_saves_unchanged = true
pc_settings_expect = { "frame_rate.limit_fps" = 60, "frame_rate.interpolation" = true, "frame_rate.interpolate_objects" = false, "frame_rate.interpolate_animation" = true, "frame_rate.interpolate_effects" = false, "camera.modern_camera" = true }
log_expect = [
    "[kuf-input] context: gameplay",
    "[kuf-settings] frame rate: 30 (original) interpolation true objects true animation true effects true",
    "[kuf-pacing] tick", "frame rate 30 (original) -> 60, interpolation true objects true animation true effects true",
    "frame rate 240 -> unlimited", "frame rate unlimited -> 240", "frame rate 60 -> 30 (original)",
    "[kuf-present] camera and player interpolation between ticks: on, world objects: off, skeletal poses: on, effect clock: off",
]
log_sequence = [
    "[kuf-pc-menu] PC Settings opened",
    "[kuf-pc-menu] display page",
    "[kuf-pc-menu] frame rate 60 interpolation true",
    "[kuf-settings] saved /run-data/user/pc_settings.toml",
    "[kuf-pc-menu] PC Settings closed",
]

[format]
width = 1280
height = 720
''')

# 2. persist
s=S(t=200); walk(s, 'play_a', 'play_b'); s.wait(100); to_fps_ingame(s); s.at('press Left'); s.at('log mark at_30', 300)
s.at('press Down'); s.at('press Down', 60); s.at('press Left'); s.at('press Down'); s.at('press Down'); s.at('press Left')
s.at('capture advanced_on', 60); leave_ingame(s); walk(s, 'play_c', 'play_d')
write('frame_rate_persist', '''# The frame-rate settings survive a restart (M6D D5): a new process starts with the pc_settings.toml that
# frame_rate_menu wrote (60 fps, world objects and effects not interpolated; ctest orders them). The game draws 60
# frames per second from the first gameplay frame, without a command-line flag; the frame-rate page shows 60. FPS
# Left: 30 (the original pacing again, at once); World Objects and Effects On. Back saves; gameplay at 30.''',
'description = "Frame-rate settings loaded after a restart and used without command-line flags; changed back to 30"\n'
f'seconds = {int((1500 + s.t) / 30) + 40}\n' + FIX + 'pc_settings_from_run = "frame_rate_menu"\nargs = ["--kuf_profile=true"]\n', CONT + s.l,
'''capture_frames = []
expect_saves_unchanged = true
pc_settings_expect = { "frame_rate.limit_fps" = 30, "frame_rate.interpolation" = true, "frame_rate.interpolate_objects" = true, "frame_rate.interpolate_animation" = true, "frame_rate.interpolate_effects" = true }
log_expect = [
    "[kuf-settings] loaded /run-data/user/pc_settings.toml",
    "[kuf-settings] frame rate: 60 interpolation true objects false animation true effects false",
    "[kuf-pacing] render-only frames: up to 60 frames per second between the game's 30 ticks",
    "[kuf-present] camera and player interpolation between ticks: on, world objects: off, skeletal poses: on, effect clock: off",
    "frame rate 60 -> 30 (original)",
    "[kuf-settings] saved /run-data/user/pc_settings.toml",
]
log_absent = ["[kuf-pacing] frame rate from the command line"]

[format]
width = 1280
height = 720
''')

# 3. controller
s=S(); to_fps_ingame(s, pad=True); s.keys('right', 3, verb='pad press'); s.at('capture fps_120'); s.at('log mark at_120', 300)
s.at('pad press down'); s.at('pad press a', 150); s.at('pad press down', 60); s.at('pad press a', 60)
leave_ingame(s, pad=True); walk(s, 'play_a', 'play_b') if False else None
s.at('log mark play_a', 300); s.at('log mark play_b', 10)
write('frame_rate_controller', '''# The frame-rate page with a controller (M6D D5): an SDL virtual Xbox 360 pad, as a player's. CONTINUE from the
# Shadow Depths fixture; Back opens the options menu, D-pad to Settings, A, D-pad to PC Settings, A, D-pad down to
# Display, A, D-pad down five times to FPS (M7 categories, UX fix 3). D-pad right three times: 120; D-pad down, A: Interpolation Off (A steps the value);
# D-pad down, A: World Objects Off. B four times saves and returns to the game, which draws up to 120 frames
# per second, each a copy of its tick.''',
'description = "Frame-rate page with a controller: D-pad, A and B"\n'
f'seconds = {int((1500 + s.t) / 30) + 40}\n' + FIX + 'args = ["--kuf_input_script_pad=xbox360", "--kuf_profile=true"]\n',
['100 pad connect','500 pad press start','700 pad press a','850 pad press a','1000 pad press down','1100 pad press a',
 '1300 repeat 60 until gameplay pad press a'] + s.l,
'''capture_frames = []
expect_saves_unchanged = true
pc_settings_expect = { "frame_rate.limit_fps" = 120, "frame_rate.interpolation" = false, "frame_rate.interpolate_objects" = false, "frame_rate.interpolate_animation" = true }
log_expect = [
    "[kuf-input] context: gameplay",
    "frame rate 90 -> 120, interpolation true objects true animation true effects true",
    "frame rate 120 -> 120, interpolation false objects true animation true effects true",
    "[kuf-settings] saved /run-data/user/pc_settings.toml",
]

[format]
width = 1280
height = 720
''')

# 4. mouse, title SETTINGS
s=S('', 1000)
s.keys('-1', 9, 40, verb='wheel'); s.wait(30); s.at('press MouseLeft', 150)
s.keys('-1', 2, 40, verb='wheel'); s.wait(30); s.at('press MouseLeft', 150)
s.keys('-1', 5, 40, verb='wheel'); s.wait(30); s.at('capture fps_row')
s.keys('MouseLeft', 3, 40); s.at('log mark at_120', 330)
s.at('sdl wheel -1 640 360', 60); s.at('sdl click left 640 360', 100); s.at('capture interpolation_off')
s.at('press MouseRight', 100); s.at('press MouseRight', 100); s.at('press MouseRight', 100); s.at('capture main_menu')
write('frame_rate_mouse_title', '''# The frame-rate page with the mouse in the title screen's SETTINGS (M6D D5; the menus have no pointer: the wheel
# moves one row per detent, the left button selects, the right button goes back, docs/INPUT.md). The keyboard opens
# SETTINGS (as pc_settings_title); nine wheel detents down to PC Settings, a left click opens the categories; two
# detents down to Display, a left click opens it, five detents down to FPS (M7 UX fix 3); three left clicks step it 30 -> 60 -> 90 -> 120 (Select
# steps forward); a real SDL wheel event to Interpolation and a real SDL click: Off. Right click three times: the
# categories, SETTINGS (the file written), the main menu. The title screen
# and its menus are drawn over the title-loop movie, which keeps the render thread busy: render-only frames reach
# about 36 per second there whatever the setting (docs/HIGH_FPS.md 21.6), so frame_rate_check.py --min-fps 33 (more
# than one frame per tick: the setting reached the scheduler; no lock at 30).''',
'description = "Frame-rate page with the mouse in the title SETTINGS: wheel, left click steps, right click back"\n'
f'seconds = {int(s.t / 30) + 30}\n', ['500 tap Return','700 tap Escape','800 press Down','900 press Return'] + s.l,
'''capture_frames = []
pc_settings_expect = { "frame_rate.limit_fps" = 120, "frame_rate.interpolation" = false }
log_expect = [
    "[kuf-pc-menu] PC Settings opened (title SETTINGS)",
    "frame rate 90 -> 120, interpolation true",
    "frame rate 120 -> 120, interpolation false",
    "[kuf-settings] saved /run-data/user/pc_settings.toml",
]

[format]
width = 1280
height = 720
''')

# 5. switching with the game's state traced, and the fault run
s=S(); to_fps_ingame(s); s.at('capture fps_row')
s.at('press Right'); s.at('log mark at_60', 300); s.keys('Right', 2); s.at('log mark at_120', 300)
s.keys('Left', 3); s.at('log mark at_30', 300); s.keys('Right', 3); s.at('log mark at_120b', 300)
s.keys('Right', 4); s.at('log mark at_unlimited', 300); s.keys('Left', 6); s.at('log mark at_60b', 300)
s.at('press Down'); s.at('press Right', 300); s.at('press Left', 60); s.at('press Down', 60)
for _ in range(3):
    s.at('press Right', 150); s.at('press Left', 60); s.at('press Down', 60)
leave_ingame(s)
s.at('log mark walk', 10); s.at('down W', 200); s.at('look 120 0', 100); s.at('up W', 30); s.at('down D', 150); s.at('up D', 60)
s.at('look -200 40', 120); s.at('down W', 200); s.at('up W', 60); s.at('log mark end', 10)
gameplay_ticks = s.t
for name, extra, desc in (('frame_rate_switch', '', 'Frame rate and interpolation switched in the menu during play: the game unchanged'),
                          ('frame_rate_switch_ignored', ', "--kuf_debug_frame_rate_ignored=true"',
                           'Fault test: frame-rate changes made in the menu dropped before the scheduler (stays at 30)')):
    write(name, f'''# M6D D5 runtime switching (docs/HIGH_FPS.md 21.4): CONTINUE from the Shadow Depths fixture, the frame-rate page
# (as frame_rate_menu): 30 -> 60 -> 90 -> 120 -> 90 -> 60 -> 30 -> ... -> 120 -> 144 -> 165 -> 240 -> Unlimited ->
# ... -> 60, Interpolation Off and On, each advanced row Off and On; back to the game, a walk with turns. The game's
# state traced per tick (--kuf_trace_game_state, --kuf_trace_camera_orbit).{" With --kuf_debug_frame_rate_ignored the menu's changes are dropped before the frame scheduler: the game stays at 30 fps (frame_rate_check.py --expect-not-applied must see it; the reference for state_compare.py)." if extra else " state_compare.py against frame_rate_switch_ignored (the same input, every change dropped, 30 fps throughout): the game's state equal at every tick."}''',
          f'description = "{desc}"\nseconds = {int((1500 + s.t) / 30) + 40}\n' + FIX +
          f'args = ["--kuf_profile=true", "--kuf_trace_game_state=true", "--kuf_trace_camera_orbit=true"{extra}]\n', CONT + s.l,
          '''capture_frames = []
expect_saves_unchanged = true
log_expect = ["[kuf-input] context: gameplay", "[kuf-pc-menu] display page"]
''')
open(sys.argv[1] + '/switch_ticks.txt', 'w').write(str(gameplay_ticks))

# 6. command line over the saved settings
s=S(t=200); walk(s, 'play_a', 'play_b'); s.at('press Escape', 100); s.keys('Down', 4, 50); s.wait(50); s.at('press Return', 150)
s.keys('Down', 8); s.wait(70); s.at('press Return', 150); s.at('press Return', 150); s.at('press Right', 60); s.at('capture mouse_5'); leave_ingame(s); walk(s, 'play_c', 'play_d')
write('frame_rate_cli', '''# Command line over the saved settings (M6D D5, docs/SETTINGS.md): pc_settings.toml says 60 fps with the effect clock
# off; --kuf_frame_rate=120 is given. The flag wins for this run (120 frames per second; the effect clock still off:
# only the flags given override), the log says so. In the menu Mouse Horizontal +1 (5); Back writes the file: the
# frame rate in it stays 60 (the command line is not saved), the mouse value is 5.''',
'description = "--kuf_frame_rate over the saved frame rate for one run; the file keeps the saved value"\n'
f'seconds = {int((1500 + s.t) / 30) + 40}\n' + FIX + 'pc_settings_file = "tests/settings/frame_rate_cli_pc_settings.toml.in"\n'
'args = ["--kuf_frame_rate=120", "--kuf_profile=true"]\n', CONT + s.l,
'''capture_frames = []
expect_saves_unchanged = true
pc_settings_expect = { "frame_rate.limit_fps" = 60, "frame_rate.interpolate_effects" = false, "mouse.sensitivity_horizontal" = 5 }
log_expect = [
    "[kuf-settings] frame rate: 60 interpolation true objects true animation true effects false",
    "[kuf-pacing] frame rate from the command line for this run (the saved settings unchanged): --kuf_frame_rate=120",
    "[kuf-pacing] render-only frames: up to 120 frames per second between the game's 30 ticks",
    "world objects: on, skeletal poses: on, effect clock: off",
    "[kuf-settings] saved /run-data/user/pc_settings.toml",
]
''')
open(T + 'frame_rate_cli_pc_settings.toml.in', 'w').write('''# Saved frame-rate settings for frame_rate_cli.toml: 60 fps, the effect clock off.
version = 1
[frame_rate]
limit_fps = 60
interpolate_effects = false
''')

# 7. invalid values
write('frame_rate_invalid', '''# A pc_settings.toml with invalid frame-rate values (M6D D5): each falls back to its default with a warning (the
# original 30 fps; interpolation on), the valid one is used, no render-only frames are drawn, the game starts.''',
'description = "pc_settings.toml with invalid frame-rate values: the original 30 fps, the valid value kept"\nseconds = 45\n'
'pc_settings_file = "tests/settings/frame_rate_invalid_pc_settings.toml.in"\n',
['500 tap Return','700 press Return','900 capture main_menu'],
'''capture_frames = []
pc_settings_expect = { "frame_rate.limit_fps" = 29, "frame_rate.interpolate_effects" = false }
log_expect = [
    "[kuf-settings] loaded /run-data/user/pc_settings.toml",
    "frame_rate.limit_fps: invalid value (default used)",
    "frame_rate.interpolation: invalid value (default used)",
    "frame_rate.interpolate_objects: invalid value (default used)",
    "[kuf-settings] frame rate: 30 (original) interpolation true objects true animation true effects false",
    "[kuf-input] context: menu",
]
log_absent = ["[kuf-pacing] render-only frames"]
''')
open(T + 'frame_rate_invalid_pc_settings.toml.in', 'w').write('''# Valid TOML with invalid frame-rate values on purpose (tests/settings/frame_rate_invalid.toml).
version = 1
[frame_rate]
limit_fps = 29
interpolation = "yes"
interpolate_objects = 1
interpolate_effects = false
''')

# 8. display changes at 120
s=S(t=200); walk(s, 'play_a', 'play_b'); s.at('press Escape', 100); s.keys('Down', 4, 50); s.wait(50); s.at('press Return', 150)
s.keys('Down', 8); s.wait(70); s.at('press Return', 150); s.keys('Down', 2); s.at('press Return', 100); s.wait(60); s.at('capture display_page')
s.at('press Left', 60); s.at('press Down'); s.at('press Left', 60); s.at('press Down'); s.at('press Down'); s.at('press Right', 60)
s.at('press Left', 60); s.keys('Down', 8); s.at('press Return', 100); s.at('press Return', 100); s.at('capture kept')
s.keys('Up', 6); s.wait(40); s.at('capture fps_row'); leave_ingame(s); walk(s, 'play_c', 'play_d')
write('frame_rate_display', '''# Display changes at 120 fps (M6D D5): the frame rate from the PC settings (--kuf_pc_settings, not --kuf_frame_rate),
# a simulated 21:9 monitor (as display_menu). The display page: Mode Windowed, a smaller size, Widescreen Off and On
# (at once), down to Apply (the last row) and Keep (the window becomes 1920x1080); Up six times to FPS (120 shown).
# Gameplay before and after: up to 120 frames per second (the swapchain recreated, the frame rate kept; one limiter).''',
'description = "Display mode, size and widescreen changed at 120 fps: the frame rate kept"\n'
f'seconds = {int((1500 + s.t) / 30) + 40}\n' + FIX +
'args = ["--kuf_display_modes=3440x1440@100,3440x1440@60,2560x1080@60,1920x1080@60,1280x720@60", "--kuf_pc_settings=frame_rate.limit_fps=120", "--kuf_profile=true"]\n',
CONT + s.l,
'''capture_frames = []
expect_saves_unchanged = true
pc_settings_expect = { "display.mode" = "windowed", "display.width" = 1920, "display.height" = 1080, "frame_rate.limit_fps" = 120 }
log_expect = [
    "[kuf-settings] overrides for this run: frame_rate.limit_fps=120",
    "[kuf-pacing] render-only frames: up to 120 frames per second between the game's 30 ticks",
    "[kuf-pc-menu] display kept: windowed 1920x1080",
    "[kuf-pc-menu] display page",
]
log_absent = ["[kuf-pacing] tick", "frame rate request dropped"]
''')

# 10. switch, save, title, continue
s=S(); to_fps_ingame(s); s.keys('Right', 3); s.at('log mark at_120', 60); leave_ingame(s); walk(s, 'play_a', 'play_b')
s.at('tap Escape', 100); s.keys('Down', 5, 30, verb='tap'); s.wait(50); s.at('press Return', 120); s.at('press Return')
t=['@prompt#2+100 capture title_after_save', '@prompt#2+200 tap Return', '@prompt#2+400 press Return',
   '@prompt#2+550 press Return', '@prompt#2+700 press Down', '@prompt#2+800 press Return',
   '@prompt#2+1000 repeat 60 until gameplay#3 press Return', '@gameplay#3+200 log mark play_c',
   '@gameplay#3+210 down W', '@gameplay#3+510 log mark play_d', '@gameplay#3+520 up W', '@gameplay#3+600 log mark end']
write('frame_rate_save', '''# Saving, the title and loading after a frame-rate change (M6D D5): CONTINUE from the Shadow Depths fixture (the run's
# copy), 120 fps set in the menu, gameplay, the game's Quit save (options > Quit > save and return to the Title
# Screen > Yes), the title, CONTINUE (the save just written), gameplay (its third entry: closing the menu was the second): still 120 frames per second, the game
# loaded, no crash (the render-only frames' kept data released through the save's object releases, K46).''',
'description = "120 fps set in the menu, Quit save, title, CONTINUE: the frame rate kept, the save loads"\n'
'seconds = 300\n' + FIX + 'args = ["--kuf_profile=true"]\n', CONT + s.l + t,
'''capture_frames = []
pc_settings_expect = { "frame_rate.limit_fps" = 120 }
log_expect = [
    "frame rate 90 -> 120, interpolation true",
    "(save message 0x41127)",
    "[kuf-input] context: start prompt",
]
log_sequence = ["frame rate 90 -> 120", "committed", "[kuf-input] context: start prompt", "[kuf-input] context: gameplay",
                "log mark play_d"]
''')
print('ok', gameplay_ticks)
