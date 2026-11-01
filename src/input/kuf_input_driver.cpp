#include "input/kuf_input_driver.h"

#include "input/game_input_hooks.h"
#include "input/key_routing.h"
#include "runtime/menu_pointer.h"
#include "runtime/pc_settings.h"
#include "runtime/prompt_text.h"

#include <atomic>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <SDL3/SDL_clipboard.h>

REXCVAR_DEFINE_BOOL(kuf_trace_text_input, false, "KUF/Debug",
                    "Log the text fields' input lifecycle: editing on/off, SDL text input, events received and "
                    "delivered (counts only: never the characters) ([kuf-text])");
REXCVAR_DEFINE_BOOL(kuf_keyboard, true, "KUF/Input", "Keyboard and mouse drive guest user 0 as a gamepad");
REXCVAR_DEFINE_BOOL(kuf_mouse_wheel, true, "KUF/Input", "Mouse wheel presses D-pad up/down, one press per detent");
REXCVAR_DEFINE_BOOL(kuf_menu_pointer, true, "KUF/Input",
                    "In the game's menus the pointer selects the row under it and a left click confirms it (M7)");
REXCVAR_DEFINE_BOOL(kuf_mouse_look, true, "KUF/Input",
                    "In gameplay, capture the pointer and turn the camera with the mouse (released in menus)");
REXCVAR_DEFINE_DOUBLE(kuf_mouse_sensitivity, 1.0, "KUF/Input", "Mouse look sensitivity");
REXCVAR_DEFINE_BOOL(kuf_mouse_invert_y, false, "KUF/Input",
                    "Invert mouse look vertically (on top of the game's own Camera setting)");
REXCVAR_DEFINE_STRING(kuf_bind_prompt_start, "Return,NumpadEnter,MouseLeft,F1,Pause", "KUF/Input/Keys",
                      "Keys that press Start (and nothing else) while the game waits for START");

// Two cvars per control, menus and gameplay: a comma-separated key list replacing the default
// (docs/INPUT.md); empty = default.
#define KUF_BIND_CVAR(name)                                                                                \
  REXCVAR_DEFINE_STRING(kuf_bind_##name, "", "KUF/Input/Keys", "Menu keys for " #name " (empty = default)"); \
  REXCVAR_DEFINE_STRING(kuf_bind_game_##name, "", "KUF/Input/Keys",                                          \
                        "Gameplay keys for " #name " (empty = default)");
KUF_BIND_CVAR(a)
KUF_BIND_CVAR(b)
KUF_BIND_CVAR(x)
KUF_BIND_CVAR(y)
KUF_BIND_CVAR(left_shoulder)
KUF_BIND_CVAR(right_shoulder)
KUF_BIND_CVAR(back)
KUF_BIND_CVAR(start)
KUF_BIND_CVAR(left_thumb)
KUF_BIND_CVAR(right_thumb)
KUF_BIND_CVAR(dpad_up)
KUF_BIND_CVAR(dpad_down)
KUF_BIND_CVAR(dpad_left)
KUF_BIND_CVAR(dpad_right)
KUF_BIND_CVAR(left_trigger)
KUF_BIND_CVAR(right_trigger)
KUF_BIND_CVAR(lstick_up)
KUF_BIND_CVAR(lstick_down)
KUF_BIND_CVAR(lstick_left)
KUF_BIND_CVAR(lstick_right)
KUF_BIND_CVAR(rstick_up)
KUF_BIND_CVAR(rstick_down)
KUF_BIND_CVAR(rstick_left)
KUF_BIND_CVAR(rstick_right)
#undef KUF_BIND_CVAR

namespace kuf::input {
namespace {

using rex::input::DeviceId;
using rex::ui::VirtualKey;

// One device, so a constant handle ("KUFK").
constexpr DeviceId kDevice = static_cast<DeviceId>(0x4B55464BULL);

// The game renders 1280x720 (XGetVideoMode reports 720p); the pointer maps onto that image.
constexpr uint32_t kGuestWidth = 1280;
constexpr uint32_t kGuestHeight = 720;

std::atomic<KufInputDriver*> g_instance{nullptr};

std::string BindCvar(Context context, Control control) {
  const bool game = context == Context::kGameplay;
  switch (control) {
#define CASE(c, name) \
  case Control::c: return game ? REXCVAR_GET(kuf_bind_game_##name) : REXCVAR_GET(kuf_bind_##name);
    CASE(kA, a) CASE(kB, b) CASE(kX, x) CASE(kY, y)
    CASE(kLeftShoulder, left_shoulder) CASE(kRightShoulder, right_shoulder) CASE(kBack, back)
    CASE(kStart, start) CASE(kLeftThumb, left_thumb) CASE(kRightThumb, right_thumb)
    CASE(kDpadUp, dpad_up) CASE(kDpadDown, dpad_down) CASE(kDpadLeft, dpad_left) CASE(kDpadRight, dpad_right)
    CASE(kLeftTrigger, left_trigger) CASE(kRightTrigger, right_trigger)
    CASE(kLeftStickUp, lstick_up) CASE(kLeftStickDown, lstick_down)
    CASE(kLeftStickLeft, lstick_left) CASE(kLeftStickRight, lstick_right)
    CASE(kRightStickUp, rstick_up) CASE(kRightStickDown, rstick_down)
    CASE(kRightStickLeft, rstick_left) CASE(kRightStickRight, rstick_right)
#undef CASE
    default: return {};
  }
}

Context ToContext(GameContext c) {
  switch (c) {
    case GameContext::kStartPrompt: return Context::kStartPrompt;
    case GameContext::kGameplay: return Context::kGameplay;
    default: return Context::kMenu;
  }
}

HostKey MouseKey(rex::ui::MouseEvent::Button button) {
  switch (button) {
    case rex::ui::MouseEvent::Button::kLeft: return static_cast<HostKey>(VirtualKey::kLButton);
    case rex::ui::MouseEvent::Button::kRight: return static_cast<HostKey>(VirtualKey::kRButton);
    case rex::ui::MouseEvent::Button::kMiddle: return static_cast<HostKey>(VirtualKey::kMButton);
    case rex::ui::MouseEvent::Button::kX1: return static_cast<HostKey>(VirtualKey::kXButton1);
    case rex::ui::MouseEvent::Button::kX2: return static_cast<HostKey>(VirtualKey::kXButton2);
    default: return 0;
  }
}

}  // namespace

KufInputDriver::KufInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

KufInputDriver::~KufInputDriver() {
  Detach();
  KufInputDriver* self = this;
  g_instance.compare_exchange_strong(self, nullptr);
}

KufInputDriver* KufInputDriver::Instance() { return g_instance.load(); }

bool KufInputDriver::BuildBindingsLocked(const std::map<std::string, std::string>& file_keys, Bindings* out) {
  Bindings bindings = Bindings::Defaults();
  for (size_t c = 0; c < kControlCount; ++c) {  // pc_settings.toml's gameplay keys (checked when read)
    const auto control = static_cast<Control>(c);
    if (const auto it = file_keys.find(ControlName(control)); it != file_keys.end()) {
      std::string error;
      if (!bindings.Set(Context::kGameplay, control, it->second, &error)) {
        REXLOG_WARN("[kuf-input] pc_settings.toml gameplay_keys: {} (default kept)", error);
      }
    }
  }
  for (Context context : {Context::kMenu, Context::kGameplay}) {
    for (size_t c = 0; c < kControlCount; ++c) {
      const auto control = static_cast<Control>(c);
      const std::string keys = BindCvar(context, control);
      std::string error;
      if (!keys.empty() && !bindings.Set(context, control, keys, &error)) {
        REXLOG_ERROR("[kuf-input] {}", error);
        return false;
      }
    }
  }
  std::vector<HostKey> prompt_start;
  std::string error;
  if (!ParseHostKeys(REXCVAR_GET(kuf_bind_prompt_start), &prompt_start, &error)) {
    REXLOG_ERROR("[kuf-input] {} in kuf_bind_prompt_start", error);
    return false;
  }
  bindings.DerivePromptContext(prompt_start);
  *out = std::move(bindings);
  return true;
}

void KufInputDriver::SetGameplayKeys(const std::map<std::string, std::string>& keys) {
  std::lock_guard lock(mutex_);
  if (keys == gameplay_keys_) return;
  Bindings bindings;
  if (!BuildBindingsLocked(keys, &bindings)) return;
  gameplay_keys_ = keys;
  pad_.set_bindings(std::move(bindings));
  REXLOG_INFO("[kuf-input] gameplay keys: {} changed from the defaults", keys.size());
}

std::vector<HostKey> KufInputDriver::GameplayKeys(Control control) const {
  std::lock_guard lock(mutex_);
  return pad_.bindings().keys(Context::kGameplay, control);
}

Bindings KufInputDriver::CurrentBindings() const {
  std::lock_guard lock(mutex_);
  return pad_.bindings();
}

void KufInputDriver::BeginKeyCapture() {
  std::lock_guard lock(mutex_);
  key_capture_ = true;
  captured_key_ = 0;
  click_pending_ = false;
}

HostKey KufInputDriver::TakeKeyCapture() {
  std::lock_guard lock(mutex_);
  const HostKey key = captured_key_;
  captured_key_ = 0;
  return key;
}

void KufInputDriver::EndKeyCapture() {
  std::lock_guard lock(mutex_);
  key_capture_ = false;
  captured_key_ = 0;
  pointer_moved_ = false;  // the pointer's position while capturing does not select a row afterwards
}

bool KufInputDriver::CaptureLocked(HostKey key) {
  if (!key_capture_) return false;
  if (!captured_key_) captured_key_ = key;
  return true;
}

X_STATUS KufInputDriver::Setup() {
  Bindings bindings;
  {
    std::lock_guard lock(mutex_);
    gameplay_keys_ = kuf::pc::Current().bindings.keys;
    if (!BuildBindingsLocked(gameplay_keys_, &bindings)) return X_STATUS_INVALID_PARAMETER;
  }
  if (!gameplay_keys_.empty()) {
    REXLOG_INFO("[kuf-input] gameplay keys: {} changed from the defaults (pc_settings.toml)", gameplay_keys_.size());
  }
  LookSettings look;
  look.sensitivity = REXCVAR_GET(kuf_mouse_sensitivity);
  look.invert_y = REXCVAR_GET(kuf_mouse_invert_y);
  {
    std::lock_guard lock(mutex_);
    pad_.set_bindings(std::move(bindings));
    pad_.set_look_settings(look);
  }
  g_instance = this;
  REXLOG_INFO("[kuf-input] keyboard/mouse gamepad {} (guest user 0), mouse look {}",
              REXCVAR_GET(kuf_keyboard) ? "enabled" : "disabled", REXCVAR_GET(kuf_mouse_look) ? "on" : "off");
  return X_STATUS_SUCCESS;
}

void KufInputDriver::SyncContextLocked() { pad_.set_context(ToContext(CurrentGameContext())); }

void KufInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (!window) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    attached_window_ = window;
  }
  window->AddInputListener(this, window_z_order());
  char_listener_.driver = this;
  window->AddInputListener(&char_listener_, kCharListenerZOrder);
  window->AddListener(this);
}

void KufInputDriver::Detach() {
  rex::ui::Window* window;
  bool was_captured;
  {
    std::lock_guard lock(mutex_);
    window = attached_window_;
    attached_window_ = nullptr;
    was_captured = mouse_captured_;
    mouse_captured_ = capture_requested_ = false;
  }
  if (window && was_captured) {
    window->SetRelativeMouseMode(false);
    window->ReleaseMouse();
  }
  if (window) {
    window->RemoveInputListener(&char_listener_);
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  }
}

void KufInputDriver::OnClosing(rex::ui::UIEvent&) { Detach(); }

void KufInputDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  if (!REXCVAR_GET(kuf_keyboard)) {
    return;
  }
  rex::input::DeviceInfo info;
  info.id = kDevice;
  info.name = "Keyboard and Mouse";
  info.synthetic = true;  // feeds guest user 0 without taking a controller's slot
  out.push_back(info);
}

X_RESULT KufInputDriver::GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) {
  if (id != kDevice || !REXCVAR_GET(kuf_keyboard)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(mutex_);
  SyncContextLocked();
  MenuPointerLocked();
  // While a runtime overlay has the input (is_active false), the game sees a neutral pad; the poll still
  // runs so taps made for the overlay do not reach the game afterwards.
  PadState s = pad_.Poll();
  if (!is_active()) {
    s = PadState{};
  }
  // Mouse look only while the player has control: the pointer is captured in gameplay and free in menus.
  UpdateCaptureLocked(REXCVAR_GET(kuf_mouse_look) && pad_.focused() && is_active() &&
                      pad_.context() == Context::kGameplay);
  if (!(s == last_state_)) {
    last_state_ = s;
    ++packet_number_;
  }
  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = s.buttons;
    out_state->gamepad.left_trigger = s.left_trigger;
    out_state->gamepad.right_trigger = s.right_trigger;
    out_state->gamepad.thumb_lx = s.thumb_lx;
    out_state->gamepad.thumb_ly = s.thumb_ly;
    out_state->gamepad.thumb_rx = s.thumb_rx;
    out_state->gamepad.thumb_ry = s.thumb_ry;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT KufInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t /*flags*/,
                                               rex::input::X_INPUT_CAPABILITIES* out_caps) {
  if (id != kDevice || !REXCVAR_GET(kuf_keyboard)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xF3FF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT KufInputDriver::SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION* /*vibration*/) {
  // Accepted and ignored: a keyboard has no motors.
  return id == kDevice && REXCVAR_GET(kuf_keyboard) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT KufInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t /*flags*/,
                                            rex::input::X_INPUT_KEYSTROKE* /*out_keystroke*/) {
  // The game never calls XamInputGetKeystroke (config/v1/imports.txt).
  return id == kDevice && REXCVAR_GET(kuf_keyboard) ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

void KufInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  std::lock_guard lock(mutex_);
  SyncContextLocked();
  // prev_state: the key was already down (OS auto-repeat).
  const bool repeat = e.prev_state() || e.repeat_count() > 1;
  kuf::prompts::NoteDevice(kuf::input::PromptDevice::kKeyboard);
  if (key_capture_) {
    if (!repeat) CaptureLocked(static_cast<HostKey>(e.virtual_key()));
    return;
  }
  // A text field being edited takes the key, else the keyboard's emulated controller does (never both).
  RouteKeyDown(text_, pad_, uint32_t(e.virtual_key()), e.is_ctrl_pressed(), e.is_alt_pressed(), repeat, [] {
    // OnKeyDown runs on the UI thread, where the clipboard is read.
    std::string out;
    if (char* clip = SDL_GetClipboardText()) {
      out = clip;
      SDL_free(clip);
    }
    return out;
  });
}

void KufInputDriver::CharListener::OnKeyChar(rex::ui::KeyEvent& e) {
  std::lock_guard lock(driver->mutex_);
  if (!driver->text_.active()) return;  // not ours: the overlays' (their console)
  const uint32_t before = driver->text_.chars_seen();
  driver->text_.Char(uint32_t(e.virtual_key()));
  e.set_handled(true);
  if (REXCVAR_GET(kuf_trace_text_input) && before == 0 && driver->text_.chars_seen() == 1) {
    REXLOG_INFO("[kuf-text] text input events arriving (characters not logged)");
  }
}

void KufInputDriver::BeginTextEntry() {
  rex::ui::Window* w;
  {
    std::lock_guard lock(mutex_);
    if (text_.active()) return;
    text_.Begin();
    click_pending_ = false;
    w = attached_window_;
  }
  // The platform's text input (SDL_StartTextInput) is off unless asked for: without it no character arrives.
  if (w) {
    w->app_context().CallInUIThreadDeferred([w] {
      w->SetTextInputActive(true);
      if (REXCVAR_GET(kuf_trace_text_input)) {
        REXLOG_INFO("[kuf-text] editing: text input on ({})", w->IsTextInputActive() ? "SDL text input started" : "not started");
      }
    });
  }
}

std::vector<TextEvent> KufInputDriver::TakeTextEntry() {
  std::lock_guard lock(mutex_);
  return text_.Take();
}

void KufInputDriver::EndTextEntry() {
  rex::ui::Window* w;
  {
    std::lock_guard lock(mutex_);
    if (!text_.active()) return;
    if (REXCVAR_GET(kuf_trace_text_input)) {
      REXLOG_INFO("[kuf-text] editing ended: {} text input character(s) received; text input off", text_.chars_seen());
    }
    text_.End();
    w = attached_window_;
  }
  if (w) w->app_context().CallInUIThreadDeferred([w] { w->SetTextInputActive(false); });
}

bool KufInputDriver::TextEntryActive() const {
  std::lock_guard lock(mutex_);
  return text_.active();
}

void KufInputDriver::CopyToClipboard(const std::string& text) {
  rex::ui::Window* w = window();
  if (!w) return;
  w->app_context().CallInUIThreadDeferred([text] { SDL_SetClipboardText(text.c_str()); });
}

void KufInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  std::lock_guard lock(mutex_);
  pad_.KeyUp(static_cast<HostKey>(e.virtual_key()));
}

void KufInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  std::lock_guard lock(mutex_);
  UpdatePointer(e.x(), e.y(), e.target() ? int32_t(e.target()->GetActualPhysicalWidth()) : 0,
                e.target() ? int32_t(e.target()->GetActualPhysicalHeight()) : 0);
  if (HostKey key = MouseKey(e.button())) {
    kuf::prompts::NoteDevice(kuf::input::PromptDevice::kKeyboard);
    SyncContextLocked();
    if (CaptureLocked(key)) {
      return;
    }
    if (key == static_cast<HostKey>(VirtualKey::kLButton) && MenuPointerButtonLocked(true)) {
      return;
    }
    pad_.KeyDown(key);
  }
}

void KufInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  std::lock_guard lock(mutex_);
  if (HostKey key = MouseKey(e.button())) {
    if (key == static_cast<HostKey>(VirtualKey::kLButton) && MenuPointerButtonLocked(false)) {
      return;
    }
    pad_.KeyUp(key);
  }
}

void KufInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  std::lock_guard lock(mutex_);
  if (mouse_captured_) {
    // Relative mode: the pointer is locked and only the motion matters.
    pad_.Look(e.dx(), e.dy());
    return;
  }
  UpdatePointer(e.x(), e.y(), e.target() ? int32_t(e.target()->GetActualPhysicalWidth()) : 0,
                e.target() ? int32_t(e.target()->GetActualPhysicalHeight()) : 0);
  pointer_moved_ = true;
  moved_at_ = std::chrono::steady_clock::now();
  kuf::prompts::NoteDevice(kuf::input::PromptDevice::kKeyboard);  // moving the mouse is using the keyboard and mouse
}

void KufInputDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!REXCVAR_GET(kuf_mouse_wheel)) {
    return;
  }
  std::lock_guard lock(mutex_);
  // Touchpads scroll in fractions of a detent: accumulate, one press per whole detent.
  constexpr int32_t kDetent = int32_t(rex::ui::MouseEvent::kScrollPerDetent);
  wheel_accum_ += e.scroll_y();
  kuf::prompts::NoteDevice(kuf::input::PromptDevice::kKeyboard);
  const int32_t detents = wheel_accum_ / kDetent;
  wheel_accum_ -= detents * kDetent;
  SyncContextLocked();
  if (!key_capture_) pad_.Wheel(detents);
}

void KufInputDriver::OnGotFocus(rex::ui::UISetupEvent&) { InjectFocus(true); }

void KufInputDriver::OnLostFocus(rex::ui::UISetupEvent&) { InjectFocus(false); }

void KufInputDriver::InjectKey(HostKey key, bool down) {
  std::lock_guard lock(mutex_);
  SyncContextLocked();
  if (down) kuf::prompts::NoteDevice(kuf::input::PromptDevice::kKeyboard);
  if (down && CaptureLocked(key)) {
    return;  // the Key Bindings page takes it
  }
  if (key == static_cast<HostKey>(VirtualKey::kLButton) && MenuPointerButtonLocked(down)) {
    return;  // the script's left click, as a window's
  }
  if (down) {
    pad_.KeyDown(key);
  } else {
    pad_.KeyUp(key);
  }
}

void KufInputDriver::InjectWheel(int detents) {
  std::lock_guard lock(mutex_);
  kuf::prompts::NoteDevice(kuf::input::PromptDevice::kKeyboard);
  SyncContextLocked();
  if (!key_capture_) pad_.Wheel(detents);
}

void KufInputDriver::InjectFocus(bool focused) {
  std::lock_guard lock(mutex_);
  if (pad_.focused() != focused) {
    REXLOG_INFO("[kuf-input] window focus {}{}", focused ? "gained" : "lost",
                focused ? "" : ": all keys released");
  }
  pad_.SetFocused(focused);
}

void KufInputDriver::InjectMouseMove(int32_t x, int32_t y, int32_t window_width, int32_t window_height) {
  std::lock_guard lock(mutex_);
  UpdatePointer(x, y, window_width, window_height);
  pointer_moved_ = true;
  moved_at_ = std::chrono::steady_clock::now();
}

void KufInputDriver::UpdatePointer(int32_t x, int32_t y, int32_t window_width, int32_t window_height) {
  pointer_valid_ = WindowToGuest(window_width, window_height, x, y, kGuestWidth, kGuestHeight, &pointer_);
}

bool KufInputDriver::MenuPointerButtonLocked(bool down) {
  if (!down) {
    const bool taken = left_taken_;
    left_taken_ = false;
    return taken;
  }
  // Only where the left button confirms (A) in the menus: a rebinding that gives it another meaning keeps it.
  const HostKey left = static_cast<HostKey>(VirtualKey::kLButton);
  if (!REXCVAR_GET(kuf_menu_pointer) || pad_.context() != Context::kMenu || !pointer_valid_ || !pad_.focused() ||
      pad_.bindings().ControlsFor(Context::kMenu, left) != (1u << static_cast<uint32_t>(Control::kA))) {
    return false;
  }
  click_pending_ = true;  // decided at the next poll, on the game thread, against the menu as it is then
  left_taken_ = true;
  return true;
}

void KufInputDriver::MenuPointerLocked() {
  if (click_pending_ && (pad_.context() != Context::kMenu || !REXCVAR_GET(kuf_menu_pointer))) {
    // The context changed before the tick took it. At the start prompt it is the plain A it was; otherwise (the menu
    // closed: gameplay) it is dropped, so a click made in a menu never reaches the game behind it (M7 UI follow-up).
    click_pending_ = false;
    if (pad_.context() == Context::kStartPrompt) pad_.Pulse(Control::kA);
  }
}

KufInputDriver::PointerWork KufInputDriver::TakePointerWork() {
  std::lock_guard lock(mutex_);
  SyncContextLocked();
  PointerWork w;
  w.active = pad_.context() == Context::kMenu && REXCVAR_GET(kuf_menu_pointer) && is_active() && pad_.focused() &&
             pointer_valid_ && !key_capture_;
  if (!w.active) return w;
  w.at = pointer_;
  w.moved = pointer_moved_;
  w.click = click_pending_;
  w.held = left_taken_;
  w.moved_at = moved_at_;
  pointer_moved_ = click_pending_ = false;
  return w;
}

bool KufInputDriver::PendingPulses() const {
  std::lock_guard lock(mutex_);
  return pad_.pending_pulses() > 0;
}

void KufInputDriver::PulseControl(Control control) {
  std::lock_guard lock(mutex_);
  pad_.Pulse(control);
}

void KufInputDriver::InjectLook(double dx, double dy) {
  std::lock_guard lock(mutex_);
  SyncContextLocked();
  pad_.Look(dx, dy);
}

void KufInputDriver::SetDirectMouseLook(bool direct) {
  std::lock_guard lock(mutex_);
  if (pad_.look_settings().direct == direct) {
    return;
  }
  LookSettings look = pad_.look_settings();
  look.direct = direct;
  pad_.set_look_settings(look);
  REXLOG_INFO("[kuf-input] mouse look: {}", direct ? "direct to the camera (modern camera)" : "as the right stick");
}

void KufInputDriver::TakeMouseLook(double* dx, double* dy) {
  std::lock_guard lock(mutex_);
  pad_.TakeLook(dx, dy);
}

bool KufInputDriver::MouseCaptured() const {
  std::lock_guard lock(mutex_);
  return mouse_captured_;
}

void KufInputDriver::UpdateCaptureLocked(bool want) {
  if (want == capture_requested_) {
    return;
  }
  capture_requested_ = want;
  REXLOG_INFO("[kuf-input] mouse look {}", want ? "on: pointer captured" : "off: pointer released");
  rex::ui::Window* window = attached_window_;
  if (!window) {
    mouse_captured_ = want;  // no window (scripted tests): the state is still tracked
    return;
  }
  // Window calls belong on the UI thread; deferred, so this never re-enters mutex_ from here.
  window->app_context().CallInUIThreadDeferred([this, window, want] {
    std::lock_guard lock(mutex_);
    if (attached_window_ != window) {
      return;
    }
    if (want) {
      saved_cursor_ = window->GetCursorVisibility();
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      window->CaptureMouse();
      if (!window->SetRelativeMouseMode(true)) {
        REXLOG_WARN("[kuf-input] pointer lock unavailable: mouse look disabled");
        window->ReleaseMouse();
        window->SetCursorVisibility(saved_cursor_);
        return;
      }
      mouse_captured_ = true;
    } else if (mouse_captured_) {
      window->SetRelativeMouseMode(false);
      window->ReleaseMouse();
      window->SetCursorVisibility(saved_cursor_);
      mouse_captured_ = false;
    }
  });
}

rex::ui::Window* KufInputDriver::window() const {
  std::lock_guard lock(mutex_);
  return attached_window_;
}

bool KufInputDriver::PointerInGuest(GuestPoint* out) const {
  std::lock_guard lock(mutex_);
  if (pointer_valid_ && out) {
    *out = pointer_;
  }
  return pointer_valid_;
}

}  // namespace kuf::input
