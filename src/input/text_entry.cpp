#include "input/text_entry.h"

namespace kuf::input {
namespace {

constexpr size_t kMaxEvents = 256;
// Windows virtual-key codes (rex::ui::VirtualKey).
constexpr uint32_t kVkC = 0x43;
constexpr uint32_t kVkBack = 0x08, kVkReturn = 0x0D, kVkEscape = 0x1B, kVkEnd = 0x23, kVkHome = 0x24, kVkLeft = 0x25,
                   kVkRight = 0x27, kVkDelete = 0x2E, kVkA = 0x41, kVkV = 0x56, kVkNumpadEnter = 0x6C;

}  // namespace

bool Typeable(uint32_t c) { return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF); }

void AppendUtf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out.push_back(char(c));
  } else if (c < 0x800) {
    out.push_back(char(0xC0 | (c >> 6)));
    out.push_back(char(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(char(0xE0 | (c >> 12)));
    out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(char(0x80 | (c & 0x3F)));
  } else {
    out.push_back(char(0xF0 | (c >> 18)));
    out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(char(0x80 | (c & 0x3F)));
  }
}

std::string TypeableText(const std::string& s, size_t max_chars) {
  std::string out;
  size_t n = 0;
  for (size_t i = 0; i < s.size() && n < max_chars;) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    uint32_t c = 0;
    size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + len > s.size()) {
      ++i;  // not UTF-8: skipped
      continue;
    }
    c = len == 1 ? b : b & (0x7F >> len);
    bool ok = true;
    for (size_t k = 1; k < len; ++k) {
      const unsigned char t = static_cast<unsigned char>(s[i + k]);
      ok &= (t >> 6) == 0x2;
      c = c << 6 | (t & 0x3F);
    }
    i += ok ? len : 1;
    if (!ok) continue;
    if (c == '\n' || c == '\r') break;  // one line
    if (c == '\t') c = ' ';
    if (!Typeable(c)) continue;
    AppendUtf8(out, c);
    ++n;
  }
  return out;
}

void TextEntryRouter::Begin() {
  active_ = true;
  chars_seen_ = 0;
  events_.clear();
}

void TextEntryRouter::End() {
  active_ = false;
  events_.clear();
}

void TextEntryRouter::Key(TextKey k) {
  if (events_.size() < kMaxEvents) events_.push_back({true, k, {}});
}

void TextEntryRouter::Text(const std::string& s) {
  if (s.empty()) return;
  if (!events_.empty() && !events_.back().is_key) {
    events_.back().text += s;  // consecutive characters as one event
  } else if (events_.size() < kMaxEvents) {
    events_.push_back({false, TextKey::kEnter, s});
  }
}

bool TextEntryRouter::KeyDown(uint32_t vk, bool ctrl, bool alt, bool repeat,
                              const std::function<std::string()>& clipboard) {
  if (!active_) return false;
  ctrl = ctrl && !alt;
  ctrl_ = ctrl;  // a key-down comes before its text: with Ctrl held it is a shortcut, not text
  switch (vk) {
    case kVkBack: Key(TextKey::kBackspace); break;  // repeats too: held, it keeps deleting
    case kVkDelete: Key(TextKey::kDelete); break;
    case kVkLeft: Key(TextKey::kLeft); break;
    case kVkRight: Key(TextKey::kRight); break;
    case kVkHome: Key(TextKey::kHome); break;
    case kVkEnd: Key(TextKey::kEnd); break;
    case kVkReturn:
    case kVkNumpadEnter:
      if (!repeat) Key(TextKey::kEnter);
      break;
    case kVkEscape:
      if (!repeat) Key(TextKey::kEscape);
      break;
    case kVkA:
      if (ctrl && !repeat) Key(TextKey::kSelectAll);
      break;  // a plain A arrives as text
    case kVkC:
      if (ctrl && !repeat) Key(TextKey::kCopy);
      break;
    case kVkV:
      if (ctrl && !repeat && clipboard) Text(TypeableText(clipboard(), 256));
      break;
    default:
      break;  // printable keys: their characters come as text input (never twice)
  }
  return true;
}

void TextEntryRouter::Char(uint32_t codepoint) {
  if (!active_ || ctrl_ || !Typeable(codepoint)) return;  // a typeable character, not Ctrl+key
  ++chars_seen_;
  std::string utf8;
  AppendUtf8(utf8, codepoint);
  Text(utf8);
}

std::vector<TextEvent> TextEntryRouter::Take() {
  std::vector<TextEvent> out;
  out.swap(events_);
  return out;
}

}  // namespace kuf::input
