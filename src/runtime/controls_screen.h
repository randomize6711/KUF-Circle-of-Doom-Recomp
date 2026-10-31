// The in-game Controls screen for the device in use (M7 UI polish, docs/INPUT.md "Button prompts"): with a controller the
// game's own page (the Xbox 360 controller and its button icons); with the keyboard and mouse a list of the bound keys
// in front of each action, without the controller picture. src/runtime/controls_screen.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace kuf::controls_screen {

// The in-game options menu object (its scenes: Settings at +44, Controls among the others), seen by its event handler.
void NoteOptionsMenu(uint32_t menu);

// Game thread, at each tick: while the Controls page shows, its layout for the device in use (and the keys bound now).
void OnTick(PPCContext& ctx, uint8_t* base);

}  // namespace kuf::controls_screen
