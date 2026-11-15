# Manual test on a Steam Deck

Nothing has been run on a physical Deck yet. Part 1 works with what is published now. Part 2 needs a build with M15, which **is not
published**: the public 0.8.0-rc.2 AppImage does not contain any M15 change. Ask for an M15 test build when you get to part 2; it does not need a public release.

You need your own game files on the Deck (a retail dump or disc image works as it is).

**Report:** the test number, what you saw, and a photo if it is visual. Logs: in Konsole,
`~/"Games/Kingdom Under Fire - Circle of Doom"/kuf-cod --collect-logs`. Button and menu names in Steam can differ by SteamOS version.

## Part 1: with the published v0.8.0-rc.2

| # | Do this | Correct | Watch for |
|---|---|---|---|
| 1.1 | Desktop Mode. Download `KingdomUnderFire-CircleOfDoom-Setup-0.8.0-rc.2-x86_64.AppImage`, tick Allow executing in its Properties, double-click | The setup opens | Nothing happening, a message about missing libraries (GTK or FUSE). Write down the exact text |
| 1.2 | Choose the game folder (SD card too), the installation folder, leave Add to Steam ticked. Steam must be closed: Steam > Exit, then Retry. Install, plugged in | Each stage shows progress and it finishes. **Note how many minutes**: this has never been measured | The Deck overheating or running out of memory, a failed stage. Try Cancel once and start again: it should continue |
| 1.3 | Start the game from the setup's shortcut in Desktop Mode | The title appears with sound | A black window, a Vulkan message, no sound |
| 1.4 | Start Steam, go back to Gaming Mode: Library > Non-Steam | The game is listed and starts full screen | Missing from the library (add it by hand: `kuf-cod` in the installation folder), a wrong picture shape |
| 1.5 | Menus with the Deck's buttons and sticks | Everything is reachable; the prompts show controller buttons | A menu you cannot leave, wrong prompts |
| 1.6 | New game or Continue: move, turn, fight, open the status menu | Everything responds, sticks in the expected direction | Doubled presses, drift when you are not touching the stick |
| 1.7 | Steam Input: game's page > Controller settings. Try the default layout, then a "Gamepad with Mouse Trackpad" template | Buttons act once; the right trackpad moves the pointer and hovering selects rows in the menus | Pointer not moving, a click doing nothing, the game confused by mouse and pad together |
| 1.8 | If you set up gyro in Steam Input (as mouse): turn in play | The camera turns; stops when you stop | Jumps; camera drift |
| 1.9 | Hurt your character, with Vibration On and Off (in Settings) | The Deck vibrates only when On | Vibration that continues |
| 1.10 | Read each Settings page from arm's length (in this release the descriptions are not in a column) | Everything is readable at 1280x800, nothing cut off | Text too small, overlaps |
| 1.11 | Steam button in play, wait, come back. Also the overlay with the ... button | The game continues; the controls work at once | Stuck movement after coming back, lost audio |
| 1.12 | Sleep the Deck during play, wake it | The game continues | A crash, no sound, a black picture |
| 1.13 | Save at a save point, quit, start again, Continue. Then reinstall nothing: restart the Deck and Continue again | Your character and place are there | A missing save, the storage question each time |
| 1.14 | Ten minutes of play with the Deck's performance overlay on (level 2 or more), defaults (FPS 30, VSync on) | An even 30, no regular stutter after the first minutes. **Note FPS and battery watts**. The first minutes stutter more: shaders are being built | Slow-downs in fights, frames way below 30 |
| 1.15 | Settings > Display > FPS 60, Apply, play a few minutes | Note whether it holds 60 and how it feels | Not holding, or the display reverting |

## Part 2: only with an M15 test build (not in the public AppImage)

| # | Do this | Correct | Watch for |
|---|---|---|---|
| 2.1 | Any Settings page, at arm's length | The selected row's description in a column on the right, readable at 1280x800, wrapped within the column | Text too small; descriptions overlapping the rows |
| 2.2 | Settings > Controller > Stick Settings: Stick Horizontal, Stick Vertical | Each changes only its axis | The wrong axis |
| 2.3 | Camera Dead Zone and Move Dead Zone. Find the smallest that has no drift on your Deck. Move Dead Zone defaults to 0 | With a sufficient zone no drift; **note the values you end up with**, and whether the default drifts | Drift at the defaults; a jump when leaving the zone |
| 2.4 | Acceleration, On and Off | On: small stick movements precise, full deflection quicker; Off as before | A lurch at full deflection |
| 2.5 | Settings > Controller > Strength at 10, 50, 100, getting hit each time | Clearly different, all stop when the hit ends | Too weak to feel at low values |
| 2.6 | Dead zones with Steam Input's own stick settings changed too | Noting whether the two add up, and whether one should be left alone | Controls that feel dead |
| 2.7 | Trackpad as mouse: sliders, click and drag; equipment rows; wheel if it is mapped | As on the PC | A click not landing where the pointer is, a drag ending early |
| 2.8 | Steam overlay in the middle of a slider drag, and while holding a stick | The drag ends; movement stops and resumes only when you push the stick again | A stuck slider; walking alone after the overlay closes |
| 2.9 | Low Latency, only if a mouse or the trackpad turns the camera: Off and On at FPS 30 and 60 | Say whether On is better; it affects mouse input only. A stick is unchanged | Twitching in the view; extra stutter |
| 2.10 | Save from 1.13: Continue in the M15 build, save again, quit, Continue | The same character and place | A save that does not load |
| 2.11 | Repeat 1.14 with the same place and settings | Same FPS or better; no new stutter. **Note FPS and watts** | Lower FPS than the published build |
