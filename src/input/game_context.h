// Which input context the game is in (keyboard meanings, mouse look or the menu pointer). Pure: tests/input.
//
// The game's input target (game_input_hooks.cpp) is valid exactly while the player character has control, and the
// single-player pause menus invalidate it because they stop the world. In co-op the world keeps running under the
// in-game menus and the target stays valid, so a menu also counts when the UI's focused control belongs to one of the
// game's in-game menu scenes (scene IDs of BsUiControlID.h: SN_STATEMANAGE 0x003, SN_PROFILE_OPTION_LIST 0x013, ...).
// The gameplay overlays that take focus during play (SN_LETTERBOX 0x023, the gauges, the minimap, the remote players'
// panels) do not.
#pragma once

#include <cstdint>

#include "input/game_input_hooks.h"

namespace kuf::input {

// A scene ID as the UI keeps it (0x01300000 for scene 0x013).
bool IsInGameMenuScene(uint32_t scene_id);

// The focused control as read from guest memory.
struct UiFocus {
  bool readable = false;
  uint32_t vtable = 0;    // 0x8203ABF0 (the base class) once the control is destroyed
  uint32_t scene_id = 0;  // its scene's ID
};
// A live control of an in-game menu has the input (not shown is fine: the options menu's focus is its list box, the
// Settings pages' a highlight button).
bool MenuHasFocus(const UiFocus& focus);

GameContext DecideGameContext(bool waiting_for_start, bool has_target, bool menu_focus);

}  // namespace kuf::input
