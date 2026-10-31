# Saves (M5B)

How Circle of Doom (v1) saves, how the port stores saves, and how that is tested. Everything here comes
from sandboxed runs with `[content]` logging (SDK patch 0005) and our own disassembly; v1 addresses.

## 1. The game's save behaviour **[V]**

**One package for everything.** The game keeps all characters in one Xbox 360 content package of type
`00000001` (saved game), title `424507D1`, file name `Circle of Doom - Character Save` (display name
"KUF:Circle of Doom-Character Save"). Inside:

| File | Size seen | What |
|---|---|---|
| `GlobalSetting` | 231 bytes | settings shared by all characters (first file written) |
| `<XX>_<n>` | 5,729 to 6,572 bytes | one per character: `KD` Kendal, `LH` Leinhart, `CL` Celine, `CU` Curian, `DU` Duane, `RG` Regnier; `<n>` differs per character created (KD_1350, KD_1351, KD_1870, LH_1271 seen) |

Character file: big-endian, a `[PLRDATA` block at 0x290; EXP is an int32 at 0x2A4, gold an int32 at
0x500 (checked against the status screen at EXP 0, 17, 21 and gold 100). **Current HP is not saved**: a
file written at HP 14/78 holds no field with that value that a full-HP save does not hold at the same place,
and the character loads at full HP (78/78). Max HP, SP, Luck, EXP, gold, equipment and inventory are saved.

**When it saves** (each save = the game rewrites files in `savedisk:`, then calls `XamContentFlush`, usually
twice in a row):

| Moment | Observed |
|---|---|
| First start: the storage message is confirmed and no package exists | package created, `GlobalSetting` written |
| A new character is chosen ("Play as this character?" > Yes) | character file created |
| Loading into the world, the intro | two saves |
| "End tutorial?" > Yes | save (character file 6,137 bytes) |
| During play, without a screen change | once in the first fight (trigger not identified) |
| The character dies ("Game Over") | save, before the Game Over menu (seen once: GlobalSetting's counters changed) |
| Options menu > Quit > "Do you want to save and return to the Title Screen?" > **Yes** | save, then the title screen: the **manual save** |

**One save, two steps (K22).** Every save in the table after character creation is the same code: message
`0x41127` of the save manager's handler `sub_826EFA38` (v1). It
1. writes the character file and calls `XamContentFlush` (`sub_826B0F68`),
2. checks that the storage device is still present (and stops if not),
3. writes `GlobalSetting` and calls `XamContentFlush` again (`sub_826B0D68`; message `0x41128` is this step
   alone).

Measured with the commit log (which files each commit changed) and the guest call chain of every flush:
the character file changes in step 1 at every save; GlobalSetting changes only when its counters do (it holds
values shared by all characters; offset 0x88 went from 21 to 26 after more fighting and a death), otherwise
step 2's flush has nothing new. The two flushes come about 5 ms apart. Character creation
(`sub_826B0F68` from `sub_8266C55C`) and the first GlobalSetting (`sub_826B0D68` at package creation) are
single-file saves. On the console each flush is durable on its own, so an interruption between the steps
leaves the new character file beside the previous GlobalSetting: two points in time mixed. The game does
not check flush results (both callers ignore them).

Sleep (options menu) does nothing in the tutorial forest (selected with keyboard and pad; no save, no
screen change); it is not used as a save point here. No other manual save exists in the menus reached.

**How it finds saves.** After Press START, the first confirm of "Your game progress will not be saved if the
storage device is removed during play" makes the game enumerate packages of type 1
(`XamContentCreateEnumerator`), then:

- no package: `XamContentCreateEx` with flags `0x812` = CREATE_ALWAYS (2) | 0x10 | 0x800 (call site
  0x826B1E30 through the wrapper at 0x827EB090);
- a package: flags `0x13` = OPEN_EXISTING (3) | 0x10 (0x827EB260).

The package stays mounted as `savedisk:` for the whole session and is never closed (the game never calls
`XamContentClose`). It reads `GlobalSetting` right after opening, a character file when CONTINUE lists or
loads that character. CONTINUE lists the characters of the package; the main menu entry is usable as soon
as the package exists.

**Missing, invalid or damaged data:** see section 4.

## 2. Storage in the port

```
<content root>/                                   --content_root, default: the user data root
  B13EBABEBABEBABE/424507D1/00000001/
      Circle of Doom - Character Save/            the package (a directory)
          GlobalSetting, KD_1350, ...
          .rex-manifest                           size + SHA-256 per file at the last commit
      Circle of Doom - Character Save.rex-staging       the session's working copy (while running)
      Circle of Doom - Character Save.rex-interrupted   uncommitted changes of an interrupted session
  B13EBABEBABEBABE/424507D1/Headers/00000001/Circle of Doom - Character Save.header
```

- **Never in the game folder.** The game folder is mounted read-only; the app refuses to start when the
  content root would be inside it; the sandbox binds the game folder read-only and refuses save
  directories that overlap it.
- **Default location** outside the sandbox: the user data root, `$XDG_DATA_HOME/kuf_cod` (else
  `~/.local/share/kuf_cod`), unless `--content_root` or `--user_data_root` says otherwise.
- **Sandboxed runs** (`tools/run_sandboxed.sh`): `build/runs/<run-id>/saves`, empty at the start of every
  run. `--saves-from <dir>` copies existing saves in (a saves directory or a run directory; the source is
  only read). `--save-dir <dir>` binds a persistent directory read-write, for playing across runs: if it
  holds saves they are first copied to `build/save-backups/<name>-<time>/`, and the run needs an
  interactive yes or `--save-dir-confirm`; inside the repository it must be below `build/` (never tracked).
- `tools/kuf_save.py <saves or run dir>` lists packages, files, the manifest check and leftovers (read-only).

## 3. Transactions (SDK patch 0005) **[V]**

The SDK stored a package as a host directory and the guest wrote into it in place; CREATE_ALWAYS deleted the
whole package before creating it. A crash or kill while the game wrote could leave a torn character file,
and on the console the content of an open package becomes durable only when it is flushed or closed.

Now (`rex/system/xam/content_transaction.h`):

1. Opening a package (create, open, CREATE_ALWAYS on an existing one) mounts a **staging copy**
   (`.rex-staging`) instead of the package; CREATE_ALWAYS/TRUNCATE_EXISTING start it empty but leave the old
   package on disk.
2. `XamContentFlush` (and close/unmount) **commits**: if the staging copy differs from the package, a
   complete copy plus a new manifest is written and fsynced next to the package, then swapped in with
   `renameat2(RENAME_EXCHANGE)` (one atomic step; fallback: two renames, recovered on the next start). An
   unchanged flush does nothing (the game flushes twice per save).
3. On the first access after start, **recovery** handles what a previous session left: a staging copy equal
   to the package (the game never closes its package) is removed; one with differences was never committed
   and is **not applied**: it is moved to `.rex-interrupted` (one per package, for diagnostics) with a
   warning, and the last committed version is used; half-finished commit copies are removed.
4. Opening a package **verifies the manifest**. On a mismatch (`content_damaged=refuse`, the default) the
   open fails with ERROR_FILE_CORRUPT and an `[error]` line naming the file and the problem (missing,
   size, content, unexpected file); `content_damaged=load` opens it anyway with the error logged. Packages
   without a manifest (from before M5B or copied in) are opened and logged as not verified.
5. A package the game deletes (`XamContentDelete`) that fails verification is moved aside to
   `<package>.rex-damaged` (one per package, never loaded) instead of being erased.
6. A new package (none existed) is committed empty at once, so it exists as on the console; a replaced
   package keeps its old version until the new one is committed.
7. `.rex-*` files are runtime bookkeeping and invisible to the game.
8. **One commit per save (K22)**: the app wraps the game's save message (`src/runtime/save_groups.cpp`) in a
   flush group (`ContentManager::BeginFlushGroup`/`EndFlushGroup`): the two flushes of a save are held
   (`[content] flush ...: held until the end of save message 0x41127`) and the package is committed once
   when the message ends (`[content] flush (save message 0x41127) ...: committed (...; changed KD_1350,
   ~GlobalSetting)`). The files and their content are the game's own; only the commit point moves to the
   end of the save, so an interruption anywhere in it keeps the previous complete save, never one step of
   the new one. A device-removed stop after step 1 commits step 1, as the game intends. Off:
   `--kuf_atomic_saves=false` (a commit per flush, as on the console).

Every content call is logged: `[content] XamContentCreate '<name>' as 'savedisk:' flags 0x13 -> open`,
`[content] flush ...: committed (2 files, 6368 bytes)`, recovery notes, damage, the content root at start.

`--content_test_kill=<changed|install>:<n>` (tests only) kills the process with SIGKILL at the n-th commit
that changes data: `changed` = the game's save is complete in staging, nothing committed; `install` = the
new version is complete and synced, right before the swap. `flush:<n>` kills at the start of the n-th
`XamContentFlush` (between the steps of a save when n is its second flush).

## 4. Missing, invalid and damaged saves **[V]**

Measured with copies of a real save, damaged by `tools/menu_test.py` (`damage = [...]`); the original save is
never touched.

| Case | The game alone (no manifest) | With the port's verification (default) |
|---|---|---|
| One byte of the character file changed | **loads it silently** and plays | open refused, `[error] ... is damaged, refusing to open it (content_damaged=refuse): KD_1350: content differs`; the game shows its own message "The save file appears to be corrupted. Press A to select a different storage device or B to delete the corrupted save file and create a new save file." A shows it again; nothing on disk changes |
| Character file cut to half its size (a torn write) | **crashes** while loading (guest access violation reading 0x00000004) | refused as above (`KD_1350: size 3170, expected 6341`) |
| Player answers the corrupted-save message with B (delete) | deletes the package, creates an empty one, main menu with CONTINUE unavailable | the same, but the damaged package is kept aside as `<package>.rex-damaged` (never loaded) |
| Character file missing | main menu, CONTINUE unavailable, no crash, nothing written | the manifest reports `KD_1350: missing`: refused as above (without a manifest: the game's own handling) |
| No package | first start: the game creates one (CREATE_ALWAYS) | the same; the new package exists at once, empty, as on the console |

So the game has no integrity check of its own; the manifest is what stops it from silently loading damaged
data or crashing on a torn file, and the game's existing corrupted-save message is the user-facing result.
A torn file cannot come from an interrupted save any more (section 3); damage now needs something outside
the game (disk errors, partial copies, edits by hand). Saves without a manifest (copied in) are loaded as
before, with an info line.

## 5. Tests

| Test | What |
|---|---|
| `save.*` (Catch2, 15 cases) | transactions on temporary directories: create/open/replace/flush/commit, unchanged commits, recovery of every leftover kind, damage detection (flipped byte, truncation, missing/extra file, bad manifest), damaged packages set aside on delete, SHA-256 vectors, atomic file writes, and a child process committing in a loop killed with SIGKILL 40 times: the package is always exactly one complete version |
| `input.*` script files | `--kuf_input_script_file` parsing (comments, blank lines, `#` in anchors), `repeat ... press` with a guaranteed release |
| `savegame.save_new` | new game, fight, gold/loot, unequip the RB slot, readings, Quit > save |
| `savegame.save_continue` | new process from those saves: CONTINUE; every status value, equipment slot (3 tabs), inventory item compared with the readings before saving; then re-equip and save again |
| `savegame.save_reload` | third process: the second save loads as saved |
| `savegame.save_kill_written` / `save_after_kill_written` | killed after the game wrote its save (before the commit); restart loads stage 1's save, recovery logged, saving works again |
| `savegame.save_kill_commit` / `save_after_kill_commit` | killed in the middle of the commit; the same expectations |
| `savegame.save_kill_between` / `save_after_kill_between` | killed at the save's second flush, after the character file was written and flushed: nothing committed (package byte-identical, the new character file only in the uncommitted copy); restart loads the last complete save, saving works again |
| `savegame.save_kill_between_split` | control: the same kill with `--kuf_atomic_saves=false` commits the character file alone (console behaviour), GlobalSetting stays old |
| `savegame.save_damaged_flip` / `_truncate` | one changed byte / a cut file: refused with the file named, the game's corrupted-save message, the save byte-identical afterwards, nothing created or committed |
| `savegame.save_damaged_delete` | the player chooses delete: new empty package, damaged one kept as `.rex-damaged` |
| `savegame.save_missing_character` | character file and manifest missing: main menu, no crash, nothing written |

Comparisons across processes use the game's own screens: glyph pixels of each value and list row
(`capture_check.py` region_diff `metric = "text"`, `text_threshold`, `align_x`; `prev:<label>` refers to the
earlier run's capture). Measured on these rectangles: identical text at most 0.3, a changed value 2.9 to
11.4; a negative control against a run with other values fails exactly the differing lines.

### Campaign fixtures (M6)
Saves from manual play sessions are copied (read only) to `build/m6-saves/<name>/`, with the inspected captures of an exploration run beside them in `<name>-captures/`. `tests/campaign/*.toml` start from such a copy (`saves_from_dir`; the sandbox copies it again into the run, the fixture is only read) and compare with those captures (`compare_dir`). Both stay outside Git; ctest registers a campaign test only where its fixture exists. The player's own save directory is never used by a test.

## 6. Manual persistent-save test (windowed, sound, controller)

```sh
mkdir -p build/manual-saves                       # dedicated save directory (ignored by Git, not the game folder)
# Session 1: new game or load, play, pick something up, then Options > Quit > save > Yes; close the window
tools/run_sandboxed.sh -t 1800 -n m5b-manual-1 --gpu --window --audio \
  --pad /dev/input/by-id/usb-Microsoft_Controller_30324D4930303333393433333434-event-joystick \
  --save-dir build/manual-saves -- --kuf_exit_after_seconds=1790
python3 -I tools/kuf_save.py build/manual-saves    # read-only: package, files, manifest "ok"
# Session 2: the same directory; the runner backs it up to build/save-backups/ and asks before using it
tools/run_sandboxed.sh -t 1800 -n m5b-manual-2 --gpu --window --audio \
  --pad /dev/input/by-id/usb-Microsoft_Controller_30324D4930303333393433333434-event-joystick \
  --save-dir build/manual-saves -- --kuf_exit_after_seconds=1790
```
Grants: the Wayland socket, the session's PulseAudio socket, one controller node, and `build/manual-saves`
read-write. No network; the game folder is read-only; logs in `build/runs/<id>/` (64 MB x 2); 30-minute
hard limit. `grep '\[content\]' build/runs/m5b-manual-*/kuf_cod.log` shows every save.
