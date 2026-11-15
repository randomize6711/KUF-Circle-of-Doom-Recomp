# Installing Kingdom Under Fire: Circle of Doom on Linux

You need:

- your own copy of the original Xbox 360 game files: an extracted retail folder or a disc image (`.iso`);
- a 64-bit Linux desktop new enough for this build (the setup tells you if yours is not), a Vulkan-capable graphics
  driver, and about 5.5 GB of free space (about 4.5 GB stays in use).

You do not need a compiler or any other developer tools: the setup carries what it needs and installs nothing on your
system.

## Install

1. Download `KingdomUnderFire-CircleOfDoom-Setup-<version>-x86_64.AppImage`.
2. Make it executable (file manager: right-click > Properties > Permissions > "Allow executing", or
   `chmod +x KingdomUnderFire-CircleOfDoom-Setup-*.AppImage`) and open it.
3. **Game Files:** choose the folder with `default.xex` (**Browse…**), or your disc image or that `default.xex` itself
   (**File…**). The setup checks that every file is there and that the game is the supported version. Your files are
   only read. A retail copy works as it is: its encrypted `default.xex` is decrypted into the installation
   (docs/RETAIL_FILES.md).
4. **Location:** keep `~/Games/Kingdom Under Fire - Circle of Doom` or choose another folder. No administrator
   password is needed.
5. **Options:** an application-menu shortcut (on by default), a desktop shortcut, and "Add to Steam as a non-Steam
   game" (close Steam first; with several Steam accounts you choose one).
6. **Install:** the game files are copied and checked, then the native game is generated and compiled on your computer
   from your files (a few minutes in all; about 15 minutes of processor time, shared over its cores; Cancel is safe, and
   an interrupted attempt continues where it stopped). Then **Launch Game** or **Finish**. Nothing is uploaded.

Start the game from the application menu, the desktop shortcut or Steam. Your saves and settings are in
`~/.local/share/kuf-cod-recomp/data` and are kept when you reinstall or update.

## Repair, reinstall or uninstall

Open the setup again: it finds the installation and offers **Repair or Reinstall** and **Uninstall**. Uninstalling is
also in the application menu ("Uninstall Kingdom Under Fire: Circle of Doom"). It removes the game and the shortcuts it
made; your saves and settings are kept unless you tick that option and confirm it.

## If something goes wrong

- *"Retail copy recognised, but its encrypted default.xex could not be decrypted"*: the file is probably damaged; an
  unencrypted `default.xex` of the same version can be given in the second field.
- *"This default.xex was decrypted, but its content differs"*: the
  copy is modified or damaged; make a new dump.
- *"Unsupported game version"*: only the original release (base version 0.0.0.1, media ID 7092B5EC, without title
  update) is supported. The message names what was found instead: another game's title ID, another version or
  release, or a title update.
- *"The disc image is damaged"*: the image's file system is inconsistent (the message says how); make a new dump.
- *"Steam is running"*: quit Steam completely (Steam > Exit) and choose Retry.
- A desktop icon that does not start: some desktops ask first; right-click it and choose "Allow Launching".
- Logs for a bug report: `~/.local/share/kuf-cod-recomp/data/logs/`.

This is an unofficial fan project, not affiliated with or endorsed by the game's publishers.
