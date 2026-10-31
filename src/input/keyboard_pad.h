// Keyboard and mouse -> Xbox 360 gamepad translation (M4, gameplay in M5A). Pure logic, no window or SDK
// runtime state, so it is unit-tested directly (tests/input).
//
// The game reads input only through XamInputGetState, once per frame, and edge-detects buttons itself
// (sub_825EAD58: a press is a 0->1 change between two polls). KeyboardPad therefore turns host events into a
// gamepad *state* that the guest polls, with these rules:
//   - three contexts with their own bindings, reported by the game itself (game_input_hooks.cpp): the
//     "Press START" prompt, menus (including the in-game status/options menus), and gameplay (the player
//     character has control). Enter means Start at the prompt and A in menus; right click is B (back) in
//     menus and the X weapon in gameplay; and so on (docs/INPUT.md);
//   - every key keeps the meaning it had when it was pressed until it is released, so a key held across a
//     screen change never turns into a different action (in M4 a delayed A answered a dialog);
//   - a key held down keeps its control pressed; OS key repeat is ignored (no repeated presses);
//   - a key pressed and released between two polls is still reported pressed for one poll (latched), so short
//     taps are never lost; the release follows on the next poll;
//   - several keys bound to one control: the control is released only when all of them are up;
//   - focus loss releases everything (no stuck keys when key-up events go to another window);
//   - each mouse-wheel detent is one discrete press of the bound control, separated by a released poll, so
//     the game sees one edge per detent;
//   - mouse look (gameplay): pointer movement since the last poll becomes a right-stick deflection; or, with
//     the modern camera (M6B), it is handed to the camera directly (TakeLook), never as a stick.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace kuf::input {

// Logical controls of an Xbox 360 gamepad as the keyboard can drive them.
enum class Control : uint8_t {
  kA, kB, kX, kY,
  kLeftShoulder, kRightShoulder, kBack, kStart, kLeftThumb, kRightThumb,
  kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
  kLeftTrigger, kRightTrigger,
  kLeftStickUp, kLeftStickDown, kLeftStickLeft, kLeftStickRight,
  kRightStickUp, kRightStickDown, kRightStickLeft, kRightStickRight,
  kCount,
};
constexpr size_t kControlCount = static_cast<size_t>(Control::kCount);

// Name used in cvars and logs ("a", "start", "dpad_up", ...).
const char* ControlName(Control control);

// Where the game is, as far as input is concerned (game_input_hooks.h GameContext has the same meaning).
enum class Context : uint8_t { kMenu, kStartPrompt, kGameplay, kCount };
constexpr size_t kContextCount = static_cast<size_t>(Context::kCount);

// Host inputs that can be bound: virtual-key codes (rex::ui::VirtualKey values, < 0x100) and mouse buttons.
// Mouse buttons use the VirtualKey codes of kLButton/kRButton/kMButton/kXButton1/kXButton2.
using HostKey = uint16_t;

// Gamepad state in host byte order (the driver converts to the guest's big-endian X_INPUT_GAMEPAD).
struct PadState {
  uint16_t buttons = 0;  // X_INPUT_GAMEPAD_* bits
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
  bool operator==(const PadState&) const = default;
};

// X_INPUT_GAMEPAD button bits (same values as rex/input/input.h; repeated here to keep this header pure).
namespace buttons {
constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080;
constexpr uint16_t kLeftShoulder = 0x0100, kRightShoulder = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
}  // namespace buttons

class Bindings {
 public:
  // The documented defaults (docs/INPUT.md). The start-prompt context is the menu bindings, except that the
  // prompt's Start keys (Enter, left click, F1, Pause) mean only Start there.
  static Bindings Defaults();

  // Replaces the keys of one control in one context from a comma-separated list of key names
  // ("Return,Space"); an empty string unbinds it. Returns false (and changes nothing) if a name is unknown.
  bool Set(Context context, Control control, std::string_view key_list, std::string* error = nullptr);

  // Rebuilds the start-prompt context from the menu context and the given Start keys (see Defaults).
  void DerivePromptContext(const std::vector<HostKey>& start_keys);

  const std::vector<HostKey>& keys(Context context, Control control) const {
    return keys_[static_cast<size_t>(context)][static_cast<size_t>(control)];
  }
  // Bitmask (1 << Control) of the controls `key` drives in `context`.
  uint32_t ControlsFor(Context context, HostKey key) const;

 private:
  std::array<std::array<std::vector<HostKey>, kControlCount>, kContextCount> keys_;
};

// Parses one key name (the SDK's names: "Return", "Escape", "Up", "A", "Numpad8", ...; plus "MouseLeft",
// "MouseRight", "MouseMiddle", "MouseX1", "MouseX2"). Returns 0 for unknown names.
HostKey ParseHostKey(std::string_view name);
// Parses a comma-separated key list; false (with *error) on an unknown name.
bool ParseHostKeys(std::string_view key_list, std::vector<HostKey>* out, std::string* error = nullptr);
// The name ParseHostKey reads ("Return", "MouseLeft"); empty for a key without one.
std::string HostKeyName(HostKey key);
// A shorter name for menus ("Mouse L" for MouseLeft, "Mouse 4" for MouseX1, "Enter", "Esc"; others as HostKeyName).
std::string HostKeyLabel(HostKey key);

// Mouse look: pointer movement (window pixels) since the last poll -> right-stick deflection.
// Measured in M5A (docs/INPUT.md "Mouse look"): the game turns the camera only above a right-stick deflection
// of about 12,900 and then roughly linearly, 0.00194 screen px per frame per unit, up to about 39 px per frame
// at full deflection. Starting at the dead zone and adding 260 per pixel makes the camera turn about 0.5
// screen px per pixel of mouse movement at sensitivity 1, proportionally, until the game's maximum turn rate.
struct LookSettings {
  double gain = 260.0;       // stick units per pixel of movement per poll, before sensitivity
  double sensitivity = 1.0;  // --kuf_mouse_sensitivity
  int32_t deadzone = 12900;  // the game's own camera dead zone (measured)
  bool invert_y = false;     // on top of the game's own Camera Normal/Inverted setting
  bool direct = false;       // modern camera: movement is taken with TakeLook, not turned into a stick
};
// Pure conversion, exposed for tests: one axis.
int16_t LookAxis(double pixels, const LookSettings& settings);

class KeyboardPad {
 public:
  explicit KeyboardPad(Bindings bindings = Bindings::Defaults()) : bindings_(std::move(bindings)) {}

  const Bindings& bindings() const { return bindings_; }
  void set_bindings(Bindings bindings);

  // The context for the next key presses (keys already down keep their press-time meaning). Leaving
  // gameplay drops pending mouse-look movement.
  void set_context(Context context);
  Context context() const { return context_; }

  // Host events. `repeat` marks OS auto-repeat (ignored for state; never a new press).
  void KeyDown(HostKey key, bool repeat = false);
  void KeyUp(HostKey key);
  // Mouse wheel: positive = away from the user (up). Each detent queues one press of wheel_up/wheel_down.
  void Wheel(int detents);
  void set_wheel_controls(Control up, Control down) { wheel_up_ = up; wheel_down_ = down; }
  // One discrete press of `control` (a poll pressed, then a poll released), queued after any wheel presses: the
  // menu pointer's steps and its deferred confirm (src/runtime/menu_pointer.cpp).
  void Pulse(Control control);
  size_t pending_pulses() const { return wheel_pulses_.size() + (wheel_gap_ ? 1 : 0); }
  // Mouse look movement in window pixels (only accumulated in gameplay).
  void Look(double dx, double dy);
  void set_look_settings(const LookSettings& settings) { look_ = settings; }
  const LookSettings& look_settings() const { return look_; }
  // Direct mouse look: the movement accumulated in gameplay since the last call (window pixels; +x right,
  // +y down), then cleared.
  void TakeLook(double* dx, double* dy);
  // Window focus. Losing focus releases every key and drops pending latches, wheel pulses and look movement.
  void SetFocused(bool focused);
  bool focused() const { return focused_; }

  // The state the guest sees at this poll. Consumes latched taps, wheel pulses and look movement, so call it
  // once per guest poll.
  PadState Poll();

  // True while any key is held (for diagnostics and tests).
  bool AnyHeld() const;

 private:
  Bindings bindings_;
  Context context_ = Context::kMenu;
  std::array<bool, 0x100> down_{};
  std::array<uint32_t, 0x100> pressed_{};  // per held key: the controls it drives (fixed at press time)
  uint32_t latched_ = 0;                   // controls pressed since the last poll
  std::deque<Control> wheel_pulses_;
  bool wheel_gap_ = false;  // a pulse was reported last poll; report released once before the next
  Control wheel_up_ = Control::kDpadUp;
  Control wheel_down_ = Control::kDpadDown;
  double look_dx_ = 0, look_dy_ = 0;
  LookSettings look_;
  bool focused_ = true;
};

// Maps a pointer position in window pixels to guest output pixels, mirroring the presenter's placement of the
// guest image (aspect-preserving fit, centred, letterbox or pillarbox; the default without overscan cutoff).
// Returns false when the point lies outside the guest image (on a letterbox bar) or the window is empty.
struct GuestPoint {
  int32_t x = 0, y = 0;
};
bool WindowToGuest(int32_t window_width, int32_t window_height, int32_t x, int32_t y, uint32_t guest_width,
                   uint32_t guest_height, GuestPoint* out);

}  // namespace kuf::input
