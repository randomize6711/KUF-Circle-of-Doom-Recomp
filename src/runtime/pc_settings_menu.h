// PC Settings in the game's own Settings menus (M6B; src/runtime/pc_settings_menu.cpp). Called from the hooks on
// the menu functions (src/runtime/menu_trace.cpp), which own those hooks.

#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace kuf::pc_menu {

// The in-game options menu's event handler is about to run (ctx holds its arguments). Returns true if the event
// was for the PC Settings row or page and has been handled (ctx.r3 holds the result); false: run the original.
bool BeforeSettingsEvent(PPCContext& ctx, uint8_t* base);
// The title SETTINGS input handler (sub_82678280: menu, event) is about to run. Returns true if the event was
// for the PC Settings row or page and has been handled; false: run the original.
bool BeforeTitleSettingsEvent(PPCContext& ctx, uint8_t* base);
// Game thread, at each tick: ends a Key Bindings / Controller Buttons capture (the key or button pressed, Escape,
// or no input for 10 s) and redraws its page.
void OnTick(PPCContext& ctx, uint8_t* base);
// The menu pointer on a Settings slider (game thread, menu_pointer.cpp OnTick; UX fix 2): a click on a slider's track
// or knob selects its row and sets the value under the pointer; while the button is held the value follows the pointer.
// True while it has the pointer (no hover or confirm then). (0, 0, false, false) ends a drag.
// Also the game's choice widgets (the selector's arrows, the toggles' halves): a click selects the row and gives it
// Left / Right; `moved` (no button): hover over a widget selects its row.
bool PointerSlider(PPCContext& ctx, uint8_t* base, int32_t px, int32_t py, bool click, bool held, bool moved = false);
// The guest address of the PC Settings row's label for its text number, or 0.
uint32_t TextOverride(uint32_t number);

}  // namespace kuf::pc_menu
