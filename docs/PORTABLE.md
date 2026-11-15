# The portable edition (M16)

**Download, extract, add your game files, launch.** No installer, no administrator rights, no system changes, no
registry entries, no Steam needed. Everything the game writes stays in its folder, which can be moved to another
drive or computer.

```text
KUF-Circle-of-Doom-Portable/
├── Play KUF            (Linux)  /  Play KUF.exe  (Windows)   the launcher
├── GameFiles/           your own game files go here (only read)
│   └── Put your game files here.txt
├── Runtime/             the program: runtime libraries, the code generator, the compiler (builder/), licences
└── Data/                made on first launch: user/ (settings), saves/, cache/, logs/, game/, build-cache/
```

## Using it

1. Extract the archive anywhere (spaces and non-ASCII folder names are fine).
2. Copy your game files into `GameFiles`: the folder with `default.xex` (a retail dump with its encrypted one works as
   it is), or an Xbox 360 disc image (`.iso`); any folder below `GameFiles`. Or drop them onto the launcher's window, or onto the launcher itself.
3. Double-click **Play KUF**. It shows **Game files detected** with *Play*, *Settings*, *Verify Game Files* and
   *Change Game Folder*. With several usable sources it asks which one; with none it explains where they go.
4. The first *Play* prepares the native game (checks and, for a disc image or retail dump, copies the game files
   into `Data/game`; generates and compiles the game's code with the compiler in `Runtime/builder`; a few minutes,
   with progress; Cancel is safe). Later launches start the game at once.

Encrypted retail executables (docs/RETAIL_FILES.md, decision of 2026-11-08 (2)): the launcher decrypts the dump's
`default.xex` in memory and writes it unencrypted into `Data/game` with the other game files; the player's files are
only read. A separate unencrypted `default.xex` beside a retail dump is still paired automatically.

## How it works (`installer/src/core/portable.{h,cpp}`, `installer/src/frontend/launcher.cpp`)

- One launcher program for Linux and Windows (SDL3 and Dear ImGui from the pinned SDK), paths taken from its own
  location (`SDL_GetBasePath`), never from the current folder.
- The game files are checked by the M15.2 code (`core/game_data`); the native game is made by the same local build as
  the setup (`core/localbuild`). Linux: the setup's builder; Windows: llvm-mingw's (docs/WINDOWS.md).
- `Data/portable.txt` records the source, the game folder, the executable's image SHA-256, the package's generation
  id and the program's SHA-256, with paths relative to the folder. A launch is direct when all match; otherwise the
  launcher says why (other game files, a new program version, a damaged program) and prepares again.
- The game runs with `--user_data_root`, `--content_root` (saves), `--cache_root` and `--log_file` inside `Data`.
  Saves are never overwritten by the launcher.
- A folder that cannot be written (read-only media): the launcher asks for a writable folder for `Data` and
  remembers it per portable folder in the user's configuration folder (the only file written outside it). When
  `Runtime/bin` alone cannot be written, the program and its libraries go into `Data/bin`.

## Tested (2026-11-08, Linux; Windows: not run yet)

- Unit tests (`installer/tests/test_portable.cpp`): sources and pairing, first and later launches, moving the folder
  to a Unicode path with spaces, a damaged program, a new package version, a failed build (the old program and record
  kept), a disc image, a read-only program folder, saves kept, Verify Game Files.
- A real portable folder from the 0.8.0-rc.3 package and a copy of the test game files, in a path with spaces and
  `Ü`: detection, first-launch preparation (the real local build), the game started from the launcher's command line
  (sandboxed, headless, 30 s, exit 0, its logs and run summary written only inside `Data`), the Vulkan menu scenario
  on this machine's GPU (passed), the folder moved to a Cyrillic path: ready at once, no new preparation.
- The window at Steam Deck size (1280x800, SDL's offscreen driver): every screen fits.
