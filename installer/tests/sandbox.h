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
  static void Write(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
  }
};

}  // namespace kuf::setup::test
