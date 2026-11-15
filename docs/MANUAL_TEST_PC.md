# Manual test on your PC (M15)

These are the things automated runs cannot tell us: how it feels with your hands, a real mouse, a real controller and your
desktop. About 45 minutes for everything; each part stands alone. No debugger or terminal is needed while you play.

## Starting the test build

The published 0.8.0-rc.2 does **not** have the M15 changes, so test this build. It uses its own folder (`build/manual-test`) for
saves, settings and logs: your installed game and your real saves and settings stay untouched (your saves are copied once, if you have any).

1. Once, in a terminal: `~/Work/kuf-cod-recomp/tools/manual_test.sh --make-launcher`
2. Double-click `KUF test build` in `~/Work/kuf-cod-recomp/build/manual-test`.
   If your file manager will not start it, run `~/Work/kuf-cod-recomp/tools/manual_test.sh` instead.

**When something is wrong**, note the test number and what you saw. The log is `build/manual-test/logs/kuf_cod.log`; send it
along, please, and a screenshot if it is visual. Start again from defaults at any time: Settings > Restore Defaults.

## A. Menus and mouse

| # | Do this | Correct | Watch for |
|---|---|---|---|
| A1 | Move the mouse slowly and quickly over the title list and every Settings page | The row under the pointer lights up at once | Lag, a row that stays behind, the highlight jumping back |
| A2 | In Settings > Mouse & Keyboard, hover each row, then use the arrow keys, then a controller if you have one | The right column explains the selected row, the same for all three | Flicker, text overlapping or cut off, a description that does not match what the row does |
| A3 | Settings > Audio: click somewhere on the Music bar, then drag it | The value jumps to the click, follows the drag, and the label updates as you go | The value moving in steps not under the pointer, another row becoming selected during the drag |
| A4 | Drag that bar and keep going past its end, far outside it, and let go there | The value stops at its end; letting go does nothing else | A click or Back happening at the release, the slider staying stuck to the pointer |
| A5 | Close Settings and open it again | The values you set are still there | Values reset; also restart the game and check once |
| A6 | Settings > Display: roll the wheel, flick it fast, drag the scrollbar | One row per notch, nothing queued after you stop; the highlighted row stays visible; the top and bottom just stop | Scrolling that continues after you stop, the highlight off-screen |
| A7 | In a game: Tab for the status menu. Hover the equipment rows, click one, change tab, right-click back | Hover selects, a click opens the row's item list, right click returns | A click that does nothing, a click going through to the game |
| A8 | **Not tested by the automated runs:** equip an item with the mouse from that list. Use a character you do not mind changing | The item is equipped and the stats change, as with the keyboard | Wrong item, nothing happening, a crash |
| A9 | Inventory: hover the items, use the wheel, right-click back | The list follows; the inventory's own tabs do not react to clicks (they do not in the original either) | Items not selectable with the mouse |
| A10 | **Not tested by the automated runs:** wherever you find a shop, buy and sell one thing with the mouse only, then with the keyboard | Both work the same; your money changes correctly | Rows you cannot reach, a click buying the wrong thing or buying twice |
| A11 | When you have points to spend: status menu, open the level-up dialog. Click the arrows, click beside the dialog, confirm | The arrows change the value; a click beside it does nothing; confirm applies | A click outside confirming or closing it |
| A12 | Quit Game on the title list: click beside the question, then answer No | The click beside it does nothing | The question answering itself |
| A13 | In any menu, press Backspace. Text fields exist only in the multiplayer menus (lobby name, password), if you get there: type, Backspace, Å Ä Ö, then Enter | In a menu Backspace does nothing. In a field it deletes one letter and never leaves the field. The fields have only had unit tests | Backspace going back a screen |

## B. Camera with the mouse

Set Settings > Display > FPS to 60 or higher for B2 and B3 (at 30 the setting helps less). Try each with Settings > Camera > **Low Latency** Off, then On.

| # | Do this | Correct | Watch for |
|---|---|---|---|
| B1 | Low Latency Off: move the mouse slowly, quickly, and stop | Same as you know it: a smooth turn | Anything different from before: Off should be unchanged |
| B2 | On: flick left and right quickly, stop dead | The view stops when your hand stops, with less float than Off | Stutter or tiny jumps during a steady turn; the view twitching back when you stop |
| B3 | Say which you prefer, and why: this decides the default | | |
| B4 | On: stand with your back against a wall and turn around quickly, several times, also in a corner and a narrow passage | The camera comes in and goes out as with Off | **Seeing through the wall**, flashes of the world outside, the camera popping in and out more than with Off. A black or wrong frame in a fast turn was found and reduced in testing; a single frame at a sharp corner can remain, so report how often you notice one |
| B5 | On, around a **large enemy**: let it get between you and the camera, circle it, then turn quickly | The same as with Off | The view jumping or going inside the enemy more than with Off |
| B6 | On: Settings > Camera > Recentering On, walk and turn, then leave the mouse alone for a few seconds | The view eases back to the normal height | Jerks where the mouse and the recentering meet |
| B7 | If your mouse has a polling rate setting: compare 1000 Hz with the highest it has, by turning slowly and quickly | The same turn at the same speed | The camera faster, slower or rougher at some rate. Only synthetic tests have been done on this |
| B8 | Mouse Horizontal and Vertical sliders at low, then high | The turn speed changes in the way you set it, for each axis | One axis reversed or ignored |

## C. Combat and movement

| # | Do this | Correct | Watch for |
|---|---|---|---|
| C1 | Fight for a few minutes at FPS 30, then at 60 or more | Attacks, dodges and timing feel the same at both | Attacks or dodges firing late, twice or not at all |
| C2 | Hold W + D, tap keys together, change direction in mid-run | Moves as you expect; diagonals no faster than straight | Walking on after you let go |

## D. Focus and devices

| # | Do this | Correct | Watch for |
|---|---|---|---|
| D1 | In play, hold W and Alt-Tab away. Release W in the other window, come back | You stopped walking when you left, and stay stopped | Walking on its own; the mouse not captured when you return |
| D2 | Alt-Tab with the mouse moving, come back, turn | The camera did not jump | A big jump when returning; your desktop's pointer stuck in the game |
| D3 | While dragging a slider, Alt-Tab, come back | The drag has ended | The slider following the pointer without a press |
| D4 | Controller, in play: hold the stick and Alt-Tab, come back still holding | It stops without focus, walks again when you are back, stops on release | Input reaching the game behind your other window |
| D5 | Controller, while it is vibrating (being hit): Alt-Tab | The motors stop | A controller that keeps buzzing |
| D6 | Unplug or switch off the controller in a menu, use the keyboard, connect it again | Keyboard works meanwhile; the controller works again with no restart; prompts follow what you touched last | A controller needing a restart, a stuck button |
| D7 | Controller settings: Settings > Controller > Stick Settings. Change Stick Horizontal and Vertical, both dead zones and Acceleration; Vibration Strength on the Controller page | Each has the effect described on the right; the defaults feel as before | A dead zone that stops small movements you want; vibration still strong at low strength |
| D8 | With a controller: the title list | An arrow beside the selected row; touching the mouse makes it go away | The arrow flashing with the keyboard and mouse (it is checked in a scripted run now, not by a person) |
