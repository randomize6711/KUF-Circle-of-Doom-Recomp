// One text field being edited (the lobby menus' name, password and code): its text (characters: Å Ä Ö count as one),
// caret and selection, the edits, and how it is shown in a menu row (a caret, the selection, clipped around the caret,
// masked where asked).
// Pure: tests/net.
#pragma once

#include <cstddef>
#include <string>

namespace kuf::coop {

class TextField {
 public:
  enum class Filter { kText, kCode };  // kCode: letters, digits and '-', shown and kept in upper case

  // Starts editing `text`: all of it selected (typing replaces it), the caret at its end.
  void Begin(const std::string& utf8, size_t max_chars, Filter filter);
  std::string text() const;  // UTF-8
  size_t caret() const { return caret_; }
  bool all_selected() const { return all_; }

  void Insert(const std::string& utf8);  // typed or pasted; replaces the selection; filtered; up to the limit
  void Backspace();
  void Delete();
  void Left();
  void Right();
  void Home();
  void End();
  void SelectAll();
  // The controller: the character before the caret steps through the characters (one is added at the start or after
  // the last); `Next` adds a new one after the caret (or moves right inside the text).
  void Step(int dir);
  void Next();

  // The field as a row shows it: at most `width` characters around the caret, '<' / '>' where clipped, the caret
  // '|' when `caret_on` (else a space: it blinks), a selection in brackets, '*' for each character when `mask`. UTF-8;
  // `width` and the caret count characters, not bytes.
  std::string Display(size_t width, bool mask, bool caret_on) const;

 private:
  bool Accept(char32_t& c) const;
  std::u32string text_;
  size_t caret_ = 0, max_ = 32;
  bool all_ = false;
  Filter filter_ = Filter::kText;
};

}  // namespace kuf::coop
