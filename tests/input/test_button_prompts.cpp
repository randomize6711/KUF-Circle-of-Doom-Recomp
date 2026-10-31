// Button prompts for the keyboard (M7 feature 6): src/input/button_prompts.h.
#include <catch2/catch_test_macros.hpp>

#include "input/button_prompts.h"

using namespace kuf::input;

namespace {
std::string Keyboard(std::string_view text, Context context, bool mouse_look = false) {
  const Bindings b = Bindings::Defaults();
  return RewritePrompts(text, [&](std::string_view icon) { return IconKeys(icon, b, context, mouse_look); });
}
}  // namespace

TEST_CASE("prompts: the menus' help with the default keys", "[prompts]") {
  CHECK(Keyboard("@(Dpad_Small)Move  @(A)Select  @(B)Back", Context::kMenu) ==
        "[Arrows] Move  [Enter] Select  [Esc] Back");
  CHECK(Keyboard("@(L_Trigger)To Left Window  @(Y)View Gamer Card", Context::kMenu) ==
        "[Z] To Left Window  [Y] View Gamer Card");
  CHECK(Keyboard("@(Start) Press START.", Context::kStartPrompt) == "[Enter] Press START.");
}

TEST_CASE("prompts: gameplay keys, sticks and the mouse", "[prompts]") {
  CHECK(Keyboard("Hold @(LT) to switch to Aiming View", Context::kGameplay) == "Hold [Z] to switch to Aiming View");
  CHECK(Keyboard("@(APad_L_Small)Move", Context::kGameplay) == "[WASD] Move");
  CHECK(Keyboard("@(Apad_R_Small)Scroll", Context::kGameplay, true) == "[Mouse] Scroll");
  CHECK(Keyboard("@(Apad_R_Small)Scroll", Context::kMenu) == "[IJKL] Scroll");
  CHECK(Keyboard("@(A)", Context::kGameplay) == "[Mouse L]");  // MouseLeft, Return: in gameplay the first
  CHECK(Keyboard("@(A)", Context::kMenu) == "[Enter]");          // in menus Enter
  CHECK(Keyboard("@(X)", Context::kGameplay) == "[Mouse R]");
}

TEST_CASE("prompts: other markup and unknown icons are kept", "[prompts]") {
  CHECK(Keyboard("@(offsety=-2)Vibration", Context::kMenu) == "@(offsety=-2)Vibration");
  CHECK(Keyboard("@(scale=0.8,0.8)@(A)OK", Context::kMenu) == "@(scale=0.8,0.8)[Enter] OK");
  CHECK(Keyboard("@(TABICON_WA_ON) @(reg) @(ENG)", Context::kMenu) == "@(TABICON_WA_ON) @(reg) @(ENG)");
  CHECK(Keyboard("no markup; @ alone; @(unclosed", Context::kMenu) == "no markup; @ alone; @(unclosed");
  CHECK(Keyboard("", Context::kMenu).empty());
}

TEST_CASE("prompts: rebound keys are shown", "[prompts]") {
  Bindings b = Bindings::Defaults();
  REQUIRE(b.Set(Context::kGameplay, Control::kLeftTrigger, "MouseX1"));
  REQUIRE(b.Set(Context::kGameplay, Control::kLeftStickUp, "Up"));
  const auto r = [&](std::string_view icon) { return IconKeys(icon, b, Context::kGameplay, false); };
  CHECK(RewritePrompts("Hold @(LT)", r) == "Hold [Mouse 4]");
  CHECK(RewritePrompts("@(APad_L_Small)", r) == "[Up/A/S/D]");
  REQUIRE(b.Set(Context::kGameplay, Control::kY, ""));  // unbound: the icon stays
  CHECK(RewritePrompts("@(Y)Pick up", r) == "@(Y)Pick up");
}

TEST_CASE("prompts: button help bars are told from hints", "[prompts]") {
  CHECK(CountButtonIcons("@(Dpad_Small)Move/Adjust  @(B)Back") == 2);
  CHECK(CountButtonIcons("Hold @(LT) to switch to Aiming View") == 1);
  CHECK(CountButtonIcons("@(offsety=-2)Vibration @(reg)") == 0);
}

TEST_CASE("prompts: long help bars drop their direction entries for the keyboard", "[prompts]") {
  CHECK(ConciseHelpBar("@(Dpad_Small)Change Slot  @(A)Change Item  @(X)Auto-equip  @(Y)Details  @(B)Back") ==
        "@(A)Change Item  @(X)Auto-equip  @(Y)Details  @(B)Back");
  CHECK(ConciseHelpBar("@(Apad_R_Small)Scroll  @(A)Use  @(X)Destroy  @(B)Back") == "@(A)Use  @(X)Destroy  @(B)Back");
  // The game's inventory footer: single spaces and a scale in front.
  CHECK(ConciseHelpBar("@(scale=0.6,0.7)@(Dpad_Small)Change Slot @(A)Change Item @(L)Resistances @(R)Inventory @(B)Back") ==
        "@(scale=0.6,0.7)@(A)Change Item  @(L)Resistances  @(R)Inventory  @(B)Back");
  // Short bars and texts without icons stay as they are.
  CHECK(ConciseHelpBar("@(Dpad_Small)Move/Adjust  @(B)Back") == "@(Dpad_Small)Move/Adjust  @(B)Back");
  CHECK(ConciseHelpBar("Hold @(LT) to switch to Aiming View") == "Hold @(LT) to switch to Aiming View");
  CHECK(ConciseHelpBar("@(A)Confirm   @(X)Reset   @(B)Back") == "@(A)Confirm  @(X)Reset  @(B)Back");
}

TEST_CASE("prompts: a help bar splits into its entries, each with the button it names", "[prompts]") {
  using kuf::input::Control;
  const auto level_up = SplitHelpBar("@(A)Confirm   @(X)Reset   @(B)Back");
  REQUIRE(level_up.size() == 3);
  CHECK(level_up[0] == "@(A)Confirm");
  CHECK(level_up[1] == "@(X)Reset");
  CHECK(level_up[2] == "@(B)Back");
  CHECK(HelpEntryControl(level_up[1]) == Control::kX);
  // As rewritten for the keyboard (single spaces inside an entry): the entries, without a known button.
  const auto keys = SplitHelpBar("[Enter] Confirm  [X] Reset  [Esc] Back");
  REQUIRE(keys.size() == 3);
  CHECK(keys[2] == "[Esc] Back");
  CHECK_FALSE(HelpEntryControl(keys[0]));
  // A scale in front is kept on each entry; spaces at the ends dropped.
  const auto scaled = SplitHelpBar("@(scale=0.6,0.7)@(A)Use  @(B)Back  ");
  REQUIRE(scaled.size() == 2);
  CHECK(scaled[0] == "@(scale=0.6,0.7)@(A)Use");
  CHECK(scaled[1] == "@(scale=0.6,0.7)@(B)Back");
  CHECK(HelpEntryControl(scaled[1]) == Control::kB);
  CHECK(SplitHelpBar("").empty());
}

TEST_CASE("prompts: a footer entry's button, from its icon or from the keys it was rewritten to", "[prompts]") {
  using kuf::input::Control;
  const Bindings b = Bindings::Defaults();
  CHECK(EntryControl("@(scale=0.6,0.7)@(Y)Details", b) == Control::kY);
  // Rewritten for the keyboard: the button whose menu keys the entry starts with.
  const std::string a = IconKeys("A", b, Context::kMenu, false), back = IconKeys("B", b, Context::kMenu, false);
  REQUIRE_FALSE(a.empty());
  CHECK(EntryControl("@(scale=0.6,0.7)" + a + " Change Item", b) == Control::kA);
  CHECK(EntryControl(back + " Back", b) == Control::kB);
  CHECK_FALSE(EntryControl("[Arrows] Change Slot", b));
  CHECK_FALSE(EntryControl("Back", b));
  // The visible length: markup left out, an icon about two characters.
  CHECK(VisibleLength("@(scale=0.6,0.7)@(A)Use") == 5);
  CHECK(VisibleLength("[Esc] Back") == 10);
}
