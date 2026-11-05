// Button prompts for the device in use (M7 feature 6, docs/INPUT.md "Button prompts"): the last device the player
// used (keyboard and mouse, or a controller) picks the prompts; texts the game looks up get the keyboard's keys in
// place of the controller icons (input/button_prompts.h) while the keyboard is in use.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/ppc.h>

#include "input/button_prompts.h"

namespace kuf::prompts {

// An input from that device (the drivers call it; any thread).
void NoteDevice(kuf::input::PromptDevice device);
// The prompts to show now: --kuf_button_prompts=controller|keyboard, or (auto) the last device used; before any
// input, a controller if one is connected.
kuf::input::PromptDevice Current();

// The text lookup sub_826CDBF0 returned `text` (a guest string): the guest address of the same text with keyboard
// prompts, or 0 to keep it (controller prompts, no icon, or not a string).
uint32_t OnTextLookup(PPCContext& ctx, uint8_t* base, uint32_t text);
// The game's message box shows its own answer line ("@(A) Yes @(B) No"): while set, looked-up texts with that line
// get these labels instead (the updater's boxes: "Install Update" / "Cancel"). Clear when the box is answered.
void SetAnswerLabels(std::string a, std::string b);
void ClearAnswerLabels();
// A text the port sets itself (PC Settings' help), for the device in use.
std::string ForCurrentDevice(std::string_view text);

}  // namespace kuf::prompts
