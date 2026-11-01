#include "coop/text_field.h"

#include <algorithm>
#include <cctype>

#include "input/text_entry.h"

namespace kuf::coop {
namespace {

constexpr char32_t kTextCycle[] = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ\u00C5\u00C4\u00D6abcdefghijklmnopqrstuvwxyz\u00E5\u00E4\u00F60123456789 -_.!?'";
constexpr char32_t kCodeCycle[] = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";

std::u32string Decode(const std::string& utf8) {
  std::u32string out;
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char b = static_cast<unsigned char>(utf8[i]);
    const size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
    if (!len || i + len > utf8.size()) {
      ++i;
      continue;
    }
    char32_t c = len == 1 ? b : b & (0x7F >> len);
    for (size_t k = 1; k < len; ++k) c = c << 6 | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
    out.push_back(c);
    i += len;
  }
  return out;
}

std::string Encode(const std::u32string& s) {
  std::string out;
  for (char32_t c : s) kuf::input::AppendUtf8(out, uint32_t(c));
  return out;
}

}  // namespace

std::string TextField::text() const { return Encode(text_); }

void TextField::Begin(const std::string& utf8, size_t max_chars, Filter filter) {
  max_ = max_chars;
  filter_ = filter;
  text_.clear();
  for (char32_t c : Decode(utf8)) {
    if (Accept(c) && text_.size() < max_) text_.push_back(c);
  }
  caret_ = text_.size();
  all_ = !text_.empty();
}

bool TextField::Accept(char32_t& c) const {
  if (!kuf::input::Typeable(uint32_t(c))) return false;
  if (filter_ == Filter::kCode) {
    if (c >= 0x80 || (!std::isalnum(int(c)) && c != '-')) return false;
    c = char32_t(std::toupper(int(c)));
  }
  return true;
}

void TextField::Insert(const std::string& utf8) {
  if (all_) {
    text_.clear();
    caret_ = 0;
    all_ = false;
  }
  for (char32_t c : Decode(utf8)) {
    if (!Accept(c) || text_.size() >= max_) continue;
    text_.insert(text_.begin() + ptrdiff_t(caret_), c);
    ++caret_;
  }
}

void TextField::Backspace() {
  if (all_) {
    text_.clear();
    caret_ = 0;
    all_ = false;
    return;
  }
  if (caret_ == 0) return;
  text_.erase(caret_ - 1, 1);
  --caret_;
}

void TextField::Delete() {
  if (all_) {
    Backspace();
    return;
  }
  if (caret_ < text_.size()) text_.erase(caret_, 1);
}

void TextField::Left() {
  if (all_) {
    all_ = false;
    caret_ = 0;
    return;
  }
  if (caret_ > 0) --caret_;
}

void TextField::Right() {
  if (all_) {
    all_ = false;
    caret_ = text_.size();
    return;
  }
  if (caret_ < text_.size()) ++caret_;
}

void TextField::Home() {
  all_ = false;
  caret_ = 0;
}

void TextField::End() {
  all_ = false;
  caret_ = text_.size();
}

void TextField::SelectAll() {
  all_ = !text_.empty();
  caret_ = text_.size();
}

void TextField::Step(int dir) {
  if (all_) {  // a selected text is replaced, as by typing
    text_.clear();
    caret_ = 0;
    all_ = false;
  }
  const std::u32string cycle = filter_ == Filter::kCode ? kCodeCycle : kTextCycle;
  if (caret_ == 0) {
    if (text_.size() >= max_) return;
    text_.insert(text_.begin(), cycle[0]);
    caret_ = 1;
    return;
  }
  char32_t& c = text_[caret_ - 1];
  size_t i = cycle.find(c);
  if (i == std::u32string::npos) i = 0;
  i = (i + cycle.size() + (dir > 0 ? 1 : cycle.size() - 1)) % cycle.size();
  c = cycle[i];
}

void TextField::Next() {
  if (all_) {  // a selected text: Right ends the selection at its end (as the keyboard's Right)
    all_ = false;
    caret_ = text_.size();
    return;
  }
  if (caret_ < text_.size()) {
    ++caret_;
    return;
  }
  if (text_.size() >= max_) return;
  text_.push_back(filter_ == Filter::kCode ? kCodeCycle[0] : kTextCycle[0]);
  caret_ = text_.size();
}

std::string TextField::Display(size_t width, bool mask, bool caret_on) const {
  std::u32string shown = mask ? std::u32string(text_.size(), U'*') : text_;
  if (all_) return "[" + Encode(shown.size() > width ? shown.substr(0, width - 1) + U">" : shown) + "]";
  shown.insert(caret_, 1, caret_on ? U'|' : U' ');
  if (shown.size() <= width || width < 3) return Encode(shown);
  // A window of `width` characters that keeps the caret in view.
  size_t start = caret_ + 1 > width - 1 ? caret_ + 2 - width : 0;
  start = std::min(start, shown.size() - width);
  std::u32string out = shown.substr(start, width);
  if (start > 0) out[0] = U'<';
  if (start + width < shown.size()) out[width - 1] = U'>';
  return Encode(out);
}

}  // namespace kuf::coop
