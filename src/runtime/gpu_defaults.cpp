// GPU defaults of the port (M8, docs/PERFORMANCE.md section 7).
//
// execute_unclipped_draw_vs_on_cpu (ReXGlue default false; Xenia's true): for draws with clipping disabled and the
// Direct3D 9 "no scissor" rectangle (8192x8192), the render target cache cannot tell how far down the draw reaches,
// and takes the render target to the end of the EDRAM. This game's full-screen passes and its 4x MSAA depth passes
// (K34) are such draws: every frame they claimed EDRAM owned by the other render targets, which the host render target
// path then copied into them and back (ownership transfers). With the option the vertex shader of those draws runs on
// the CPU to find their real extent. Measured at 3840x2160 in Shadow Depths: render target transfers 3.7 -> 1.4 ms of
// GPU time per frame. The game's images are unchanged (tests/display render comparisons, render.k34_*).
//
// vulkan_texture_load_direct_to_image (SDK patch 0013, default false; M9): the game's render-to-texture results (the
// HDR scene, depth read as a texture, RGBA8 copies), reloaded every frame at the render scale, are written straight
// into their images instead of a staging buffer copied into them. Measured at 3840x2160 (n = 5 interleaved):
// GPU time per frame 10.58 -> 8.58 ms, 87.3 -> 101.5 fps; the gameplay frames are bit-identical
// (render.direct_load_check).
#include "runtime/gpu_defaults.h"

#include <cstdlib>
#include <string>

#include <rex/logging.h>

namespace kuf::gpu {

namespace {

// The GPU plugin registers its options when it loads, after OnPreSetup; it reads REX_<NAME> environment variables
// at registration, below the command line (as display::Configure passes the render scale).
void SetDefault(const char* env_name, const char* value) {
  if (std::getenv(env_name)) {
    REXLOG_INFO("[kuf-gpu] {} from the environment: {}", env_name, std::getenv(env_name));
    return;
  }
  setenv(env_name, value, 1);
}

}  // namespace

void ConfigureDefaults() {
  SetDefault("REX_EXECUTE_UNCLIPPED_DRAW_VS_ON_CPU", "true");
  SetDefault("REX_VULKAN_TEXTURE_LOAD_DIRECT_TO_IMAGE", "true");
}

}  // namespace kuf::gpu
