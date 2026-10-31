#include "input/pad_remap_driver.h"

#include <array>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "input/game_input_hooks.h"
#include "input/kuf_input_driver.h"
#include "runtime/pc_settings.h"
#include "runtime/prompt_text.h"

REXCVAR_DEFINE_BOOL(kuf_trace_pad_remap, false, "KUF/Debug",
                    "Log each controller button change the gameplay layout remaps (tests/settings/bindings_game.toml)");

namespace kuf::input {
namespace {

std::atomic<PadRemapDriver*> g_instance{nullptr};

PadState FromGuest(const rex::input::X_INPUT_GAMEPAD& g) {
  PadState s;
  s.buttons = g.buttons;
  s.left_trigger = g.left_trigger;
  s.right_trigger = g.right_trigger;
  s.thumb_lx = g.thumb_lx;
  s.thumb_ly = g.thumb_ly;
  s.thumb_rx = g.thumb_rx;
  s.thumb_ry = g.thumb_ry;
  return s;
}

void ToGuest(const PadState& s, rex::input::X_INPUT_GAMEPAD* g) {
  g->buttons = s.buttons;
  g->left_trigger = s.left_trigger;
  g->right_trigger = s.right_trigger;
  g->thumb_lx = s.thumb_lx;
  g->thumb_ly = s.thumb_ly;
  g->thumb_rx = s.thumb_rx;
  g->thumb_ry = s.thumb_ry;
}

}  // namespace

PadRemap RemapFromSettings(const kuf::pc::BindingSettings& b) {
  std::array<PadButton, kPadButtonCount> sources;
  for (size_t i = 0; i < sources.size(); ++i) sources[i] = static_cast<PadButton>(i);
  for (const auto& [action, physical] : b.buttons) {  // validated by pc_settings (one permutation)
    const auto a = ParsePadButton(action), p = ParsePadButton(physical);
    if (a && p) sources[static_cast<size_t>(*a)] = *p;
  }
  PadRemap remap;
  if (!remap.Set(sources)) REXLOG_WARN("[kuf-input] controller buttons: not one permutation (default layout)");
  return remap;
}

PadRemapDriver::PadRemapDriver(std::unique_ptr<rex::input::InputDriver> inner)
    : InputDriver(nullptr, 0), inner_(std::move(inner)) {
  // The inner driver drops input while a runtime overlay has it, as this one would.
  inner_->set_is_active_callback([this] { return is_active(); });
}

PadRemapDriver::~PadRemapDriver() {
  PadRemapDriver* self = this;
  g_instance.compare_exchange_strong(self, nullptr);
}

PadRemapDriver* PadRemapDriver::Instance() { return g_instance.load(); }

X_STATUS PadRemapDriver::Setup() {
  const X_STATUS status = inner_->Setup();
  if (status == X_STATUS_SUCCESS) {
    SetRemap(RemapFromSettings(kuf::pc::Current().bindings));
    g_instance = this;
  }
  return status;
}

bool PadRemapDriver::HasDevices() {
  std::vector<rex::input::DeviceInfo> devices;
  inner_->EnumerateDevices(devices);
  return !devices.empty();
}

void PadRemapDriver::SetRemap(const PadRemap& remap) {
  std::lock_guard lock(mutex_);
  if (!(remap == remap_)) {
    std::string layout;
    for (size_t i = 0; i < kPadButtonCount; ++i) {
      const auto action = static_cast<PadButton>(i);
      if (remap.source(action) != action) {
        layout += std::string(layout.empty() ? "" : ", ") + PadButtonName(remap.source(action)) + " as " +
                  PadButtonName(action);
      }
    }
    REXLOG_INFO("[kuf-input] controller buttons in gameplay: {}", layout.empty() ? "default" : layout);
  }
  remap_ = remap;
}

void PadRemapDriver::BeginCapture() {
  std::lock_guard lock(mutex_);
  capturing_ = true;
  captured_.reset();
  held_at_begin_.clear();
  std::vector<rex::input::DeviceInfo> devices;
  inner_->EnumerateDevices(devices);
  for (const auto& d : devices) {
    rex::input::X_INPUT_STATE state{};
    if (inner_->GetDeviceState(d.id, &state) == X_ERROR_SUCCESS) {
      held_at_begin_[static_cast<uint64_t>(d.id)] = FromGuest(state.gamepad);
    }
  }
}

std::optional<PadButton> PadRemapDriver::TakeCapture() {
  std::lock_guard lock(mutex_);
  auto c = captured_;
  captured_.reset();
  return c;
}

void PadRemapDriver::EndCapture() {
  std::lock_guard lock(mutex_);
  capturing_ = false;
  captured_.reset();
  held_at_begin_.clear();
}

X_RESULT PadRemapDriver::GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) {
  rex::input::X_INPUT_STATE state{};
  const X_RESULT result = inner_->GetDeviceState(id, &state);
  if (result != X_ERROR_SUCCESS) {
    if (out_state) *out_state = state;
    return result;
  }
  PadState s = FromGuest(state.gamepad);
  constexpr int kStickMoved = 12000;  // a stick pushed (not resting off-centre) counts as using the controller
  if (s.buttons || Pressed(s, PadButton::kLeftTrigger) || Pressed(s, PadButton::kRightTrigger) ||
      std::abs(s.thumb_lx) > kStickMoved || std::abs(s.thumb_ly) > kStickMoved ||
      std::abs(s.thumb_rx) > kStickMoved || std::abs(s.thumb_ry) > kStickMoved) {
    kuf::prompts::NoteDevice(PromptDevice::kController);
  }
  {
    std::lock_guard lock(mutex_);
    if (capturing_) {
      PadState& before = held_at_begin_[static_cast<uint64_t>(id)];
      if (const auto b = NewlyPressed(before, s); b && !captured_) captured_ = b;
      // A button released since the capture began can be pressed again to count.
      before.buttons &= s.buttons;
      if (!Pressed(s, PadButton::kLeftTrigger)) before.left_trigger = 0;
      if (!Pressed(s, PadButton::kRightTrigger)) before.right_trigger = 0;
      s = PadState{};
    } else if (!remap_.identity() && CurrentGameContext() == GameContext::kGameplay) {
      const PadState in = s;
      s = remap_.Apply(s);
      static uint16_t last_in = 0;  // the trace: each change of what the controller presses
      if (REXCVAR_GET(kuf_trace_pad_remap) && in.buttons != last_in) {
        last_in = in.buttons;
        REXLOG_INFO("[kuf-input] gameplay pad: buttons {:04X} -> {:04X}, triggers {}/{} -> {}/{}", in.buttons,
                    s.buttons, in.left_trigger, in.right_trigger, s.left_trigger, s.right_trigger);
      }
    }
  }
  if (out_state) {
    out_state->packet_number = state.packet_number;
    ToGuest(s, &out_state->gamepad);
  }
  return result;
}

void ApplyBindingSettings() {
  const auto settings = kuf::pc::Current().bindings;
  if (auto* pads = PadRemapDriver::Instance()) pads->SetRemap(RemapFromSettings(settings));
  if (auto* keyboard = KufInputDriver::Instance()) keyboard->SetGameplayKeys(settings.keys);
}

}  // namespace kuf::input
