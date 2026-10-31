// The controller driver with the player's button layout (M7 feature 5): wraps the SDK's SDL driver, applies the
// PadRemap (pad_remap.h) while the player has control, and captures the next button for the PC Settings
// Controller Buttons page. Everything else passes through unchanged.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <rex/input/input_driver.h>
#include <rex/system/xtypes.h>

#include "input/pad_remap.h"
#include "runtime/pc_settings.h"

namespace kuf::input {

using rex::X_RESULT;
using rex::X_STATUS;

class PadRemapDriver final : public rex::input::InputDriver {
 public:
  explicit PadRemapDriver(std::unique_ptr<rex::input::InputDriver> inner);
  ~PadRemapDriver() override;

  X_STATUS Setup() override;
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override { inner_->EnumerateDevices(out); }
  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    return inner_->GetDeviceCapabilities(id, flags, out_caps);
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override {
    return inner_->SetDeviceVibration(id, vibration);
  }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out_keystroke) override {
    return inner_->GetDeviceKeystroke(id, flags, out_keystroke);
  }

  // A controller is connected.
  bool HasDevices();
  // The layout from now on (any thread).
  void SetRemap(const PadRemap& remap);
  // Capture: from now on no controller input reaches the game; the next button pressed on any controller (not one
  // held when the capture began) is returned once by TakeCapture. EndCapture gives the input back.
  void BeginCapture();
  std::optional<PadButton> TakeCapture();
  void EndCapture();

  static PadRemapDriver* Instance();

 private:
  std::unique_ptr<rex::input::InputDriver> inner_;
  std::mutex mutex_;
  PadRemap remap_;
  bool capturing_ = false;
  std::optional<PadButton> captured_;
  std::unordered_map<uint64_t, PadState> held_at_begin_;  // per device: unknown devices start from released
};

// pc_settings.toml's controller_buttons as a remap (the default layout if they are not one permutation).
PadRemap RemapFromSettings(const kuf::pc::BindingSettings& bindings);

// The gameplay keys and controller buttons of pc_settings.toml (kuf::pc::Current().bindings) applied to the drivers;
// the menu calls it after a change. Command-line --kuf_bind_game_* still win.
void ApplyBindingSettings();

}  // namespace kuf::input
