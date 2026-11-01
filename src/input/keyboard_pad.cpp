#include "input/keyboard_pad.h"

#include <algorithm>
#include <cmath>

#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

namespace kuf::input {
namespace {

constexpr const char* kControlNames[kControlCount] = {
    "a", "b", "x", "y",
    "left_shoulder", "right_shoulder", "back", "start", "left_thumb", "right_thumb",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "left_trigger", "right_trigger",
    "lstick_up", "lstick_down", "lstick_left", "lstick_right",
    "rstick_up", "rstick_down", "rstick_left", "rstick_right",
};

constexpr uint16_t ButtonBit(Control control) {
  switch (control) {
    case Control::kA: return buttons::kA;
    case Control::kB: return buttons::kB;
    case Control::kX: return buttons::kX;
    case Control::kY: return buttons::kY;
    case Control::kLeftShoulder: return buttons::kLeftShoulder;
    case Control::kRightShoulder: return buttons::kRightShoulder;
    case Control::kBack: return buttons::kBack;
    case Control::kStart: return buttons::kStart;
    case Control::kLeftThumb: return buttons::kLeftThumb;
    case Control::kRightThumb: return buttons::kRightThumb;
    case Control::kDpadUp: return buttons::kDpadUp;
    case Control::kDpadDown: return buttons::kDpadDown;
    case Control::kDpadLeft: return buttons::kDpadLeft;
    case Control::kDpadRight: return buttons::kDpadRight;
    default: return 0;
  }
}

constexpr uint32_t Bit(Control c) { return uint32_t(1) << static_cast<uint32_t>(c); }

std::string_view Trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return s;
}

int16_t Axis(bool negative, bool positive) {
  // Opposite keys cancel, like a centred stick.
  return static_cast<int16_t>((positive ? 32767 : 0) - (negative ? 32767 : 0));
}

int16_t LargerMagnitude(int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; }

}  // namespace

const char* ControlName(Control control) {
  const size_t i = static_cast<size_t>(control);
  return i < kControlCount ? kControlNames[i] : "?";
}

HostKey ParseHostKey(std::string_view name) {
  using rex::ui::VirtualKey;
  if (name == "MouseLeft") return static_cast<HostKey>(VirtualKey::kLButton);
  if (name == "MouseRight") return static_cast<HostKey>(VirtualKey::kRButton);
  if (name == "MouseMiddle") return static_cast<HostKey>(VirtualKey::kMButton);
  if (name == "MouseX1") return static_cast<HostKey>(VirtualKey::kXButton1);
  if (name == "MouseX2") return static_cast<HostKey>(VirtualKey::kXButton2);
  const auto vk = static_cast<uint16_t>(rex::ui::ParseVirtualKey(name));
  return vk < 0x100 ? vk : 0;
}

std::string HostKeyName(HostKey key) {
  using rex::ui::VirtualKey;
  switch (static_cast<VirtualKey>(key)) {
    case VirtualKey::kLButton: return "MouseLeft";
    case VirtualKey::kRButton: return "MouseRight";
    case VirtualKey::kMButton: return "MouseMiddle";
    case VirtualKey::kXButton1: return "MouseX1";
    case VirtualKey::kXButton2: return "MouseX2";
    default: break;
  }
  if (key == 0 || key >= 0x100) return {};
  std::string name = rex::ui::VirtualKeyToString(static_cast<VirtualKey>(key));
  return ParseHostKey(name) == key ? name : std::string();  // only names that read back as the same key
}

std::string HostKeyLabel(HostKey key) {
  using rex::ui::VirtualKey;
  switch (static_cast<VirtualKey>(key)) {
    case VirtualKey::kLButton: return "Mouse L";
    case VirtualKey::kRButton: return "Mouse R";
    case VirtualKey::kMButton: return "Mouse M";
    case VirtualKey::kXButton1: return "Mouse 4";
    case VirtualKey::kXButton2: return "Mouse 5";
    case VirtualKey::kReturn: return "Enter";
    case VirtualKey::kEscape: return "Esc";
    default: return HostKeyName(key);
  }
}

bool ParseHostKeys(std::string_view key_list, std::vector<HostKey>* out, std::string* error) {
  std::vector<HostKey> keys;
  std::string_view rest = key_list;
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    const std::string_view token = Trim(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    if (token.empty()) {
      continue;
    }
    const HostKey key = ParseHostKey(token);
    if (!key) {
      if (error) {
        *error = "unknown key '" + std::string(token) + "'";
      }
      return false;
    }
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
      keys.push_back(key);
    }
  }
  *out = std::move(keys);
  return true;
}

Bindings Bindings::Defaults() {
  // docs/INPUT.md lists these; keep the two in sync. F3, F4, F7 and Backtick are taken by the runtime's
  // overlays. Gameplay follows the game's own Controls page (Back/Start menus, A/X weapons, B ability, Y pick
  // up, LB/RB items, LT aim, RT ability, R-stick click centres the camera).
  struct Default {
    Control control;
    const char* menu;
    const char* gameplay;
  };
  static const Default kDefaults[] = {
      {Control::kA, "Space,Return,NumpadEnter,MouseLeft", "MouseLeft,Return"},
      {Control::kB, "Escape,MouseRight", "Space"},  // never Backspace in menus (text deletion only)
      {Control::kX, "X", "MouseRight,X"},
      {Control::kY, "Y", "E,Y"},
      {Control::kLeftShoulder, "Q,PageUp", "Q,1"},
      {Control::kRightShoulder, "E,PageDown", "R,2"},
      {Control::kBack, "Tab", "Escape"},
      {Control::kStart, "F1,Pause", "Tab,F1,Pause"},
      {Control::kLeftThumb, "Shift", "Shift"},
      {Control::kRightThumb, "MouseMiddle", "MouseMiddle,V"},
      {Control::kDpadUp, "Up", "Up"},
      {Control::kDpadDown, "Down", "Down"},
      {Control::kDpadLeft, "Left", "Left"},
      {Control::kDpadRight, "Right", "Right"},
      {Control::kLeftTrigger, "Z", "Z,MouseX1"},
      {Control::kRightTrigger, "C", "F,C,MouseX2"},
      {Control::kLeftStickUp, "W", "W"},
      {Control::kLeftStickDown, "S", "S"},
      {Control::kLeftStickLeft, "A", "A"},
      {Control::kLeftStickRight, "D", "D"},
      {Control::kRightStickUp, "I", "I"},
      {Control::kRightStickDown, "K", "K"},
      {Control::kRightStickLeft, "J", "J"},
      {Control::kRightStickRight, "L", "L"},
  };
  Bindings b;
  for (const auto& d : kDefaults) {
    b.Set(Context::kMenu, d.control, d.menu);
    b.Set(Context::kGameplay, d.control, d.gameplay);
  }
  std::vector<HostKey> prompt_start;
  ParseHostKeys("Return,NumpadEnter,MouseLeft,F1,Pause", &prompt_start);
  b.DerivePromptContext(prompt_start);
  return b;
}

bool Bindings::Set(Context context, Control control, std::string_view key_list, std::string* error) {
  std::vector<HostKey> keys;
  if (!ParseHostKeys(key_list, &keys, error)) {
    if (error) {
      *error += std::string(" for ") + ControlName(control);
    }
    return false;
  }
  if (context != Context::kGameplay) {
    // Backspace deletes text in the menus (the lobby fields) and is never a menu control: not Back, not B, not
    // anything else, even when a setting names it (the player's own gameplay keys may still use it).
    std::erase(keys, kBackspaceKey);
  }
  keys_[static_cast<size_t>(context)][static_cast<size_t>(control)] = std::move(keys);
  return true;
}

void Bindings::DerivePromptContext(const std::vector<HostKey>& start_keys) {
  // The title screen accepts only Start (A in the same frame even blocks it), so the prompt's Start keys
  // must not press anything else there.
  auto& prompt = keys_[static_cast<size_t>(Context::kStartPrompt)];
  prompt = keys_[static_cast<size_t>(Context::kMenu)];
  for (auto& keys : prompt) {
    std::erase_if(keys, [&](HostKey k) { return std::find(start_keys.begin(), start_keys.end(), k) != start_keys.end(); });
  }
  prompt[static_cast<size_t>(Control::kStart)] = start_keys;
}

uint32_t Bindings::ControlsFor(Context context, HostKey key) const {
  uint32_t mask = 0;
  const auto& table = keys_[static_cast<size_t>(context)];
  for (size_t c = 0; c < kControlCount; ++c) {
    if (std::find(table[c].begin(), table[c].end(), key) != table[c].end()) {
      mask |= uint32_t(1) << c;
    }
  }
  return mask;
}

int16_t LookAxis(double pixels, const LookSettings& s) {
  // Linear in movement, starting at the game's dead zone, so that small mouse movements still turn the camera.
  if (pixels == 0) {
    return 0;
  }
  const double magnitude = s.deadzone + std::abs(pixels) * s.gain * s.sensitivity;
  const double clamped = std::min(magnitude, 32767.0);
  return static_cast<int16_t>(pixels > 0 ? clamped : -clamped);
}

void KeyboardPad::set_bindings(Bindings bindings) {
  bindings_ = std::move(bindings);
  latched_ = 0;
}

void KeyboardPad::set_context(Context context) {
  if (context != context_) {
    // A change either way starts the camera from rest (no jump from motion made before a menu opened or while it
    // closed) and drops queued wheel steps (a menu scroll never reaches gameplay). Held keys keep their press-time
    // meaning (KeyDown), so a button held into a menu is never re-read as a menu action.
    look_dx_ = look_dy_ = 0;
    wheel_pulses_.clear();
  }
  context_ = context;
}

void KeyboardPad::KeyDown(HostKey key, bool repeat) {
  if (!focused_ || key >= down_.size() || repeat || down_[key]) {
    return;  // auto-repeat and duplicate downs never create a second press
  }
  down_[key] = true;
  pressed_[key] = bindings_.ControlsFor(context_, key);  // meaning fixed until release
  latched_ |= pressed_[key];
}

void KeyboardPad::KeyUp(HostKey key) {
  // Accepted even without focus, so a key released elsewhere cannot stay down.
  if (key < down_.size()) {
    down_[key] = false;
    pressed_[key] = 0;
  }
}

constexpr size_t kMaxQueued = 8;  // queued pulses: a fast wheel spin must not scroll for seconds

void KeyboardPad::Pulse(Control control) {
  if (focused_ && wheel_pulses_.size() < kMaxQueued) {
    wheel_pulses_.push_back(control);
  }
}

void KeyboardPad::Wheel(int detents) {
  if (!focused_) {
    return;
  }
  for (int i = 0; i < std::abs(detents) && wheel_pulses_.size() < kMaxQueued; ++i) {
    wheel_pulses_.push_back(detents > 0 ? wheel_up_ : wheel_down_);
  }
}

void KeyboardPad::Look(double dx, double dy) {
  if (!focused_ || context_ != Context::kGameplay) {
    return;
  }
  // A stalled game (or a camera that does not take the movement) must not release a huge turn later.
  constexpr double kMaxPending = 4000;
  look_dx_ = std::clamp(look_dx_ + dx, -kMaxPending, kMaxPending);
  look_dy_ = std::clamp(look_dy_ + dy, -kMaxPending, kMaxPending);
}

void KeyboardPad::TakeLook(double* dx, double* dy) {
  *dx = look_dx_;
  *dy = look_dy_;
  look_dx_ = look_dy_ = 0;
}

void KeyboardPad::SetFocused(bool focused) {
  focused_ = focused;
  if (!focused) {
    down_.fill(false);
    pressed_.fill(0);
    latched_ = 0;
    wheel_pulses_.clear();
    wheel_gap_ = false;
    look_dx_ = look_dy_ = 0;
  }
}

bool KeyboardPad::AnyHeld() const {
  return std::any_of(down_.begin(), down_.end(), [](bool d) { return d; });
}

PadState KeyboardPad::Poll() {
  uint32_t active = latched_;
  latched_ = 0;
  for (size_t k = 0; k < down_.size(); ++k) {
    if (down_[k]) {
      active |= pressed_[k];
    }
  }
  if (wheel_gap_) {
    wheel_gap_ = false;  // one released poll between pulses, so each detent is its own edge
  } else if (!wheel_pulses_.empty()) {
    active |= Bit(wheel_pulses_.front());
    wheel_pulses_.pop_front();
    wheel_gap_ = true;
  }

  auto on = [&](Control c) { return (active & Bit(c)) != 0; };
  PadState s;
  for (size_t c = 0; c < kControlCount; ++c) {
    if (active & (uint32_t(1) << c)) {
      s.buttons |= ButtonBit(static_cast<Control>(c));
    }
  }
  s.left_trigger = on(Control::kLeftTrigger) ? 0xFF : 0;
  s.right_trigger = on(Control::kRightTrigger) ? 0xFF : 0;
  s.thumb_lx = Axis(on(Control::kLeftStickLeft), on(Control::kLeftStickRight));
  s.thumb_ly = Axis(on(Control::kLeftStickDown), on(Control::kLeftStickUp));
  s.thumb_rx = Axis(on(Control::kRightStickLeft), on(Control::kRightStickRight));
  s.thumb_ry = Axis(on(Control::kRightStickDown), on(Control::kRightStickUp));

  // Mouse look: right (+x) turns right, up (-y in window pixels) looks up, unless inverted. In direct mode
  // the camera takes the movement itself (TakeLook).
  if (!look_.direct && (look_dx_ != 0 || look_dy_ != 0)) {
    s.thumb_rx = LargerMagnitude(s.thumb_rx, LookAxis(look_dx_, look_));
    s.thumb_ry = LargerMagnitude(s.thumb_ry, LookAxis(look_.invert_y ? look_dy_ : -look_dy_, look_));
    look_dx_ = look_dy_ = 0;
  }
  return s;
}

bool WindowToGuest(int32_t window_width, int32_t window_height, int32_t x, int32_t y, uint32_t guest_width,
                   uint32_t guest_height, GuestPoint* out) {
  if (window_width <= 0 || window_height <= 0 || guest_width == 0 || guest_height == 0) {
    return false;
  }
  // Same fit as rex::ui::Presenter (present_letterbox on, no overscan cutoff): the guest image keeps its
  // aspect ratio, fills one window dimension and is centred along the other.
  const uint64_t ww = uint64_t(window_width), wh = uint64_t(window_height);
  uint64_t out_w, out_h;
  int64_t out_x, out_y;
  if (ww * guest_height > wh * guest_width) {  // window wider: pillarbox
    out_h = wh;
    out_w = (wh * guest_width + guest_height / 2) / guest_height;
  } else {  // window taller (or equal): letterbox
    out_w = ww;
    out_h = (ww * guest_height + guest_width / 2) / guest_width;
  }
  out_x = (int64_t(ww) - int64_t(out_w)) / 2;
  out_y = (int64_t(wh) - int64_t(out_h)) / 2;
  const int64_t rx = int64_t(x) - out_x, ry = int64_t(y) - out_y;
  if (rx < 0 || ry < 0 || rx >= int64_t(out_w) || ry >= int64_t(out_h)) {
    return false;
  }
  out->x = static_cast<int32_t>(rx * guest_width / out_w);
  out->y = static_cast<int32_t>(ry * guest_height / out_h);
  return true;
}

}  // namespace kuf::input
