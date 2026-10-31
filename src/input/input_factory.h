// The game's input system (M4): SDL controllers + our keyboard/mouse driver + the SDK's NOP fallback.
#pragma once

#include <memory>

#include <rex/system/interfaces/input.h>

namespace kuf::input {

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

}  // namespace kuf::input
