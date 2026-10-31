// Button prompts for the device in use (M7 feature 6, docs/INPUT.md "Button prompts"): the game's texts show
// controller icons with markup (`@(A)Select  @(B)Back`, MENU_ARCHITECTURE 2.6); while the keyboard and mouse are in
// use the icons become the keys bound to those buttons (`[Enter] Select  [Esc] Back`). Pure (tests/input); the
// texts are rewritten as the game looks them up (src/runtime/prompt_text.cpp).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "input/keyboard_pad.h"

namespace kuf::input {

// The pad control an icon stands for ("A", "L", "L_Trigger", "LT", "Start", ...); nullopt for the direction icons
// and anything else.
std::optional<Control> IconControl(std::string_view icon);

// The text shown for an icon with the keyboard, from the bindings of one context: a bound key's menu label in
// brackets ("[Enter]"; Enter preferred when bound, then the first key), the four directions of the D-pad
// (`Dpad_Small`) or a stick (`APad_L_Small`, `APad_R_Small`) as their keys joined ("[Arrows]" for the arrow keys,
// "[WASD]" for single letters); `mouse_look` makes the right stick "[Mouse]" (gameplay). Empty: keep the icon (not a
// button icon, or nothing bound).
std::string IconKeys(std::string_view icon, const Bindings& bindings, Context context, bool mouse_look);

// `text` with each button icon replaced by `replace(icon)` (when not empty) and a space after it when a word follows
// directly. Other markup (`@(offsety=-2)`, `@(scale=...)`) is kept.
std::string RewritePrompts(std::string_view text, const std::function<std::string(std::string_view)>& replace);

// The number of button icons in `text` (the icons IconKeys knows). Two or more is a button help bar
// ("@(A)Select  @(B)Back"): those name the menu's buttons even when looked up in the tick a menu opens.
int CountButtonIcons(std::string_view text);

// A button help bar for the keyboard and mouse (M7 menu overhaul): its entries are separated by two spaces
// ("@(Dpad_Small)Change Slot  @(A)Change Item  ..."); with three or more, the ones for a direction icon (D-pad or a
// stick: moving the selection, obvious with the mouse and the arrow keys) are left out, so long footers fit on one
// line. Other texts are returned unchanged.
std::string ConciseHelpBar(std::string_view text);

// A button help bar's entries ("@(A)Confirm   @(X)Reset   @(B)Back", or as rewritten for the keyboard
// "[Enter] Confirm   [X] Reset   [Esc] Back"): split at each run of two or more spaces, a leading markup prefix
// ("@(scale=...)") kept on every entry. Empty entries are dropped.
std::vector<std::string> SplitHelpBar(std::string_view text);
// The pad control an entry names by its first button icon ("@(X)Reset" -> X); nullopt for keyboard text or none.
std::optional<Control> HelpEntryControl(std::string_view entry);

// The pad button an entry stands for: its button icon, or, rewritten for the keyboard ("[Enter] Change Item"), the
// button whose menu keys (`bindings`, the menu context) the entry starts with. nullopt for a direction or none.
std::optional<Control> EntryControl(std::string_view entry, const Bindings& bindings);
// An entry's visible length (markup left out), for laying entries out side by side.
size_t VisibleLength(std::string_view entry);

// Which prompts to show.
enum class PromptDevice : uint8_t { kController, kKeyboard };

}  // namespace kuf::input
