// Controller buttons in gameplay (M7 feature 5): src/input/pad_remap.h.
#include <catch2/catch_test_macros.hpp>

#include "input/pad_remap.h"

using namespace kuf::input;

TEST_CASE("pad remap: names, icons and parsing round-trip", "[pad_remap]") {
  for (size_t i = 0; i < kPadButtonCount; ++i) {
    const auto b = static_cast<PadButton>(i);
    REQUIRE(ParsePadButton(PadButtonName(b)) == b);
    REQUIRE(std::string(PadButtonIcon(b)).starts_with("@("));
  }
  REQUIRE_FALSE(ParsePadButton("dpad_up"));
  REQUIRE_FALSE(ParsePadButton(""));
  // The names are ControlName's, as in pc_settings.toml.
  REQUIRE(std::string(PadButtonName(PadButton::kLeftShoulder)) == ControlName(Control::kLeftShoulder));
  REQUIRE(std::string(PadButtonName(PadButton::kRightTrigger)) == ControlName(Control::kRightTrigger));
}

TEST_CASE("pad remap: the default passes the state through unchanged", "[pad_remap]") {
  PadRemap remap;
  REQUIRE(remap.identity());
  PadState s;
  s.buttons = buttons::kA | buttons::kDpadUp | buttons::kStart;
  s.left_trigger = 17;
  s.thumb_lx = 1234;
  REQUIRE(remap.Apply(s) == s);
}

TEST_CASE("pad remap: assigning swaps, so every button keeps one action", "[pad_remap]") {
  PadRemap remap;
  remap.Assign(PadButton::kA, PadButton::kX);  // X acts as A; A takes A's old button's place: acts as X
  REQUIRE(remap.source(PadButton::kA) == PadButton::kX);
  REQUIRE(remap.source(PadButton::kX) == PadButton::kA);
  REQUIRE_FALSE(remap.identity());
  PadState in;
  in.buttons = buttons::kX | buttons::kDpadLeft;
  const PadState out = remap.Apply(in);
  REQUIRE(out.buttons == (buttons::kA | buttons::kDpadLeft));  // the D-pad is not remapped
  remap.Assign(PadButton::kA, PadButton::kA);  // back
  REQUIRE(remap.identity());
  remap.Assign(PadButton::kB, PadButton::kB);  // a button to itself: nothing changes
  REQUIRE(remap.identity());
}

TEST_CASE("pad remap: triggers and buttons exchanged", "[pad_remap]") {
  PadRemap remap;
  remap.Assign(PadButton::kA, PadButton::kRightTrigger);  // RT acts as A, A as RT
  PadState in;
  in.right_trigger = kTriggerThreshold;  // not yet a press
  REQUIRE(remap.Apply(in).buttons == 0);
  in.right_trigger = kTriggerThreshold + 1;
  PadState out = remap.Apply(in);
  REQUIRE(out.buttons == buttons::kA);
  REQUIRE(out.right_trigger == 0);
  in = {};
  in.buttons = buttons::kA;
  out = remap.Apply(in);
  REQUIRE(out.buttons == 0);
  REQUIRE(out.right_trigger == 255);  // a button acting as a trigger presses it fully
  // A trigger acting as the other trigger keeps its travel.
  PadRemap triggers;
  triggers.Assign(PadButton::kLeftTrigger, PadButton::kRightTrigger);
  in = {};
  in.right_trigger = 99;
  in.left_trigger = 12;
  out = triggers.Apply(in);
  REQUIRE(out.left_trigger == 99);
  REQUIRE(out.right_trigger == 12);
}

TEST_CASE("pad remap: Set refuses what is not a permutation", "[pad_remap]") {
  PadRemap remap;
  std::array<PadButton, kPadButtonCount> sources;
  for (size_t i = 0; i < kPadButtonCount; ++i) sources[i] = static_cast<PadButton>(i);
  std::swap(sources[0], sources[3]);
  REQUIRE(remap.Set(sources));
  REQUIRE(remap.source(PadButton::kA) == PadButton::kY);
  sources[1] = sources[0];  // two actions on one button
  REQUIRE_FALSE(remap.Set(sources));
  REQUIRE(remap.source(PadButton::kA) == PadButton::kY);  // unchanged
}

TEST_CASE("pad remap: newly pressed buttons for capture", "[pad_remap]") {
  PadState before, now;
  before.buttons = buttons::kA;  // held when the capture began
  now.buttons = buttons::kA;
  REQUIRE_FALSE(NewlyPressed(before, now));
  now.buttons = buttons::kA | buttons::kY;
  REQUIRE(NewlyPressed(before, now) == PadButton::kY);
  now = {};
  now.left_trigger = 200;
  REQUIRE(NewlyPressed(before, now) == PadButton::kLeftTrigger);
}

TEST_CASE("host key names read back as the same key", "[pad_remap]") {
  for (const char* name : {"Return", "Escape", "Space", "W", "F1", "MouseLeft", "MouseX2", "Numpad8", "Shift"}) {
    const HostKey key = ParseHostKey(name);
    REQUIRE(key != 0);
    REQUIRE(ParseHostKey(HostKeyName(key)) == key);
  }
  REQUIRE(HostKeyLabel(ParseHostKey("MouseLeft")) == "Mouse L");
  REQUIRE(HostKeyLabel(ParseHostKey("MouseX1")) == "Mouse 4");
  REQUIRE(HostKeyName(0).empty());
}
