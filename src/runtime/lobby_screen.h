// The multiplayer menus on the game type screen (lobby_screen.cpp; on by default, --kuf_lobby_menus=false).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ppc.h>

namespace kuf::lobby_screen {

// The menus are on and the game type screen is shown (its co-op gates stay open: the model decides).
bool Active();
// Row 0 shows one of the page's rows (every page but Home): it is not the game's Difficulty value row then.
bool RowZeroIsPageRow();
// A field is being edited: the pointer's hover is not given to the menu; a click goes to ClickWhileEditing (it ends
// the edit, then acts as a click on the row under it).
bool Editing();
void ClickWhileEditing(PPCContext& ctx, uint8_t* base, int32_t x, int32_t y);
// Every tick on the game thread (frame_pacing, with the menu pointer): the screen's drawing, typed text, game steps.
void OnTick(PPCContext& ctx, uint8_t* base);
// The game type screen's layout (UiStartGame.uitb, as read): the label and value columns added when the menus are on.
bool IsLayout(const std::string& lower_case_path);
bool ExtendLayout(std::vector<uint8_t>* data, std::string* why);
// The model's progress every tick (frame_pacing via the co-op runtime's Maintain).
void Tick();

}  // namespace kuf::lobby_screen
