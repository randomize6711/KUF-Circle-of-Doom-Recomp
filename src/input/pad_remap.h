// Controller buttons in gameplay (M7 feature 5, docs/INPUT.md "Rebinding"): which physical button acts as each of the
// game's buttons. A permutation of the twelve buttons the game's actions use (the D-pad and the sticks stay as they
// are), applied to a controller's state only while the player has control (menus keep A confirm, B back). Pure:
// tests/input. The driver that applies it wraps the SDK's SDL driver (src/input/pad_remap_driver.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "input/keyboard_pad.h"

namespace kuf::input {

enum class PadButton : uint8_t {
  kA, kB, kX, kY, kLeftShoulder, kRightShoulder, kLeftTrigger, kRightTrigger, kBack, kStart, kLeftThumb, kRightThumb,
  kCount,
};
constexpr size_t kPadButtonCount = static_cast<size_t>(PadButton::kCount);

// As in pc_settings.toml and ControlName ("a", "left_shoulder", "left_trigger", ...).
const char* PadButtonName(PadButton button);
std::optional<PadButton> ParsePadButton(std::string_view name);
// The game's icon markup for the button ("@(A)", "@(L)", "@(L_Trigger)", "@(APad_L_Small)", ...).
const char* PadButtonIcon(PadButton button);

// The analog trigger travel that counts as a press (XINPUT_GAMEPAD_TRIGGER_THRESHOLD).
constexpr uint8_t kTriggerThreshold = 30;

bool Pressed(const PadState& state, PadButton button);
// The first button (in PadButton order) pressed in `now` and not in `before`.
std::optional<PadButton> NewlyPressed(const PadState& before, const PadState& now);

class PadRemap {
 public:
  // The physical button that acts as `action` (itself by default).
  PadButton source(PadButton action) const { return source_[static_cast<size_t>(action)]; }
  bool identity() const;
  // `physical` acts as `action` from now on; the action it acted as gets `action`'s previous button, so every
  // button keeps exactly one action.
  void Assign(PadButton action, PadButton physical);
  // From (action, physical) pairs; false if they do not form a permutation (unchanged then).
  bool Set(const std::array<PadButton, kPadButtonCount>& sources);
  // The state as the game sees it: each action pressed when its physical button is (a trigger acting as a button
  // above kTriggerThreshold; a button acting as a trigger fully pressed; a trigger as a trigger keeps its travel).
  PadState Apply(const PadState& in) const;
  bool operator==(const PadRemap&) const = default;

 private:
  std::array<PadButton, kPadButtonCount> source_{PadButton::kA, PadButton::kB, PadButton::kX, PadButton::kY,
                                                 PadButton::kLeftShoulder, PadButton::kRightShoulder,
                                                 PadButton::kLeftTrigger, PadButton::kRightTrigger,
                                                 PadButton::kBack, PadButton::kStart, PadButton::kLeftThumb,
                                                 PadButton::kRightThumb};
};

}  // namespace kuf::input
