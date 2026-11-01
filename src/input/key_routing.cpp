#include "input/key_routing.h"

namespace kuf::input {

KeyRoute RouteKeyDown(TextEntryRouter& text, KeyboardPad& pad, uint32_t vk, bool ctrl, bool alt, bool repeat,
                      const std::function<std::string()>& clipboard) {
  if (text.KeyDown(vk, ctrl, alt, repeat, clipboard)) return KeyRoute::kText;  // one place only: never both
  pad.KeyDown(static_cast<HostKey>(vk), repeat);
  return KeyRoute::kPad;
}

}  // namespace kuf::input
