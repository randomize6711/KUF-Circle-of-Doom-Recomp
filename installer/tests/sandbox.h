// A disposable home for the setup's tests: HOME, XDG_DATA_HOME and XDG_CONFIG_HOME point into a new temporary folder
// (removed afterwards), so nothing reaches the real desktop, menu, Steam or saves.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "core/util.h"

namespace kuf::setup::test {

struct Sandbox {
  std::filesystem::path dir;
  std::vector<std::pair<std::string, std::string>> saved;
  Sandbox() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "kuf-setup-test-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    dir = mkdtemp(buf.data());
    for (const char* k : {"HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME"}) {
      const char* v = std::getenv(k);
      saved.push_back({k, v ? v : ""});
    }
    std::filesystem::create_directories(dir / "home");
    setenv("HOME", (dir / "home").c_str(), 1);
    setenv("XDG_DATA_HOME", (dir / "home/.local/share").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (dir / "home/.config").c_str(), 1);
  }
  ~Sandbox() {
    for (const auto& [k, v] : saved) {
      if (v.empty()) {
        unsetenv(k.c_str());
      } else {
        setenv(k.c_str(), v.c_str(), 1);
      }
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::filesystem::path Home() const { return dir / "home"; }
  // A minimal XEX2: no code, the fields the setup reads. encryption 0 none, 1 normal; compression 0 none, 1 basic
  // (one block: the image and 16 zero bytes), 2 LZX.
  static std::string Xex(const std::string& image, int encryption = 0, int compression = 0) {
    const auto be32 = [](uint32_t v) {
      return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
    };
    const auto be16 = [](uint32_t v) { return std::string{char(v >> 8), char(v)}; };
    const bool basic = compression == 1;
    const uint32_t security = 32, format = 40, format_size = basic ? 16 : 8, payload = format + format_size;
    const uint32_t image_size = uint32_t(image.size()) + (basic ? 16 : 0);
    std::string x = "XEX2" + be32(0) + be32(payload) + be32(0) + be32(security) + be32(1);
    x += be32(0x3FF) + be32(format);
    x += be32(0) + be32(image_size);
    x += be32(format_size) + be16(uint32_t(encryption)) + be16(uint32_t(compression));
    if (basic) x += be32(uint32_t(image.size())) + be32(16);
    return x + image;
  }
  static void Write(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
  }
};

}  // namespace kuf::setup::test
