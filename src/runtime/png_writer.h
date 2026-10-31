// Minimal lossless PNG writer (8-bit RGB) without external dependencies: zlib "stored" deflate blocks.
// Used for offscreen frame captures (src/runtime/frame_capture.cpp). Files are larger than compressed PNG
// (about 2.7 MB for 1280x720), which is acceptable for local diagnostics.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace kuf::png {

// `rgbx` is width*height pixels of R,G,B,X bytes with `stride` bytes per row. Returns false on I/O error.
bool WriteRgb(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* rgbx,
              size_t stride);

}  // namespace kuf::png
