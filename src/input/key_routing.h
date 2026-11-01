// The input driver's decision for each key-down (KufInputDriver::OnKeyDown), as one function the driver and the tests
// share: a text field being edited takes the key (text entry: never the emulated controller), else the keyboard's
// emulated Xbox 360 controller (KeyboardPad) gets it. Key capture (Key Bindings) is decided before, in the driver.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "input/keyboard_pad.h"
#include "input/text_entry.h"

namespace kuf::input {

enum class KeyRoute { kText, kPad };

KeyRoute RouteKeyDown(TextEntryRouter& text, KeyboardPad& pad, uint32_t vk, bool ctrl, bool alt, bool repeat,
                      const std::function<std::string()>& clipboard);

}  // namespace kuf::input
