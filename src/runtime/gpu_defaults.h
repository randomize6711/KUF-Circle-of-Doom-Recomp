// GPU defaults of the port (M8): SDK GPU options whose ReXGlue default does not suit this game
// (src/runtime/gpu_defaults.cpp; docs/PERFORMANCE.md section 7).
#pragma once

namespace kuf::gpu {

// Before the graphics system starts (OnPreSetup): passes the port's defaults to the GPU plugin. An explicit command
// line option or REX_<NAME> environment variable still wins.
void ConfigureDefaults();

}  // namespace kuf::gpu
