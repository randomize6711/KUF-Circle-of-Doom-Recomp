#include "input/input_factory.h"

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>

#include "input/kuf_input_driver.h"
#include "input/pad_remap_driver.h"

REXCVAR_DECLARE(std::string, input_backend);

namespace kuf::input {

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  // Same layout as the SDK's CreateDefaultInputSystem, with our keyboard/mouse driver in place of its MnK
  // driver (whose defaults are shooter controls and whose mouse only drives the right stick).
  auto input = std::make_unique<rex::input::InputSystem>(nullptr);
  if (!tool_mode) {
    if (REXCVAR_GET(input_backend) == "sdl") {
      // M7: the controllers with the player's gameplay button layout (pad_remap_driver.h).
      auto sdl = std::make_unique<PadRemapDriver>(std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0));
      if (sdl->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl));
      } else {
        REXLOG_WARN("[kuf-input] SDL controller driver unavailable; keyboard and mouse only");
      }
    }
    auto keyboard = std::make_unique<KufInputDriver>(nullptr, 0);
    if (keyboard->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(keyboard));
    }
  }
  input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, tool_mode ? 0 : 1));
  // Controller N -> guest user N; keyboard/mouse (synthetic) -> guest user 0, merged with controller 0.
  input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
  return input;
}

}  // namespace kuf::input
