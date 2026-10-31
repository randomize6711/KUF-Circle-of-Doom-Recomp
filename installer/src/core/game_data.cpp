#include "core/game_data.h"

#include <sstream>

namespace kuf::setup {

const GameFile* GameManifest::Find(std::string_view path) const {
  for (const GameFile& f : files) {
    if (f.path == path) return &f;
  }
  return nullptr;
}

GameManifest ParseGameManifest(std::string_view text, std::string name) {
  GameManifest m;
  m.name = std::move(name);
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) return {};
    GameFile f;
    f.sha256 = line.substr(0, a);
    try {
      f.size = std::stoull(line.substr(a + 1, b - a - 1));
    } catch (...) {
      return {};
    }
    f.path = line.substr(b + 1);
    if (f.sha256.size() != 64 || f.path.empty() || f.path[0] == '/' || f.path.find("..") != std::string::npos) {
      return {};
    }
    m.total += f.size;
    m.files.push_back(std::move(f));
  }
  if (!m.Find("default.xex")) return {};
  return m;
}

GameCheck CheckGameFolder(const fs::path& folder, const GameManifest& manifest) {
  GameCheck c;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return c;
  c.folder_ok = true;
  for (const GameFile& f : manifest.files) {
    const fs::path p = folder / f.path;
    if (!fs::is_regular_file(p, ec)) {
      c.missing.push_back(f.path);
      continue;
    }
    if (fs::file_size(p, ec) != f.size) {
      c.wrong_size.push_back(f.path);
      continue;
    }
    c.bytes += f.size;
  }
  const GameFile* xex = manifest.Find("default.xex");
  const fs::path exe = folder / "default.xex";
  c.executable_found = fs::is_regular_file(exe, ec);
  c.executable_known = c.executable_found && xex && Sha256File(exe) == xex->sha256;
  return c;
}

fs::path FindGameRoot(const fs::path& picked) {
  std::error_code ec;
  if (fs::is_regular_file(picked / "default.xex", ec)) return picked;
  fs::path found;
  int count = 0;
  for (const auto& e : fs::directory_iterator(picked, ec)) {
    if (e.is_directory(ec) && fs::is_regular_file(e.path() / "default.xex", ec)) {
      found = e.path();
      ++count;
    }
  }
  return count == 1 ? found : picked;
}

}  // namespace kuf::setup
