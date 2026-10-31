// Keyboard/mouse -> gamepad translation (src/input/keyboard_pad.*) and the input-script parser. Pure logic:
// no window, no guest. Each Poll() is one guest poll (the game polls once per frame).

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include <rex/ui/virtual_key.h>

#include "input/input_script_parse.h"
#include "input/keyboard_pad.h"

using namespace kuf::input;
namespace b = kuf::input::buttons;
using VK = rex::ui::VirtualKey;

namespace {

HostKey K(VK vk) { return static_cast<HostKey>(vk); }
HostKey K(const char* name) { return ParseHostKey(name); }

}  // namespace

TEST_CASE("default bindings: arrows are the D-pad, Escape is B, Space is A", "[input][keyboard]") {
  KeyboardPad pad;
  pad.KeyDown(K(VK::kUp));
  CHECK(pad.Poll().buttons == b::kDpadUp);
  pad.KeyUp(K(VK::kUp));
  pad.KeyDown(K(VK::kDown));
  CHECK(pad.Poll().buttons == b::kDpadDown);
  pad.KeyUp(K(VK::kDown));
  pad.KeyDown(K(VK::kLeft));
  pad.KeyDown(K(VK::kRight));
  CHECK(pad.Poll().buttons == (b::kDpadLeft | b::kDpadRight));
  pad.KeyUp(K(VK::kLeft));
  pad.KeyUp(K(VK::kRight));
  pad.KeyDown(K(VK::kEscape));
  CHECK(pad.Poll().buttons == b::kB);
  pad.KeyUp(K(VK::kEscape));
  pad.KeyDown(K(VK::kSpace));
  CHECK(pad.Poll().buttons == b::kA);
  pad.KeyUp(K(VK::kSpace));
  pad.KeyDown(K("F1"));
  CHECK(pad.Poll().buttons == b::kStart);
  pad.KeyUp(K("F1"));
  pad.KeyDown(K(VK::kTab));
  CHECK(pad.Poll().buttons == b::kBack);
  pad.KeyUp(K(VK::kTab));
  CHECK(pad.Poll() == PadState{});
}

TEST_CASE("press and release transitions: held keys stay pressed, release on the next poll", "[input][keyboard]") {
  KeyboardPad pad;
  CHECK(pad.Poll().buttons == 0);
  pad.KeyDown(K(VK::kDown));
  for (int i = 0; i < 5; ++i) {
    CHECK(pad.Poll().buttons == b::kDpadDown);  // held: one press edge, no repeats
  }
  pad.KeyUp(K(VK::kDown));
  CHECK(pad.Poll().buttons == 0);
}

TEST_CASE("OS key repeat never creates a second press", "[input][keyboard]") {
  KeyboardPad pad;
  pad.KeyDown(K(VK::kDown));
  CHECK(pad.Poll().buttons == b::kDpadDown);
  pad.KeyDown(K(VK::kDown), /*repeat=*/true);
  pad.KeyDown(K(VK::kDown));  // duplicate down without repeat flag
  CHECK(pad.Poll().buttons == b::kDpadDown);
  pad.KeyUp(K(VK::kDown));
  CHECK(pad.Poll().buttons == 0);
}

TEST_CASE("a tap shorter than one poll is latched for exactly one poll", "[input][keyboard]") {
  KeyboardPad pad;
  pad.KeyDown(K(VK::kEscape));
  pad.KeyUp(K(VK::kEscape));
  CHECK(pad.Poll().buttons == b::kB);
  CHECK(pad.Poll().buttons == 0);
}

TEST_CASE("two keys on one control: released only when both are up", "[input][keyboard]") {
  KeyboardPad pad;  // B = Escape, Backspace
  pad.KeyDown(K(VK::kEscape));
  pad.KeyDown(K(VK::kBack));
  CHECK(pad.Poll().buttons == b::kB);
  pad.KeyUp(K(VK::kEscape));
  CHECK(pad.Poll().buttons == b::kB);
  pad.KeyUp(K(VK::kBack));
  CHECK(pad.Poll().buttons == 0);
}

TEST_CASE("focus loss releases held keys and drops pending taps", "[input][keyboard][focus]") {
  KeyboardPad pad;
  pad.KeyDown(K(VK::kDown));
  pad.KeyDown(K(VK::kSpace));
  pad.KeyDown(K(VK::kEscape));
  pad.KeyUp(K(VK::kEscape));  // latched tap, not yet polled
  pad.SetFocused(false);
  CHECK(pad.Poll() == PadState{});
  CHECK_FALSE(pad.AnyHeld());
  pad.KeyDown(K(VK::kUp));  // ignored while unfocused
  CHECK(pad.Poll() == PadState{});
  pad.SetFocused(true);
  CHECK(pad.Poll() == PadState{});  // nothing stuck after regaining focus
  pad.KeyDown(K(VK::kUp));
  CHECK(pad.Poll().buttons == b::kDpadUp);
}

TEST_CASE("a key-up while unfocused is still applied", "[input][keyboard][focus]") {
  KeyboardPad pad;
  pad.KeyDown(K(VK::kDown));
  CHECK(pad.Poll().buttons == b::kDpadDown);
  pad.SetFocused(false);
  pad.KeyUp(K(VK::kDown));
  pad.SetFocused(true);
  CHECK(pad.Poll().buttons == 0);
}

TEST_CASE("sticks and triggers: WASD, IJKL, Z/C; opposite keys cancel", "[input][keyboard]") {
  KeyboardPad pad;
  pad.KeyDown(K("W"));
  pad.KeyDown(K("D"));
  pad.KeyDown(K("J"));
  pad.KeyDown(K("Z"));
  PadState s = pad.Poll();
  CHECK(s.thumb_ly == 32767);
  CHECK(s.thumb_lx == 32767);
  CHECK(s.thumb_rx == -32767);
  CHECK(s.left_trigger == 0xFF);
  CHECK(s.right_trigger == 0);
  pad.KeyDown(K("A"));  // left + right
  CHECK(pad.Poll().thumb_lx == 0);
}

TEST_CASE("mouse wheel: one D-pad press per detent, each its own edge", "[input][mouse]") {
  KeyboardPad pad;
  pad.Wheel(2);
  CHECK(pad.Poll().buttons == b::kDpadUp);
  CHECK(pad.Poll().buttons == 0);  // gap so the game sees two presses
  CHECK(pad.Poll().buttons == b::kDpadUp);
  CHECK(pad.Poll().buttons == 0);
  CHECK(pad.Poll().buttons == 0);
  pad.Wheel(-1);
  CHECK(pad.Poll().buttons == b::kDpadDown);
  CHECK(pad.Poll().buttons == 0);
}

TEST_CASE("mouse wheel queue is bounded and cleared by focus loss", "[input][mouse][focus]") {
  KeyboardPad pad;
  pad.Wheel(-100);
  int presses = 0;
  for (int i = 0; i < 40; ++i) {
    presses += pad.Poll().buttons == b::kDpadDown;
  }
  CHECK(presses == 8);
  pad.Wheel(-3);
  pad.SetFocused(false);
  pad.SetFocused(true);
  CHECK(pad.Poll().buttons == 0);
}

TEST_CASE("pulses (menu pointer): one press each, in order after the wheel, counted as pending", "[input][mouse]") {
  KeyboardPad pad;
  pad.Wheel(-1);
  pad.Pulse(Control::kDpadLeft);
  pad.Pulse(Control::kA);
  CHECK(pad.pending_pulses() == 3);
  CHECK(pad.Poll().buttons == b::kDpadDown);
  CHECK(pad.pending_pulses() == 3);  // two queued and the released poll after this press
  CHECK(pad.Poll().buttons == 0);
  CHECK(pad.Poll().buttons == b::kDpadLeft);
  CHECK(pad.Poll().buttons == 0);
  CHECK(pad.Poll().buttons == b::kA);
  CHECK(pad.Poll().buttons == 0);
  CHECK(pad.pending_pulses() == 0);
  pad.SetFocused(false);
  pad.Pulse(Control::kA);  // not while unfocused
  pad.SetFocused(true);
  CHECK(pad.Poll().buttons == 0);
}

TEST_CASE("mouse buttons: right click is B, middle click is the right stick button", "[input][mouse]") {
  KeyboardPad pad;
  pad.KeyDown(K(VK::kRButton));
  CHECK(pad.Poll().buttons == b::kB);
  pad.KeyUp(K(VK::kRButton));
  pad.KeyDown(K(VK::kMButton));
  CHECK(pad.Poll().buttons == b::kRightThumb);
}

TEST_CASE("Enter and left click are Start at 'Press START' (and nothing else), A in menus", "[input][keyboard][context]") {
  KeyboardPad pad;
  pad.set_context(Context::kStartPrompt);
  pad.KeyDown(K(VK::kReturn));
  CHECK(pad.Poll().buttons == b::kStart);  // never A and Start together: the title screen rejects that
  pad.KeyUp(K(VK::kReturn));
  CHECK(pad.Poll().buttons == 0);
  pad.set_context(Context::kMenu);
  pad.KeyDown(K(VK::kReturn));
  CHECK(pad.Poll().buttons == b::kA);
  pad.KeyUp(K(VK::kReturn));
  pad.set_context(Context::kStartPrompt);
  pad.KeyDown(K(VK::kLButton));
  pad.KeyUp(K(VK::kLButton));  // a click shorter than a frame
  CHECK(pad.Poll().buttons == b::kStart);
  CHECK(pad.Poll().buttons == 0);
  pad.KeyDown(K(VK::kSpace));  // Space stays A at the prompt (does not pass it)
  CHECK(pad.Poll().buttons == b::kA);
}

TEST_CASE("every key keeps its press-time meaning across a context change", "[input][keyboard][context]") {
  KeyboardPad pad;
  pad.set_context(Context::kStartPrompt);
  pad.KeyDown(K(VK::kReturn));
  CHECK(pad.Poll().buttons == b::kStart);
  pad.set_context(Context::kMenu);  // the game left "Press START": the held key must not become A
  CHECK(pad.Poll().buttons == b::kStart);
  pad.KeyUp(K(VK::kReturn));
  CHECK(pad.Poll().buttons == 0);
  pad.KeyDown(K("NumpadEnter"));  // the next press decides again (the SDK maps NumpadEnter to Return)
  CHECK(pad.Poll().buttons == b::kA);
  pad.KeyUp(K("NumpadEnter"));

  pad.set_context(Context::kGameplay);
  pad.KeyDown(K(VK::kRButton));  // gameplay: X weapon
  CHECK(pad.Poll().buttons == b::kX);
  pad.set_context(Context::kMenu);  // the game pauses while the button is held
  CHECK(pad.Poll().buttons == b::kX);
  pad.KeyUp(K(VK::kRButton));
  pad.KeyDown(K(VK::kRButton));  // in the menu: B (back)
  CHECK(pad.Poll().buttons == b::kB);
}

TEST_CASE("gameplay bindings follow the game's Controls page", "[input][keyboard][context]") {
  KeyboardPad pad;
  pad.set_context(Context::kGameplay);
  auto press = [&](HostKey k) {
    pad.KeyDown(k);
    pad.KeyUp(k);
    return pad.Poll();
  };
  CHECK(press(K(VK::kLButton)).buttons == b::kA);    // A weapon (primary attack)
  CHECK(press(K(VK::kRButton)).buttons == b::kX);    // X weapon (secondary attack)
  CHECK(press(K(VK::kSpace)).buttons == b::kB);      // B ability (Dash)
  CHECK(press(K("E")).buttons == b::kY);             // pick up items / detection
  CHECK(press(K(VK::kEscape)).buttons == b::kBack);  // options menu (pause)
  CHECK(press(K(VK::kTab)).buttons == b::kStart);    // status menu (equipment, inventory)
  CHECK(press(K("Q")).buttons == b::kLeftShoulder);  // LB item
  CHECK(press(K("R")).buttons == b::kRightShoulder); // RB item
  CHECK(press(K("Z")).left_trigger == 0xFF);         // aiming view
  CHECK(press(K("F")).right_trigger == 0xFF);        // RT ability
  CHECK(press(K(VK::kMButton)).buttons == b::kRightThumb);  // centre the camera
  pad.KeyDown(K("W"));
  CHECK(pad.Poll().thumb_ly == 32767);
  pad.KeyUp(K("W"));
  pad.set_context(Context::kMenu);  // menus keep their M4 meaning
  CHECK(press(K(VK::kEscape)).buttons == b::kB);
  CHECK(press(K(VK::kSpace)).buttons == b::kA);
}

TEST_CASE("mouse look: movement becomes right-stick deflection in gameplay only", "[input][mouse][look]") {
  KeyboardPad pad;
  pad.Look(50, 0);  // menus: ignored
  CHECK(pad.Poll().thumb_rx == 0);
  pad.set_context(Context::kGameplay);
  pad.Look(10, -10);  // right and up
  PadState s = pad.Poll();
  CHECK(s.thumb_rx > 12900);
  CHECK(s.thumb_ry > 12900);        // moving the mouse up looks up
  CHECK(pad.Poll().thumb_rx == 0);  // consumed: no movement, no turning
  pad.Look(-10000, 0);
  CHECK(pad.Poll().thumb_rx == -32767);  // clamped
  pad.Look(5, 0);
  pad.set_context(Context::kMenu);  // leaving gameplay drops pending movement
  pad.set_context(Context::kGameplay);
  CHECK(pad.Poll().thumb_rx == 0);
  pad.KeyDown(K("J"));  // keyboard camera and mouse combine: the larger deflection wins
  pad.Look(3, 0);
  CHECK(pad.Poll().thumb_rx == -32767);
  pad.SetFocused(false);  // focus loss drops pending movement too
  pad.SetFocused(true);
  CHECK(pad.Poll().thumb_rx == 0);
}

TEST_CASE("direct mouse look (modern camera): no stick deflection, the movement is taken whole", "[input][mouse][look]") {
  KeyboardPad pad;
  LookSettings s;
  s.direct = true;
  pad.set_look_settings(s);
  double dx = 1, dy = 1;
  pad.Look(50, 0);  // menus: ignored
  pad.TakeLook(&dx, &dy);
  CHECK(dx == 0);
  pad.set_context(Context::kGameplay);
  pad.Look(10, -4);
  pad.Look(5.5, -1);
  const PadState state = pad.Poll();  // polls in between do not turn it into a stick deflection
  CHECK(state.thumb_rx == 0);
  CHECK(state.thumb_ry == 0);
  pad.TakeLook(&dx, &dy);
  CHECK(dx == 15.5);  // every count, unscaled (no acceleration)
  CHECK(dy == -5);
  pad.TakeLook(&dx, &dy);
  CHECK(dx == 0);  // taken once
  pad.Look(3, 0);
  pad.set_context(Context::kMenu);  // leaving gameplay drops pending movement
  pad.set_context(Context::kGameplay);
  pad.TakeLook(&dx, &dy);
  CHECK(dx == 0);
  for (int i = 0; i < 100; ++i) pad.Look(1000, 0);  // a stalled game never releases a huge turn later
  pad.TakeLook(&dx, &dy);
  CHECK(dx == 4000);
  pad.KeyDown(K("J"));  // the keyboard's camera keys stay a stick
  CHECK(pad.Poll().thumb_rx == -32767);
}

TEST_CASE("LookAxis: dead-zone compensated, linear, sensitivity, inversion", "[input][mouse][look]") {
  LookSettings s;
  CHECK(LookAxis(0, s) == 0);
  CHECK(LookAxis(1, s) == static_cast<int16_t>(s.deadzone + s.gain));
  CHECK(LookAxis(-2, s) == -static_cast<int16_t>(s.deadzone + 2 * s.gain));
  s.sensitivity = 2.0;
  CHECK(LookAxis(1, s) == static_cast<int16_t>(s.deadzone + 2 * s.gain));
  KeyboardPad pad;
  s.sensitivity = 1.0;
  s.invert_y = true;
  pad.set_look_settings(s);
  pad.set_context(Context::kGameplay);
  pad.Look(0, -10);
  CHECK(pad.Poll().thumb_ry < 0);
}

TEST_CASE("bindings: custom lists, multiple controls per key, unknown names rejected", "[input][keyboard]") {
  Bindings bindings = Bindings::Defaults();
  std::string error;
  CHECK(bindings.Set(Context::kMenu, Control::kA, "K, Return"));
  CHECK(bindings.Set(Context::kMenu, Control::kStart, "K"));
  CHECK_FALSE(bindings.Set(Context::kMenu, Control::kB, "Escape,NotAKey", &error));
  CHECK(error.find("NotAKey") != std::string::npos);
  CHECK(bindings.keys(Context::kMenu, Control::kB).size() == 3);  // Escape, Backspace, MouseRight: unchanged
  CHECK(bindings.Set(Context::kMenu, Control::kY, ""));            // unbind
  KeyboardPad pad(bindings);
  pad.KeyDown(K("K"));
  CHECK(pad.Poll().buttons == (b::kA | b::kStart));
  pad.KeyUp(K("K"));
  pad.KeyDown(K("Y"));
  CHECK(pad.Poll().buttons == 0);
  // The prompt context is derived from the menu context and its Start keys.
  std::vector<HostKey> start{K("K")};
  bindings.DerivePromptContext(start);
  CHECK(bindings.ControlsFor(Context::kStartPrompt, K("K")) == (1u << static_cast<int>(Control::kStart)));
}

TEST_CASE("WindowToGuest: exact fit, pillarbox, letterbox, resized windows", "[input][mouse][resize]") {
  GuestPoint p;
  REQUIRE(WindowToGuest(1280, 720, 640, 360, 1280, 720, &p));
  CHECK((p.x == 640 && p.y == 360));
  REQUIRE(WindowToGuest(2560, 1440, 2559, 1439, 1280, 720, &p));
  CHECK((p.x == 1279 && p.y == 719));
  // 1890x1029 (the M3 windowed run): wider than 16:9 -> pillarbox, image 1829x1029 at x=30.
  CHECK_FALSE(WindowToGuest(1890, 1029, 10, 500, 1280, 720, &p));
  REQUIRE(WindowToGuest(1890, 1029, 945, 514, 1280, 720, &p));
  CHECK((p.x == 640 && p.y == 359));
  // 1280x1024 (5:4): letterbox, image 1280x720 at y=152.
  CHECK_FALSE(WindowToGuest(1280, 1024, 640, 100, 1280, 720, &p));
  REQUIRE(WindowToGuest(1280, 1024, 0, 152, 1280, 720, &p));
  CHECK((p.x == 0 && p.y == 0));
  REQUIRE(WindowToGuest(1280, 1024, 1279, 152 + 719, 1280, 720, &p));
  CHECK((p.x == 1279 && p.y == 719));
  CHECK_FALSE(WindowToGuest(0, 0, 0, 0, 1280, 720, &p));
}

TEST_CASE("input script parser: expansion, ordering and errors", "[input][script]") {
  std::multimap<uint64_t, script::Event> ev;
  std::string error;
  REQUIRE(script::Parse("500 tap Return; 600 press Escape; 700 wheel -2; 50 focus off; 800 pad connect;"
                        " 801 pad press a; 802 pad axis lefty 32767; 900 mouse 10 20 1280 720; 901 log hello world",
                        ev, &error) == 9);
  auto at = [&](uint64_t f) {
    std::string s;
    auto [lo, hi] = ev.equal_range(f);
    for (auto it = lo; it != hi; ++it) {
      s += it->second.verb + (it->second.args.empty() ? "" : ":" + it->second.args.back()) + " ";
    }
    return s;
  };
  CHECK(ev.begin()->first == 50);
  CHECK(at(500) == "down:Return up:Return ");  // tap: same frame, in order
  CHECK(at(600) == "down:Escape ");
  CHECK(at(604) == "up:Escape ");                // press: released 4 frames later
  CHECK(at(801) == "pad:a ");
  CHECK(at(805) == "pad:a ");
  CHECK(at(901) == "log: hello world ");
  for (const char* bad : {"0 tap Return", "x tap Return", "10 tap", "10 tap NoSuchKey", "10 jump",
                          "10 wheel up", "10 focus maybe", "10 pad press z", "10 pad axis lefty 40000",
                          "10 mouse 1 2 3"}) {
    INFO(bad);
    CHECK(script::Parse(bad, ev, &error) == -1);
    CHECK(ev.empty());
    CHECK(error.find("input script") != std::string::npos);
  }
  CHECK(script::Parse("", ev, &error) == 0);
}

TEST_CASE("input script parser: real SDL events and pointer queries", "[input][script]") {
  std::multimap<uint64_t, script::Event> ev;
  std::string error;
  REQUIRE(script::Parse("10 sdl key down Return; 11 sdl key repeat Down; 12 sdl click right 5 6; "
                        "13 sdl wheel -1 0 0; 14 sdl motion 1 2; 15 sdl focus off; 16 sdl resize 1280 1024; 17 pointer",
                        ev, &error) == 8);
  CHECK(ev.count(12) == 2);  // click = button down + up
  CHECK(ev.find(12)->second.args[1] == "down");
  for (const char* bad : {"10 sdl key press Return", "10 sdl key down NoSuchKey", "10 sdl click left 1",
                          "10 sdl resize 10 10", "10 sdl teleport", "10 pointer now"}) {
    INFO(bad);
    CHECK(script::Parse(bad, ev, &error) == -1);
  }
}

TEST_CASE("input script parser: context anchors", "[input][script]") {
  std::multimap<uint64_t, script::Event> ev;
  std::string error;
  REQUIRE(script::Parse("@gameplay#2+30 press Space; @menu+0 tap Return; 100 tap Up", ev, &error) == 3);
  int anchored = 0;
  for (const auto& [when, e] : ev) {
    if (e.anchor_count) {
      ++anchored;
      if (e.verb == "down" && e.args[0] == "Space") {
        CHECK((when == 30 && e.anchor_context == "gameplay" && e.anchor_count == 2));
      }
      if (e.args[0] == "Space" && e.verb == "up") {
        CHECK(when == 34);  // the release of an anchored press is anchored too
      }
      if (e.args[0] == "Return") {
        CHECK((e.anchor_context == "menu" && e.anchor_count == 1 && when == 0));
      }
    }
  }
  CHECK(anchored == 4);
  for (const char* bad : {"@play+3 tap Up", "@gameplay#0+3 tap Up", "@gameplay+x tap Up", "@gameplay#2 tap Up",
                          "@menu+-1 tap Up"}) {
    INFO(bad);
    CHECK(script::Parse(bad, ev, &error) == -1);
  }
}

TEST_CASE("input script parser: repeat until a context", "[input][script]") {
  std::multimap<uint64_t, script::Event> ev;
  std::string error;
  REQUIRE(script::Parse("1800 repeat 60 until gameplay#1 tap Return", ev, &error) == 1);
  REQUIRE(ev.size() == 2);  // tap = down + up, both repeated
  for (const auto& [when, e] : ev) {
    CHECK((when == 1800 && e.repeat_interval == 60 && e.until_context == "gameplay" && e.until_count == 1));
  }
  // press = down now, up 4 frames later; both repeat, and the up is marked as a release (it still applies
  // once after the target context was reached, so no key stays held).
  REQUIRE(script::Parse("1750 repeat 60 until gameplay press Return", ev, &error) == 1);
  REQUIRE(ev.size() == 2);
  CHECK((ev.count(1750) == 1 && !ev.find(1750)->second.release && ev.find(1750)->second.repeat_interval == 60));
  CHECK((ev.count(1754) == 1 && ev.find(1754)->second.release && ev.find(1754)->second.repeat_interval == 60));
  for (const char* bad : {"10 repeat 0 until gameplay tap Up", "10 repeat 5 untl gameplay tap Up",
                          "10 repeat 5 until nowhere tap Up", "10 repeat 5 until menu log x"}) {
    INFO(bad);
    CHECK(script::Parse(bad, ev, &error) == -1);
  }
}


// K42: the controller scenario skips the intro with "repeat 60 until gameplay pad down a" and "pad up a" 3 ticks later.
// When the game entered gameplay between such a down and its up, the up was dropped with the repeat and A stayed
// held, so the next A press gave the game no new edge (the tutorial's "End tutorial?" ignored it).
TEST_CASE("input script: a repeat that ends leaves nothing held (K42)", "[input][script][k42]") {
  // Plays the script tick by tick, entering `context` in tick `entered`; returns the pad buttons and stick axes left
  // non-zero after tick 2000.
  auto held_after = [](const std::string& text, uint64_t entered) {
    std::multimap<uint64_t, script::Event> ev;
    std::string error;
    REQUIRE(script::Parse(text, ev, &error) > 0);
    std::map<std::string, int> entries;
    std::map<std::string, bool> held;
    for (uint64_t tick = 1; tick <= 2000; ++tick) {
      std::vector<std::pair<uint64_t, script::Event>> due;
      script::TakeDue(ev, tick, entries, due);
      for (const auto& [when, e] : due) {
        if (e.verb != "pad") continue;
        if (e.args[0] == "down" || e.args[0] == "up") held[e.args[1]] = e.args[0] == "down";
        if (e.args[0] == "axis") held[e.args[1]] = e.args[2] != "0";
      }
      if (tick == entered) ++entries["gameplay"];  // the game enters the context during this tick
    }
    std::string left;
    for (const auto& [name, on] : held) {
      if (on) left += name + " ";
    }
    return left;
  };
  const std::string skip = "1800 repeat 60 until gameplay pad down a; 1803 repeat 60 until gameplay pad up a";
  // Entered between a down (1860, applied as tick 1861 starts) and its up (1863): the failing runs.
  for (uint64_t entered : {1861u, 1862u, 1863u}) {
    INFO(entered);
    CHECK(held_after(skip, entered).empty());
  }
  // Entered after an up, before the next down: nothing was ever left held.
  CHECK(held_after(skip, 1870).empty());
  CHECK(held_after(skip, 1799).empty());
  // Sticks: an axis back to 0 is a release too; a press's release as before.
  CHECK(held_after("100 repeat 30 until gameplay pad axis lefty -32767; 110 repeat 30 until gameplay pad axis lefty 0",
                   105).empty());
  CHECK(held_after("100 repeat 30 until gameplay pad press start", 101).empty());
  // Downs stop with the repeat: no further press once the context was entered.
  std::multimap<uint64_t, script::Event> ev;
  std::string error;
  REQUIRE(script::Parse(skip, ev, &error) == 2);
  std::map<std::string, int> entries{{"gameplay", 1}};
  std::vector<std::pair<uint64_t, script::Event>> due;
  script::TakeDue(ev, 2000, entries, due);
  REQUIRE(due.size() == 1);
  CHECK((due[0].second.args == std::vector<std::string>{"up", "a"}));
  CHECK(ev.empty());
}

TEST_CASE("input script: what counts as a release", "[input][script][k42]") {
  std::multimap<uint64_t, script::Event> ev;
  std::string error;
  REQUIRE(script::Parse("1 down Return; 2 up Return; 3 pad down a; 4 pad up a; 5 pad axis leftx 0; "
                        "6 pad axis leftx 9000; 7 sdl key up Return; 8 sdl key down Return; "
                        "9 sdl button up left 1 1; 10 wheel 1; 11 pad press b",
                        ev, &error) == 11);
  std::vector<bool> releases;
  for (const auto& [when, e] : ev) releases.push_back(script::Releases(e));
  // 11 pad press b expands to a down at 11 and its release at 15.
  CHECK(releases == std::vector<bool>{false, true, false, true, true, false, true, false, true, false, false, true});
}

TEST_CASE("input script files: lines, comments, anchors with '#'", "[input][script]") {
  const std::string file =
      "# new game, keyboard\n"
      "500 tap Return\n"
      "\n"
      "   # indented comment\n"
      "700 tap Return; 850 tap Return\r\n"
      "@menu#2+60 press Space\n"
      "@gameplay+10 log reached # not a comment here\n";
  const std::string text = script::ScriptFromFileText(file);
  CHECK(text ==
        "500 tap Return; 700 tap Return; 850 tap Return; @menu#2+60 press Space; "
        "@gameplay+10 log reached # not a comment here");
  std::multimap<uint64_t, script::Event> ev;
  std::string error;
  REQUIRE(script::Parse(text, ev, &error) == 5);
  CHECK(ev.count(500) == 2);  // a tap is down + up at the same frame
  CHECK(ev.count(850) == 2);
  CHECK(script::ScriptFromFileText("# only comments\n\n").empty());
}

TEST_CASE("input script parser: freeze (M6D diagnostic)", "[input][script]") {
  std::multimap<uint64_t, script::Event> ev;
  std::string error;
  REQUIRE(script::Parse("@gameplay+300 freeze 3 idle; 420 freeze 30 title_2", ev, &error) == 2);
  CHECK(ev.find(420)->second.args[0] == "30");
  CHECK(ev.find(420)->second.args[1] == "title_2");
  for (const char* bad : {"10 freeze", "10 freeze 3", "10 freeze 0 a", "10 freeze 31 a", "10 freeze 3 Idle",
                          "10 freeze x a", "10 freeze 3 a b"}) {
    INFO(bad);
    CHECK(script::Parse(bad, ev, &error) == -1);
  }
}

TEST_CASE("input script parser: trace (K35, the GPU trace of the next tick frame)", "[input][script]") {
  std::multimap<uint64_t, script::Event> ev;
  std::string error;
  REQUIRE(script::Parse("@gameplay+342 trace; 1840 trace", ev, &error) == 2);
  CHECK(ev.find(1840)->second.verb == "trace");
  CHECK(ev.find(1840)->second.args.empty());
  CHECK(script::Parse("10 trace 3", ev, &error) == -1);
}
