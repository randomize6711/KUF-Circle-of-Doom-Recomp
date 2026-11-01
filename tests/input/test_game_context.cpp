// Which input context the game is in (src/input/game_context.cpp): the target alone in single-player, the focused
// in-game menu too in co-op (the world keeps running under the menus).
#include <catch2/catch_test_macros.hpp>

#include "input/game_context.h"

using kuf::input::DecideGameContext;
using kuf::input::GameContext;
using kuf::input::IsInGameMenuScene;
using kuf::input::MenuHasFocus;
using kuf::input::UiFocus;

namespace {
constexpr uint32_t kButtonVtable = 0x8203AD08, kDestroyed = 0x8203ABF0;
UiFocus Focus(uint32_t scene, uint32_t vtable = kButtonVtable) { return {true, vtable, scene}; }
}  // namespace

TEST_CASE("in-game menu scenes: menus and dialogs count, gameplay overlays do not", "[input][context]") {
  for (uint32_t menu : {0x00300000u, 0x00400000u, 0x01300000u, 0x01500000u, 0x02500000u, 0x02700000u, 0x02800000u,
                        0x03300000u, 0x00F00000u, 0x00E00000u, 0x01000000u}) {
    CHECK(IsInGameMenuScene(menu));
  }
  // The letterbox took the focus during single-player gameplay; the HUD, minimap, remote players' panels.
  for (uint32_t overlay : {0x02300000u, 0x00600000u, 0x00700000u, 0x02200000u, 0x00A00000u, 0x03000000u, 0x01900000u,
                           0x01100000u, 0x00500000u}) {
    CHECK_FALSE(IsInGameMenuScene(overlay));
  }
  CHECK_FALSE(IsInGameMenuScene(0x01300001));  // a control ID, not a scene ID
  CHECK_FALSE(IsInGameMenuScene(0));
}

TEST_CASE("a menu has the focus only through a live control", "[input][context]") {
  CHECK(MenuHasFocus(Focus(0x01300000)));
  CHECK_FALSE(MenuHasFocus(Focus(0x01300000, kDestroyed)));  // the menu closed: its controls fall back to the base
  CHECK_FALSE(MenuHasFocus(Focus(0x01300000, 0)));
  CHECK_FALSE(MenuHasFocus(UiFocus{}));                      // nothing readable
  CHECK_FALSE(MenuHasFocus(Focus(0x02300000)));              // the letterbox
}

TEST_CASE("the context: single-player pause, co-op menus, the start prompt", "[input][context]") {
  // Single-player: the pause menu stops the world and the target goes invalid.
  CHECK(DecideGameContext(false, true, false) == GameContext::kGameplay);
  CHECK(DecideGameContext(false, false, false) == GameContext::kMenu);
  CHECK(DecideGameContext(false, false, true) == GameContext::kMenu);
  // Co-op: the target stays valid under the menu; the focused menu decides.
  CHECK(DecideGameContext(false, true, true) == GameContext::kMenu);
  CHECK(DecideGameContext(true, true, true) == GameContext::kStartPrompt);
}
