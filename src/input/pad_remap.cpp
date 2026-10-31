#include "input/pad_remap.h"

#include <algorithm>

namespace kuf::input {
namespace {

struct ButtonInfo {
  const char* name;
  const char* icon;
  uint16_t bit;  // 0 for the triggers
};

constexpr ButtonInfo kButtons[kPadButtonCount] = {
    {"a", "@(A)", buttons::kA},
    {"b", "@(B)", buttons::kB},
    {"x", "@(X)", buttons::kX},
    {"y", "@(Y)", buttons::kY},
    {"left_shoulder", "@(L)", buttons::kLeftShoulder},
    {"right_shoulder", "@(R)", buttons::kRightShoulder},
    {"left_trigger", "@(L_Trigger)", 0},
    {"right_trigger", "@(R_Trigger)", 0},
    {"back", "@(Back)", buttons::kBack},
    {"start", "@(Start)", buttons::kStart},
    {"left_thumb", "@(APad_L_Small)", buttons::kLeftThumb},
    {"right_thumb", "@(APad_R_Small)", buttons::kRightThumb},
};

const ButtonInfo& Info(PadButton b) { return kButtons[static_cast<size_t>(b)]; }

}  // namespace

const char* PadButtonName(PadButton button) {
  return static_cast<size_t>(button) < kPadButtonCount ? Info(button).name : "?";
}

const char* PadButtonIcon(PadButton button) {
  return static_cast<size_t>(button) < kPadButtonCount ? Info(button).icon : "";
}

std::optional<PadButton> ParsePadButton(std::string_view name) {
  for (size_t i = 0; i < kPadButtonCount; ++i) {
    if (name == kButtons[i].name) return static_cast<PadButton>(i);
  }
  return std::nullopt;
}

bool Pressed(const PadState& state, PadButton button) {
  switch (button) {
    case PadButton::kLeftTrigger: return state.left_trigger > kTriggerThreshold;
    case PadButton::kRightTrigger: return state.right_trigger > kTriggerThreshold;
    default: return (state.buttons & Info(button).bit) != 0;
  }
}

std::optional<PadButton> NewlyPressed(const PadState& before, const PadState& now) {
  for (size_t i = 0; i < kPadButtonCount; ++i) {
    const auto b = static_cast<PadButton>(i);
    if (Pressed(now, b) && !Pressed(before, b)) return b;
  }
  return std::nullopt;
}

bool PadRemap::identity() const { return *this == PadRemap{}; }

void PadRemap::Assign(PadButton action, PadButton physical) {
  const auto other = std::find(source_.begin(), source_.end(), physical);
  if (other != source_.end()) *other = source(action);
  source_[static_cast<size_t>(action)] = physical;
}

bool PadRemap::Set(const std::array<PadButton, kPadButtonCount>& sources) {
  std::array<bool, kPadButtonCount> used{};
  for (const PadButton b : sources) {
    const size_t i = static_cast<size_t>(b);
    if (i >= kPadButtonCount || used[i]) return false;
    used[i] = true;
  }
  source_ = sources;
  return true;
}

PadState PadRemap::Apply(const PadState& in) const {
  if (identity()) return in;
  PadState out = in;
  for (const auto& info : kButtons) out.buttons &= uint16_t(~info.bit);
  out.left_trigger = out.right_trigger = 0;
  for (size_t i = 0; i < kPadButtonCount; ++i) {
    const auto action = static_cast<PadButton>(i);
    const PadButton physical = source_[i];
    const bool physical_trigger = physical == PadButton::kLeftTrigger || physical == PadButton::kRightTrigger;
    const uint8_t travel = physical == PadButton::kLeftTrigger    ? in.left_trigger
                           : physical == PadButton::kRightTrigger ? in.right_trigger
                           : Pressed(in, physical)                ? uint8_t(255)
                                                                  : uint8_t(0);
    if (action == PadButton::kLeftTrigger) {
      out.left_trigger = travel;
    } else if (action == PadButton::kRightTrigger) {
      out.right_trigger = travel;
    } else if (physical_trigger ? travel > kTriggerThreshold : travel != 0) {
      out.buttons |= Info(action).bit;
    }
  }
  return out;
}

}  // namespace kuf::input
