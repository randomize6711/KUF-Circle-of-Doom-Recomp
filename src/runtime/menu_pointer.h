// Mouse pointer in the original menus (M7 feature 2, docs/INPUT.md "Mouse"): the focused scene's buttons, read from
// guest memory, and the next step that brings the game's own selection under the pointer (menu_pointer_math.h).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

#include "runtime/menu_pointer_math.h"

namespace rex::memory {
class Memory;
}

namespace kuf::menu_pointer {

// Guest memory for the reads below (ReXApp::OnPostSetup).
void Start(rex::memory::Memory* memory);

// Game thread, at each tick's start (before the tick reads input): hover moves the game's selection to the row under
// the pointer at once, through the focused control's own input handler (the step the game makes for a D-pad press,
// one call per row, all in this tick); a click then confirms with A. PC Settings sliders take clicks and drags
// (pc_settings_menu.cpp PointerSlider).
void OnTick(PPCContext& ctx, uint8_t* base);
// One input event (code: 0 Select, 1 Back, 6-9 Up/Down/Left/Right) to the focused control's handler, as the game's
// input update gives it; false without a focused control.
bool Dispatch(PPCContext& ctx, uint8_t* base, uint32_t code);

// The focused control's scene ID (the layout's, e.g. 0x00F00000 the level-up dialog; 0 without a focused control).
uint32_t FocusedSceneId();
// The control with layout ID `id` in the focused control's scene (read from guest memory; 0 if none).
uint32_t FindInFocusedScene(uint32_t id);

// True if guest memory [address, address + size) is mapped readable (a stale pointer is never followed).
bool GuestReadable(uint32_t address, uint32_t size);
// An input event {code} given to the focused control's own input handler (as the pointer's steps are); false if
// there is no live focused control.
bool DispatchToFocused(PPCContext& ctx, uint8_t* base, uint32_t code);

// Game thread: the plan for a pointer at (px, py) in the 1280 x 720 layout space. `focus` gets the
// focused control (id 0 without a focused button: list boxes and gameplay plan nothing).
Step PlanAt(int32_t px, int32_t py, UiButton* focus = nullptr);

}  // namespace kuf::menu_pointer
