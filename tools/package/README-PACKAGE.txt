Kingdom Under Fire: Circle of Doom - native PC port (unofficial)

This package does not contain the game or any code of it. You need your own copy of the original Xbox 360 game,
extracted to a folder with an unencrypted default.xex; this package contains no console decryption keys. The native
program is generated and compiled on your machine from that file (a few minutes; nothing is uploaded). The port only
reads that folder.

Install:      ./install.sh --game-dir /path/to/your/game   (the setup AppImage does the same with a window)
Start:        ~/.local/share/kuf-cod-recomp/app/kuf-cod --game-dir /path/to/your/game   (the folder is remembered)
Steam Deck:   see STEAM_DECK.md in the project's documentation (non-Steam game shortcut, controller).
Updates:      ~/.local/share/kuf-cod-recomp/app/kuf-update check | update | rollback  (only when configured)
Bug reports:  ~/.local/share/kuf-cod-recomp/app/kuf-cod --collect-logs  (logs and crash report, no saves)

Saves, settings and logs: ~/.local/share/kuf-cod-recomp/data. The game folder is never written.
Licences: licenses/THIRD-PARTY-NOTICES.txt, or kuf-cod --licenses (this software uses FFmpeg and libmspack, under the
GNU LGPL). Source: the release's -source.tar.xz and -toolchain-sources.tar.
