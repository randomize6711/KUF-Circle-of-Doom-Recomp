# Engine Analysis — Kingdom Under Fire: Circle of Doom (Xbox 360)

Status: M0, 2026-10-20. All findings below come from read-only inspection of the retail files in
the game folder (never modified). Analysis scripts live in
the session scratchpad (`tools/xexinfo.py`, `tools/xeximage.py`, `tools/soxindex.py`) and will move into the
workspace's `tools/` once it is approved.

Legend: **[V]** verified from the files · **[H]** reasonable hypothesis · **[?]** unknown.

## 1. Disc layout

The game is already extracted (a loose folder, not an ISO/GOD container). 138 files, 4.1 GB.

| Path | Size | Content |
|---|---|---|
| `default.xex` | 11.3 MB | Game executable (XEX2) **[V]** |
| `*.pak` + `*.sox` (11 pairs) | 3.6 GB | Asset archives + their index tables **[V]** |
| `dependency.bin` | 0.9 MB | Per-asset dependency lists (model → textures, etc.) **[V]** |
| `Data/Material/CodShaders.ccz` | 3.1 MB | zlib bundle of 342 precompiled FXLite effects **[V]** |
| `Data/Sound/SoundRes/*.xwb/*.xsb` | ~1.2 GB | XACT wave/sound banks: 6 character voice sets × 9 languages + BGM **[V]** |
| `Data/Title/*.wmv` | 128 MB | Attract + title-loop movies (ASF/WMV) **[V]** |
| `$SystemUpdate/su20076000_00000000` | 7.3 MB | Dashboard system update — not needed **[V]** |

Archive groups: `initres` (boot), `preloadres`, `mainres` (729 MB), `robbyres` ("lobby" UI/resources),
and one `bg_XX` per world (cl, cu, dr, du, en, lh, rg).

## 2. Executable

**[V]** XEX2, retail AES-encrypted, *basic* (non-LZX) compression; decrypts with the public retail key to a
valid PE image (`MZ`). Original PE name `CircleOfDoom.exe`, title ID `0x424507D1`, media ID `0x7092B5EC`,
version 1 (no title update in the folder), all regions, built with XDK **2.0.5632** (static libs) against
kernel/XAM **2.0.5766**.

| Property | Value |
|---|---|
| Image base / size | `0x82000000` / `0xB80000` |
| Entry point | `0x826F49A8` |
| `.text` | `0x82150000`, 0x8CE084 bytes (~9.2 MB) |
| Extra code sections | 8 × `.embsec_` at `0x82A1E200`–`0x82A71150` (~0.25 MB total) |
| `.pdata` | 22,650 function entries; covers 97 % of `.text` and nearly all `.embsec_` |
| `.data` | `0x82A80000`, 0x702BC |
| TLS | 64 slots, no TLS data |
| Default stack | 256 KB |

Instruction profile across all code (2.39 M words) **[V]**: ~66 k VMX/VMX128 words (opcodes 4/5/6, ~2.8 %),
19.8 k `bctr`/`bctrl` (indirect calls + switch tables), 8,007 `twi` (trap assertions/debug breaks), 336/486
`lwarx`/`stwcx.` (atomics), 86 `dcbz`, 16 `mftb`. Profile is typical of a 2007 LTCG 360 title; nothing exotic.

### Static libraries (all 2.0.5632)
XAPILIB, D3DX9, XBOXKRNL, XMP, X3DAUD, XMEDIA, LIBCPMT, **VCOMP (OpenMP)**, XGRAPHC, XNET, XONLINE, XHV,
D3D9LTCG, XAUDLTCG, XACTLTCG.

### Imports **[V]** (222 unique: xboxkrnl + xam)
- Threads/sync: `ExCreateThread`, `Ke*`/`Nt*` events, semaphores, timers, waits, critical sections, spinlocks,
  TLS, `KeSetAffinityThread`, `NtSuspend/ResumeThread`, SLists.
- Memory: `MmAllocatePhysicalMemoryEx`, `NtAllocateVirtualMemory`, `MmGetPhysicalAddress`, `MmQuery*`.
- Files: `NtCreateFile`/`NtOpenFile`/`NtReadFile`/`NtReadFileScatter`/`NtWriteFile`/`NtQueryDirectoryFile`,
  `ObCreateSymbolicLink`.
- GPU: full `Vd*` set (`VdInitializeRingBuffer`, `VdSwap`, `VdGetSystemCommandBuffer`,
  `VdSetGraphicsInterruptCallback`, EDRAM retrain, …) → guest D3D9 builds PM4 command buffers itself.
- Audio: `XAudioRegisterRenderDriverClient`/`XAudioSubmitRenderDriverFrame`, `XMACreateContext`.
- XAM: input (`XamInputGetState/SetState/GetCapabilities`), users/profiles, content (save) APIs, UI
  (sign-in, device selector, message box, friends, gamercard, marketplace, player review), `XamSession*`,
  `XMsg*` (async XSession/XUser calls), voice (`XamVoice*`), notifications, `XamLoaderLaunchTitle`.
- Network: `NetDll_*` Winsock (socket/bind/connect/listen/accept/send/recv/sendto/recvfrom/select) plus
  `XNetStartup`, `XNetGetTitleXnAddr`, `XNetXnAddrToInAddr`, `XNetInAddrToXnAddr`, `XNetQos*`, DNS.
- Misc: `XexLoadImage`/`XexGetProcedureAddress` (dynamic module load — target still unknown **[?]**).

## 3. Engine and middleware

| Component | Finding |
|---|---|
| Engine | **[V]** Blueside in-house engine. Source paths `d:\project\circleofdoom\cfmtlib\…` (CFThreadBase, CThreadSafeQueue, CFKernelObject); asset headers "BlueSide FX Export File", "BlueSide UI Export File"; main class `CCircleOfDoom`. |
| Physics | **[V]** NVIDIA/AGEIA **PhysX 2.7.2** (NovodeX), statically linked (cloth, soft body, fluids, joints present). |
| Scripting | **[V]** **Lua 5.1.1**; 120 `.lua` files ship as **plain-text source** (defs, events, coroutines). The engine exposes ~100+ script functions (`AmITheHost`, `SaveGame`, `JumpToLevel`, `SetNetworkGlobalFlag`, …). |
| Graphics | **[V]** D3D9 (Xenos) + D3DX9 + FXLite effects; XGraphics shader compiler/assembler libraries are linked in (see §5). |
| UI | **[V]** Custom Blueside UI (`.uitb`, "BlueSide UI Export File"); no XUI. |
| Audio | **[V]** XACT (`COD_SOUND.xgs`, `.xsb`/`.xwb` banks, XMA contexts), X3DAudio, XMP (custom soundtrack). |
| Video | **[V]** WMV via XMEDIA — the WMV9 decoder is guest code with its own Xenos shaders (`codecs\wmv\…\Shader_Deblock*.updb`). |
| Threading | **[V]** CFThreadBase threads + OpenMP (VCOMP). **[H]** OpenMP used for parallel loops in rendering or animation. |
| Debug console | **[V]** Strings for console commands remain (`showfps`, `pingcheck`, `saveallact`, `savecam`, `fogblend`, …). Useful for testing **[H]**. |

No Havok, Bink, Scaleform, FMOD/Miles/Wwise. A lone "fmod" string hit is a C math symbol **[H]**.

## 4. Asset formats

`.sox` **[V]**: big-endian typed table. Header `u32 version=1, u32 columns, u32 rows`; columns are
`char name[64] + u32 type` (1 = length-prefixed string, 3 = u32); a zero-filled region follows (likely a hash
index **[?]**), then rows of `u32 id` + values. Archive indexes have columns
`Path, Pak, Offset, CompressedSize, UncompressedSize`. Gameplay tables (`D:\Data\SOX\*.sox`, 29 files) use
the same container.

`.pak` **[V]**: concatenated raw zlib streams aligned to 0x800, no encryption. All 23,752 indexed entries
were listed, and 40 random samples inflated to the exact expected size.

Content by type (uncompressed) **[V]**:

| Ext | Files | MB | Meaning |
|---|---|---|---|
| `.36t` | 10,888 | 3,152 | Textures (Xenos-tiled texture header + data **[H]**) |
| `.bm` | 3,047 | 1,543 | Meshes |
| `.bvl` | 702 | 994 | Level geometry/visibility chunks |
| `.chunk` | 1,376 | 348 | Level data (`_ambient`, `_basic`, `_navi`, `_destruct`) |
| `.skin`, `.bpf`, `.bfx`, `.bs`, `.ba`, `.fxa`, `.bb`, `.as`, `.cam` | — | — | Skins/material bindings (reference `CommonShader.fx`), particle FX, skeletons, animations, cameras **[H]** |
| `.lua`, `.txt`, `.h`, `.def`, `.set` | — | — | Plain-text scripts/configs (Korean comments in CP949) |
| `.xsb`/`.xwb` | 106 | 53 | Sound effect banks inside the paks |
| `.vsc`, `.uitb` | — | — | Localized text/UI layouts |

Levels are built from `.bvl`/`.chunk` pieces per world (random map generation is a game feature **[H]**:
the store text says "randomly generated maps").

## 5. Shaders

**[V]** `CodShaders.ccz`: `u32 uncompressed_size`, padding to 0x800, then one zlib stream (23.6 MB). Inside:
`u32 count=342`, then 342 × {`char name[0x104]`, `u32 offset`, `u32 size`}, then the data. Entries are
compiled FXLite effects (magic `A3 D7 01 41`). There are 154 `CommonShader_<feature-bitmask>.fx` permutations
plus 188 named effects (water, lava, IBL, bloom, DOF, motion blur, shadow, UI, …). They contain parameter
names, `vs_3_0`/`ps_3_0` profiles and **4,143 embedded Xenos microcode containers** (`0x102A1100` = 2,737,
`0x102A1101` = 1,402) — the input format XenosRecomp scans for. Duplicates are expected.

**[V]** The executable also links the XGraphics microcode assembler, HLSL compiler and shader state manager
(`ucode\compiler`, `ssm\*`, "alternate cases for 'technique' are deprecated in strict mode") and references
`D:\Data\Material\LineCurve.fx` / `SimpleTrail.fx` by path. **[H]** Some effects may be compiled at runtime
from source, or the library is just linked in by D3DX. If any are, offline translation must be supplemented
with runtime capture or translation (see TECHNICAL_RISKS R3).

Other embedded microcode: WMV decoder shaders (XMEDIA), D3D internal clear/resolve shaders **[H]**.

## 6. Timing **[H]**

Strings `GetElapsedSec`, `GetElapsedTick`, `deltaTime`, `elapsedtime`, `SPperTick`, `FPS : %.2f`, and
PhysX `Scene::simulate` (fixed- or variable-step depending on scene descriptor). The game refuses PAL-50
(60 Hz display required), which suggests vsync-locked 30 fps. **[V, external]** Xenia's compatibility
tracker labels the title `vsync-off-speedup`: the game runs faster when vsync is disabled, so at least part of
gameplay advances per frame, not per second. Which systems do so (animation, AI, PhysX step, camera, Lua
coroutines) is **[?]** and is the key M8 question.

Title update: **[V, external]** at least one TU exists (Xenia reports it fixes slow camera movement and some
shading/blur). The supplied files are the base version 1, with no TU. Including the TU would require the user
to supply it (it is a delta patch, XEX `DELTA_PATCH` format).

## 7. Saves and profiles **[V]**

M5B, measured (details in docs/SAVES.md): **one** content package of type 1 for all characters,
`Circle of Doom - Character Save` (display name "KUF:Circle of Doom-Character Save"; the earlier guess of one
package per character was wrong), holding `GlobalSetting` and one file per character (`KD_1350`, ...).
Found with `XamContentCreateEnumerator` after the storage prompt; opened with `XamContentCreateEx` flags
0x13 (OPEN_EXISTING) from 0x827EB260, or created with 0x812 (CREATE_ALWAYS) from 0x826B1E30 via 0x827EB090
when none exists; mounted as `savedisk:` for the whole session, never closed; every save is a rewrite of the
files plus `XamContentFlush`. Saves at character creation, world load, end of tutorial, sometimes during play,
and on Quit > "save and return to the Title Screen". Character file: `[PLRDATA` at 0x290, EXP int32 at
0x2A4, gold int32 at 0x500 (big-endian); current HP is not saved. The game does not validate saves (a
changed byte loads silently, a truncated file crashes it). Thumbnails `COD_SAVEDATA_<char>.dds` and the
script hooks `SaveGame`, `SaveSetting`, `SaveGlobalSetting`, `LoadSetting` exist (not traced).

## 8. Multiplayer **[V]/[H]**

**[V]** Up to 4-player online co-op (store text: "seamless 4 player co-op … join … at any time"). The
implementation uses XNet secure sockets (`NetDll_*`, `XNetXnAddrToInAddr`), XSession (via `XamSession*` +
`XMsg*`) and QoS probing; voice uses XHV. Lua functions `AmITheHost`, `Get/SetNetworkGlobalFlag`,
`ReqMigrateNewAct`, `IsAllInArea`; console command `pingcheck`; a separate `robbyres` (lobby) archive.

**[H]** Host-authoritative over a single host-relayed session, with drop-in joining. **[V, external]**
Co-Optimus/co-op.gg list *no System Link and no split-screen*. Online co-op only; joining players keep
the XP and items they earn on their offline character, and enemies scale with player count. The `LAN_KEY` XEX
header is present anyway; it is generated by the toolchain and does not by itself prove LAN support. Native LAN therefore means making the
*Xbox Live* code path run over a local transport, not reusing an existing System Link mode. The wire protocol, sync model and host migration are **[?]**; answering them
needs disassembly in M11 prep.

## 9. Identified functions (M1, v1 addresses)

| Address | Function | How identified |
|---|---|---|
| `0x8224C420` | zlib 1.2.3 `adler32` | BASE 65521 constant, NMAX loop; verified bit-exact by `kuf_image_tests` |
| `0x8224C760` | zlib `crc32` (big-endian table path, tables at `0x820E1938`) | CRC table reference; verified |
| `0x82269BE8` | engine table CRC (init 0, no final xor; table at `0x82AB9E50` in `.data`) | verified |
| `0x828ee4f0`, `0x828F9CD8` | second zlib copy (crc32/adler32), presumably inside XDK/XMedia code | same constants; not tested |
| `0x82A184F0` | CRT `setjmp` (jmp_buf: f14–f31 @0, r1 @144, r13–r31 @152, CR @304, LR @308, unwind flag @312) | M2, configured |
| `0x828008C0` | CRT `longjmp` (unwind-flag path through `RtlUnwind`) | M2, configured |
| `0x8280E58C` | Context restore + jump (used by the unwinder; restores FPSCR/CR/XER/r1 from a CONTEXT) | M2 (the M1 "longjmp candidate") |
| `0x82808510` | FPU control-word helper (`mffs`/`mtfsf`), not `setjmp` | M2 |
| `0x82828398` | Static `RaiseException(code, flags, nargs, args)` → `RtlRaiseException` | M2, guarded |
| `0x82828530` | Static `GetFileAttributesA` (via `NtQueryFullAttributesFile`) | M2 |
| `0x826F38B8` | Static `CreateFileA`, main copy (`NtCreateFile` through XAPI's I/O function table) | M2, traced |
| `0x823E7168` | Static `CreateFileA`, second copy (direct `NtCreateFile`) | M2, traced |
| `0x8216E1B0` | D3D present path, the only `VdSwap` call site | M2, frame counter |
| `0x82160C90` | D3D device poll with `db16cyc` spin-wait (render thread) | M2 |
| `0x828E6438`, `0x828570E0`, `0x82855214` | XGraphics shader compiler internals (fault site, `setjmp` caller, `longjmp` caller) | M2 |
| `0x821F89C8–0x821F8A18` | run of `lwz/lwz/mtctr/bctr` virtual-call thunks passed as callbacks | M1 runtime abort, fixed |
| `0x82A08D40` | COM-style method on the audio path (returns `0x80070057` E_INVALIDARG or tail-calls `0x82A08CB8`) | M1 runtime abort, fixed |

Runtime observations (sandboxed, M1):
- With GPU emulation enabled, the game builds about 40 graphics pipelines from its own Xenos shaders on
  several threads before the first fault.
- It probes `ShaderDumpxe:\CompareBackEnds`, a debug device ReXGlue does not map (harmless).

## 10. Boot sequence and threads (M2, sandboxed runs)

1. ~0.45 s: CRT/XAPI init, TLS, critical sections, 2 XAM notification listeners, profile settings read,
   XAudio render client registered, `NtCreateFile` begins.
2. ~0.9 s: `dependency.bin`, then each `.sox` index with its `.pak`: `initres`, `preloadres`, `mainres`,
   `robbyres`, and the world archives `bg_cl`, `bg_cu`, `bg_du`, `bg_lh`, `bg_rg`, `bg_en` (`bg_dr` is not
   opened at boot). Then `CodShaders.ccz` and the BGM banks `71_BGM.xwb`/`.xsb`.
3. ~1.0 s: D3D device and GPU ring buffer set up, first `VdSwap`. The XGraphics runtime shader compiler
   runs (and uses `setjmp`/`longjmp` error recovery at least once).
4. ~10.9 s: `XMsgStartIORequest` to XGI (rich presence context), XMA contexts created, then
   `COD_TITLE_LOOP.wmv` and later `COD_ATTRACT.wmv`: the title screen / attract loop.
5. Steady state: 30 fps presents (`VdSwap` every 33.3 ms), 35,700 `XamInputGetState` polls in 300 s, and
   `XNotifyGetNext` polling.

Guest threads at 45 s (21 in total): the main thread (waits on the game thread), the render thread
(D3D, running), 6 identical event-waiting workers in `sub_828274D0` (six workers for six hardware
threads, presumably the OpenMP/VCOMP pool; inference), 2 "Job Thread"s, 2 self-suspended threads,
2 GPU/D3D helper threads, audio threads. Runtime host threads: GPU command processor, VSync, XMA decoder,
audio worker, Vulkan pipeline compilers, disk I/O.

## 11. Input (M4, v1 addresses) **[V]**

- Polling: `sub_825EAD58`, once per frame, calls `XamInputGetState` for users 0-3 (call site `0x825EADB0`)
  and `XamInputSetState` (vibration) per user (`0x826F3CE8` caller). Nothing else reads input.
- Edge detection: per user, a hold counter per button for 14 masks from the table at `0x82034EB0`
  (A, B, X, Y, LB, RB, D-pad up, down, left, right, Start, Back, LS, RS) and both triggers (byte > 30, events
  14/15). A counter reaching 1 calls the handler (`vtable+20` of the input object) with the event index; a
  release calls it with `index | 0x80000000`.
- "Press START": byte +48 of the input object (its `r3`) is set while the game waits for a START press to
  pick the controller; the first START press stores the user at +44 and clears +48. Set again when the
  title screen returns (e.g. B from character select). Only Start passes the title screen; A in the same
  frame as Start blocks it.
- Input routing: presses go out as messages (0x40000, `sub_825EB570` -> `sub_8245FB58`) to each user's
  input-target handle (8 bytes at input object +8*(user+1)). User 0's handle is valid (e.g.
  `00010005:40E98230`, the player-controller object) exactly while the player character has control, and
  `FFFFFFFF` on the title screen, in menus, dialogs' opening animations and the in-game status/options
  menus. The port uses it as its gameplay/menu signal (M5A).
- Menus: the game's own menus auto-repeat a held D-pad/stick, skip greyed-out items (CONTINUE and
  LEADERBOARD without a save) and read both the D-pad and the left stick. Main-menu object: vtable
  `0x8203E658`, 7 item pointers at +0x20, selected index at +0x70; items (vtable `0x8203BD28`) hold no screen
  coordinates.
- Screens reached with input (M4): title ("Press START") → main menu with the game's storage-device prompt
  ("Do you want to select a storage device now?", A Yes / B No; Yes → `XamShowDeviceSelectorUI`, answered by
  the runtime, then "progress will not be saved if the storage device is removed", A Confirm) → main menu
  (NEW GAME, CONTINUE, LEADERBOARD, SETTINGS, LANGUAGE, PROLOGUE, CREDITS) → SETTINGS (volumes, brightness,
  skin, camera, blood, vibration, change storage device), LANGUAGE (8 languages), NEW GAME → character
  selection (Kendal, Regnier, ...; "Play as this character?"). No separate profile screen: the runtime's
  offline profile (user 0, signed in locally) is used.

## 12. First playable world (M5A) **[V]**

- New game: character selection -> "Play as this character?" -> (offline) "You cannot play co-op ..." ->
  game type "Begin Solo Game" -> saving -> loading -> in-engine intro cutscene (skippable with A/Start) ->
  title card "Forest of Embracing" -> play, with a tutorial (hint boxes with Confirm/Back; Back offers "End
  tutorial?"). The character save is written to the selected storage device (in sandbox runs: the run's user
  folder, `B13EBABEBABEBABE/424507D1/...`).
- The game's Controls page (options menu -> Controls): LT aiming view, RT ability, LB/RB item (hold:
  inventory), Y pick up / detection, B ability, X weapon, A weapon, Start status menu, Back options menu,
  right stick camera, left stick move, D-pad co-op ally targeting.
- Status menu (Start): HP, Luck, EXP, SP, SP recovery, Gold; tabs Weapons & Abilities (A/X weapon, B ability
  Dash, RT ability), footer Change Slot, Change Item, Auto-equip (X), Details (Y), Resistances (LB),
  Inventory (RB). Inventory items show attack power, SP use, recovery and synthesis chance ("86% chance of
  successful synthesis. Has never been synthesized"). Options menu (Back): Sleep, Change Regions, View Quests,
  Controls, Settings, Quit.
- Starting equipment stats are rolled per new game (e.g. Heater Shield ATK 2-3, Mace ATK 4-5).
- First fight (tutorial route): Red Lizardman Warrior/Soldier/Archer Lv. 1 groups near the ruins. Kills give
  EXP and gold; drops are picked up with Y ("Iron Ring - (Y) Pick up item."): Mace, Siege Bow of Puncture,
  Iron Ring, Small Red Potion seen.
- Camera: the right stick has a dead zone of about 12,900 (39 %); above it the turn rate is roughly linear,
  up to about 39 screen px per frame at full deflection, with a short ease-in.
