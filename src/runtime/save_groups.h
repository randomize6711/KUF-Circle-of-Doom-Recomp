// The game's saves (src/runtime/save_groups.cpp).
#pragma once

namespace kuf::saves {

// True while the game handles its save message (character file and GlobalSetting); M7 UX fix 6: Quit Game waits.
bool SaveInProgress();

}  // namespace kuf::saves
