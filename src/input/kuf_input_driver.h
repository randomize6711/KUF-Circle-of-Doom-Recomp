// Keyboard and mouse input driver (M4): a synthetic gamepad for guest user 0, fed by window key, mouse and
// focus events through KeyboardPad. Physical controllers stay with the SDK's SDL driver; InputSystem merges
// both into what XamInputGetState returns. See docs/INPUT.md.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include <rex/input/input_driver.h>
#include <rex/system/xtypes.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "input/keyboard_pad.h"

namespace kuf::input {

using rex::X_RESULT;
using rex::X_STATUS;

class KufInputDriver final : public rex::input::InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  KufInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~KufInputDriver() override;

  // Builds the bindings from the kuf_bind_* cvars (defaults where empty). Fails on unknown key names.
  X_STATUS Setup() override;
  void OnWindowAvailable(rex::ui::Window* window) override;

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out_keystroke) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;
  // WindowListener
  void OnGotFocus(rex::ui::UISetupEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnClosing(rex::ui::UIEvent& e) override;

  // Scripted input (src/input/input_script.cpp) enters here, through the same KeyboardPad as window events.
  void InjectKey(HostKey key, bool down);
  void InjectWheel(int detents);
  void InjectFocus(bool focused);
  void InjectMouseMove(int32_t window_x, int32_t window_y, int32_t window_width, int32_t window_height);
  void InjectLook(double dx, double dy);  // mouse-look movement, as from a captured pointer

  // Gameplay keys from pc_settings.toml (ControlName -> key list; M7 feature 5): the defaults, then these, then the
  // --kuf_bind_game_* cvars. Invalid lists keep the default (logged).
  void SetGameplayKeys(const std::map<std::string, std::string>& keys);
  // The keys a control has in gameplay now (defaults, file and cvars).
  std::vector<HostKey> GameplayKeys(Control control) const;
  // All bindings now (the button prompts, M7 feature 6).
  Bindings CurrentBindings() const;

  // Key capture for the Key Bindings page (M7): from BeginKeyCapture on, the next key or mouse button pressed (not
  // a repeat) is kept for TakeKeyCapture and does not reach the game; nor does any other input until EndKeyCapture
  // (the pointer does not move the menu's selection either).
  void BeginKeyCapture();
  HostKey TakeKeyCapture();  // 0 while none
  void EndKeyCapture();

  // True while the pointer is captured for mouse look (gameplay).
  bool MouseCaptured() const;

  // Modern camera (M6B): mouse look goes to the camera directly instead of becoming a right-stick
  // deflection. TakeMouseLook returns the movement since the last call (window pixels, +y down).
  void SetDirectMouseLook(bool direct);
  void TakeMouseLook(double* dx, double* dy);

  // The window the driver listens to (null before the window exists); for the input script.
  rex::ui::Window* window() const;

  // The menu pointer's input since the last call (M7 UX fix 1; game thread, at the tick's start, src/runtime/
  // menu_pointer.cpp OnTick). `active`: the menus have the pointer (menu context, --kuf_menu_pointer, focus, no
  // overlay or key capture); then `moved` and `click` are taken (cleared) here.
  struct PointerWork {
    bool active = false;
    GuestPoint at;
    bool moved = false;  // the pointer moved since the last call
    bool click = false;  // a left click (press) to decide
    bool held = false;   // the left button is down (the press went to the menu pointer)
    std::chrono::steady_clock::time_point moved_at;  // the last movement (latency diagnostics)
  };
  PointerWork TakePointerWork();
  // One press of a control as a short tap (the pointer's confirm, and its fallback steps).
  void PulseControl(Control control);
  bool PendingPulses() const;

  // Last pointer position over the guest image, in guest pixels (false while outside it).
  bool PointerInGuest(GuestPoint* out) const;

  // The driver instance the game uses (for the input script); null before Setup.
  static KufInputDriver* Instance();

 private:
  void UpdatePointer(int32_t x, int32_t y, int32_t window_width, int32_t window_height);
  // At each poll: a click left for the menu pointer when the game is no longer in a menu is the plain A it was.
  void MenuPointerLocked();
  // Left button (window or script): true if the menu pointer takes it (the pad does not see it).
  bool MenuPointerButtonLocked(bool down);
  void SyncContextLocked();
  // A key or mouse button down while capturing: kept (the first) and true (the game does not see it).
  bool CaptureLocked(HostKey key);
  bool BuildBindingsLocked(const std::map<std::string, std::string>& keys, Bindings* out);
  void UpdateCaptureLocked(bool want);
  void Detach();

  mutable std::mutex mutex_;
  KeyboardPad pad_;
  PadState last_state_;
  uint32_t packet_number_ = 1;
  int32_t wheel_accum_ = 0;
  bool pointer_valid_ = false;
  GuestPoint pointer_;
  bool pointer_moved_ = false;    // moved since the last menu-pointer plan
  std::chrono::steady_clock::time_point moved_at_;  // the last pointer movement
  bool click_pending_ = false;    // a left click for the menu pointer (decided at the next tick)
  bool left_taken_ = false;       // the left button's press went to the menu pointer (its release too)
  rex::ui::Window* attached_window_ = nullptr;
  bool key_capture_ = false;      // the Key Bindings page waits for a key
  HostKey captured_key_ = 0;
  std::map<std::string, std::string> gameplay_keys_;  // from pc_settings.toml
  bool capture_requested_ = false;
  bool mouse_captured_ = false;
  rex::ui::Window::CursorVisibility saved_cursor_ = rex::ui::Window::CursorVisibility::kVisible;
};

}  // namespace kuf::input
