// Backspace and text entry at the input boundary: the keyboard pad's real bindings (src/input/keyboard_pad.cpp) and
// the driver's text entry (src/input/text_entry.cpp), fed the window's raw key-downs and text events.
#include <catch2/catch_test_macros.hpp>

#include <rex/ui/virtual_key.h>

#include "input/key_routing.h"
#include "input/keyboard_pad.h"
#include "input/text_entry.h"

using namespace kuf::input;
namespace b = kuf::input::buttons;
using VK = rex::ui::VirtualKey;

namespace {
HostKey K(VK k) { return static_cast<HostKey>(k); }
}  // namespace

TEST_CASE("Backspace is never a menu control (no B, no Back) in menus and at Press START", "[input][text]") {
  KeyboardPad pad;
  for (Context c : {Context::kMenu, Context::kStartPrompt}) {
    pad.set_context(c);
    pad.KeyDown(K(VK::kBack), false);
    CHECK(pad.Poll().buttons == 0);  // nothing at all: the game's menu never sees it
    pad.KeyUp(K(VK::kBack));
  }
  // Escape and the right mouse button stay B (Back) in the menus.
  pad.set_context(Context::kMenu);
  pad.KeyDown(K(VK::kEscape), false);
  CHECK(pad.Poll().buttons == b::kB);
  pad.KeyUp(K(VK::kEscape));
}

TEST_CASE("Backspace named in a menu binding is dropped; a gameplay binding keeps it", "[input][text]") {
  Bindings bindings = Bindings::Defaults();
  REQUIRE(bindings.Set(Context::kMenu, Control::kB, "Escape,Backspace"));
  REQUIRE(bindings.Set(Context::kGameplay, Control::kY, "Backspace"));
  KeyboardPad pad;
  pad.set_bindings(bindings);
  pad.set_context(Context::kMenu);
  pad.KeyDown(K(VK::kBack), false);
  CHECK(pad.Poll().buttons == 0);
  pad.KeyUp(K(VK::kBack));
  pad.set_context(Context::kGameplay);
  pad.KeyDown(K(VK::kBack), false);
  CHECK(pad.Poll().buttons == b::kY);  // the player's own gameplay key
}

TEST_CASE("text entry: characters only from text events (never twice); editing keys from key-downs", "[input][text]") {
  TextEntryRouter r;
  const auto no_clipboard = [] { return std::string(); };
  CHECK_FALSE(r.KeyDown(uint32_t(VK::kBack), false, false, false, no_clipboard));  // off: not consumed
  r.Begin();
  // Typing "ab": each key's key-down and its text event, as the window delivers them.
  CHECK(r.KeyDown(uint32_t(VK::kA), false, false, false, no_clipboard));
  r.Char('a');
  CHECK(r.KeyDown(0x42, false, false, false, no_clipboard));
  r.Char('b');
  CHECK(r.KeyDown(uint32_t(VK::kBack), false, false, false, no_clipboard));
  CHECK(r.KeyDown(uint32_t(VK::kBack), false, false, true, no_clipboard));  // held: repeats delete
  CHECK(r.KeyDown(uint32_t(VK::kDelete), false, false, false, no_clipboard));
  CHECK(r.KeyDown(uint32_t(VK::kHome), false, false, false, no_clipboard));
  CHECK(r.KeyDown(uint32_t(VK::kEnd), false, false, false, no_clipboard));
  CHECK(r.KeyDown(uint32_t(VK::kLeft), false, false, false, no_clipboard));
  CHECK(r.KeyDown(uint32_t(VK::kRight), false, false, false, no_clipboard));
  CHECK(r.KeyDown(uint32_t(VK::kReturn), false, false, false, no_clipboard));
  CHECK(r.KeyDown(uint32_t(VK::kEscape), false, false, false, no_clipboard));
  CHECK(r.KeyDown(uint32_t(VK::kEscape), false, false, true, no_clipboard));  // a repeat is not a second Escape
  const auto e = r.Take();
  REQUIRE(e.size() == 10);
  CHECK_FALSE(e[0].is_key);
  CHECK(e[0].text == "ab");  // once, in order
  CHECK(e[1].key == TextKey::kBackspace);
  CHECK(e[2].key == TextKey::kBackspace);
  CHECK(e[3].key == TextKey::kDelete);
  CHECK(e[4].key == TextKey::kHome);
  CHECK(e[5].key == TextKey::kEnd);
  CHECK(e[6].key == TextKey::kLeft);
  CHECK(e[7].key == TextKey::kRight);
  CHECK(e[8].key == TextKey::kEnter);
  CHECK(e[9].key == TextKey::kEscape);
}

TEST_CASE("text entry: Ctrl+A selects all, Ctrl+V pastes one clean line once; AltGr still types", "[input][text]") {
  TextEntryRouter r;
  r.Begin();
  const auto clipboard = [] { return std::string("KUF-AB12\tx\nsecond line"); };
  r.KeyDown(uint32_t(VK::kA), true, false, false, clipboard);
  r.Char('a');  // a platform that also sends Ctrl+A's letter as text: ignored
  r.KeyDown(uint32_t(VK::kV), true, false, false, clipboard);
  r.Char('v');
  r.KeyDown(0x51, true, true, false, clipboard);  // AltGr+Q (Ctrl+Alt): '@' on some layouts
  r.Char('@');
  const auto e = r.Take();
  REQUIRE(e.size() == 2);
  CHECK(e[0].key == TextKey::kSelectAll);
  CHECK_FALSE(e[1].is_key);
  CHECK(e[1].text == "KUF-AB12 x@");  // the first line (a tab as a space); then the AltGr character
}

TEST_CASE("key routing (the driver's own): while a field is edited no key reaches the emulated controller",
          "[input][text]") {
  // KufInputDriver::OnKeyDown calls RouteKeyDown with its pad and text entry: the same function here.
  KeyboardPad pad;
  pad.set_context(Context::kMenu);
  TextEntryRouter text;
  const auto none = [] { return std::string(); };
  auto down = [&](VK k, bool ctrl = false) { return RouteKeyDown(text, pad, uint32_t(k), ctrl, false, false, none); };
  auto up = [&](VK k) { pad.KeyUp(K(k)); };
  text.Begin();
  // Every key a text field uses, and keys that are menu controls (Space/Return = A, Escape = B, arrows = D-pad,
  // Tab = Back, W/A/S/D = the stick): one physical press is either text or a controller input, never both.
  for (VK k : {VK::kBack, VK::kDelete, VK::kEscape, VK::kReturn, VK::kSpace, VK::kUp, VK::kDown, VK::kLeft, VK::kRight,
               VK::kHome, VK::kEnd, VK::kTab, VK::kW, VK::kA, VK::kS, VK::kD}) {
    CHECK(down(k) == KeyRoute::kText);
  }
  text.Char('w');
  const PadState s = pad.Poll();
  CHECK(s.buttons == 0);  // no A, B, Back or D-pad press
  CHECK(s.thumb_lx == 0);
  CHECK(s.thumb_ly == 0);
  for (VK k : {VK::kBack, VK::kEscape, VK::kReturn, VK::kSpace, VK::kW}) up(k);
  text.End();
  // After the edit: Escape is the controller's B (Back) again; Backspace still nothing.
  CHECK(down(VK::kEscape) == KeyRoute::kPad);
  CHECK(pad.Poll().buttons == b::kB);
  up(VK::kEscape);
  CHECK(down(VK::kBack) == KeyRoute::kPad);
  CHECK(pad.Poll().buttons == 0);
}

TEST_CASE("text entry: Swedish letters, case, spaces and punctuation as typed; nothing outside Latin-1",
          "[input][text]") {
  TextEntryRouter r;
  r.Begin();
  // The platform's text events carry what the layout produced: a Swedish keyboard gives Å Ä Ö å ä ö, Shift gives
  // capitals (the key-downs carry no characters).
  for (uint32_t c : {0xC5u, 0xC4u, 0xD6u, 0xE5u, 0xE4u, 0xF6u, uint32_t('M'), uint32_t('y'), uint32_t(' '),
                     uint32_t('!'), uint32_t('?'), uint32_t('1'), 0x1F600u /* an emoji: not typeable here */,
                     0x7Fu /* DEL */}) {
    r.Char(c);
  }
  CHECK(r.chars_seen() == 12);
  const auto e = r.Take();
  REQUIRE(e.size() == 1);
  CHECK(e[0].text == "\xC3\x85\xC3\x84\xC3\x96\xC3\xA5\xC3\xA4\xC3\xB6My !?1");  // UTF-8, in order
  r.KeyDown(uint32_t(VK::kC), true, false, false, [] { return std::string(); });
  r.KeyDown(uint32_t(VK::kV), true, false, false, [] { return std::string("\xC3\x85sa \xE2\x98\xBA" "ok"); });
  const auto e2 = r.Take();
  REQUIRE(e2.size() == 2);
  CHECK(e2[0].key == TextKey::kCopy);
  CHECK(e2[1].text == "\xC3\x85sa ok");  // the pasted line, its typeable characters
  CHECK(TypeableText("a\xFF" "b", 10) == "ab");  // not UTF-8: skipped
}
