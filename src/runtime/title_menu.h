// The title list's own rows (M7 feature 4): Check for Updates and Quit Game below CREDITS (Invite Friend when an invite
// service is available: none yet, docs/SESSION_STATE.md feature 7). src/runtime/title_menu.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <rex/ppc.h>

namespace kuf::title_menu {

// UiTitle.uitb as read (after the PC Settings row was added): the title list's rows added (ui_layout.h
// AddTitleMainRows). Allocates the labels' guest strings the first time. False (and the layout unchanged) on a layout
// that is not as expected.
bool ExtendLayout(PPCContext& ctx, uint8_t* base, std::vector<uint8_t>* layout, std::string* error);

// The guest string for one of the rows' text numbers (0 for any other number).
uint32_t TextOverride(uint32_t number);

// Game thread, at each tick: a label whose text changed (the update check runs on its own thread) is set here.
void OnTick(PPCContext& ctx, uint8_t* base);

// The title list's input sub_826791D0 (menu, event), before the game's (menu_trace.cpp): true when the added rows
// handled it (r3 the result), false to run the game's.
bool BeforeInput(PPCContext& ctx, uint8_t* base);

// The game posts a message (sub_825D5560 / sub_825D5A88: message r3, data r4), before it does: the storage prompt (a
// message box, 0x42205, whose answers are 2003 / 2004) and the notice after it (0x42208) are answered at the next tick
// (--kuf_storage_prompt=auto); a box's answer (0x42206) to the Quit Game question quits.
// Returns true if the post must not happen (the storage notice: not shown at all).
bool BeforePost(PPCContext& ctx, uint8_t* base);
// The UI manager's update (before the tick is drawn; menu_pointer.cpp's arrow hook): answers the storage question
// in the tick its box appears, hidden.
void OnUiUpdate(PPCContext& ctx, uint8_t* base);

// The title menu's event handler sub_8267A5E8 (its scenes' input and messages), before the game's (false: the game's
// runs). --kuf_trace_title_events logs each event's words (diagnostics).
bool BeforeMenuEvent(PPCContext& ctx, uint8_t* base);

// The title menu object (its SETTINGS scene at +44), once the title list had an input; 0 before.
uint32_t Menu();

}  // namespace kuf::title_menu

namespace kuf::app {

// How the port quits as a player closing the window would (kuf_cod_app.h sets it).
void SetQuitHandler(std::function<void()> handler);
bool RequestQuit();

}  // namespace kuf::app
