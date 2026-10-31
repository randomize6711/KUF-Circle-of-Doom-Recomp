// The mouse in the game's own dialogs beyond row selection (M7 menu overhaul; src/runtime/menu_mouse.cpp): the
// level-up dialog (its panel more opaque over a dimmed backdrop, the value arrows clickable and repeating while held,
// Confirm / Reset / Back as three clickable entries with the keyboard) and the inventory's category tabs. Every
// action is the game's own input (a pad press), so the game's rules decide what it does.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rex/ppc.h>

namespace kuf::menu_mouse {

// A menu layout file the game has just read (lower-case name, as the file hook sees it): true if `data` was changed
// (the level-up dialog's backdrop and panel; the footer entries of the level-up dialog, the equipment screen and the
// item list, whichever of their scenes the file has); `why` says why not when a change failed.
bool ExtendLayout(std::string_view name, std::vector<uint8_t>* data, std::string* why);
// The files ExtendLayout looks at (every menu layout, *.uitb).
bool IsExtendedLayout(std::string_view lower_name);

// Game thread, at each tick (menu_pointer.cpp OnTick): the level-up footer for the device in use.
void OnTick(PPCContext& ctx, uint8_t* base);

// The menu pointer (game thread, before the row hover): true if a click or a held press here was taken (no hover
// step or confirm then). (held false, click false) ends a repeat.
bool Pointer(PPCContext& ctx, uint8_t* base, int32_t px, int32_t py, bool moved, bool click, bool held);
// The status menu's object, from its event handler (the equipment screen's pointer).
void NoteStatusMenu(uint32_t menu);

}  // namespace kuf::menu_mouse
