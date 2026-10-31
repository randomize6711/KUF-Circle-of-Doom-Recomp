// The game's input-layer state, read from its per-frame input update (game_input_hooks.cpp).
#pragma once

#include <cstdint>

namespace kuf::input {

enum class GameContext : uint8_t {
  kMenu,         // menus, dialogs, in-game status/options menus (pause)
  kStartPrompt,  // the game waits for a START press to pick the active controller ("Press START")
  kGameplay,     // the player character has control
};

GameContext CurrentGameContext();
// The same, read from the game's input object now rather than at its last input update (the button prompts: a menu
// built in the tick it opens already counts as a menu).
GameContext LiveGameContext(const uint8_t* base);

// True while the game waits for a START press ("Press START").
bool GameWaitsForStart();

}  // namespace kuf::input
