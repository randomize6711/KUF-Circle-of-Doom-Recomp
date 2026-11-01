// Keyboard text entry for the menus' text fields (the lobby name, password, invite code): what the window's key and
// text events become while a field is being edited. Pure: tests/input.
//
// Printable characters come only from the platform's text-input events (SDL_EVENT_TEXT_INPUT through OnKeyChar), never
// from key-downs, so nothing is inserted twice; key-downs give the editing keys (Backspace, Delete, arrows, Home, End,
// Enter, Escape), Ctrl+A (select all), Ctrl+C (copy) and Ctrl+V (paste, the clipboard's typeable characters). While a field is
// edited every key-down is the field's: none reaches the game or the menu navigation. Events keep their order.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kuf::input {

enum class TextKey : uint8_t { kBackspace, kDelete, kLeft, kRight, kHome, kEnd, kSelectAll, kCopy, kEnter, kEscape };

// The characters a field takes: printable ASCII and printable Latin-1 (Å Ä Ö é ü ß ...: the game's menus draw
// single-byte text), as Unicode code points. Text is kept as UTF-8.
bool Typeable(uint32_t codepoint);
void AppendUtf8(std::string& out, uint32_t codepoint);
// The typeable characters of UTF-8 text (one line: it stops at a line break), as UTF-8.
std::string TypeableText(const std::string& utf8, size_t max_chars);

struct TextEvent {
  bool is_key = false;
  TextKey key = TextKey::kEnter;
  std::string text;  // typed or pasted (UTF-8, typeable characters only)
};

class TextEntryRouter {
 public:
  void Begin();
  void End();
  bool active() const { return active_; }
  // A key-down (Windows virtual-key codes, as the SDK gives them); true: consumed (always, while active).
  // Ctrl with Alt is AltGr on many layouts (it types characters), not a shortcut.
  bool KeyDown(uint32_t vk, bool ctrl, bool alt, bool repeat, const std::function<std::string()>& clipboard);
  // A text-input character (a Unicode code point).
  void Char(uint32_t codepoint);
  std::vector<TextEvent> Take();
  // Text-input characters received since Begin (diagnostics: never the characters themselves).
  uint32_t chars_seen() const { return chars_seen_; }

 private:
  void Key(TextKey k);
  void Text(const std::string& s);
  bool active_ = false;
  bool ctrl_ = false;  // the last key-down had Ctrl held
  uint32_t chars_seen_ = 0;
  std::vector<TextEvent> events_;
};

}  // namespace kuf::input
