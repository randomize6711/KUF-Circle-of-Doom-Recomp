#include "core/game_data.h"

#include <glib.h>

#include <sstream>

namespace kuf::setup {

namespace {
constexpr uint32_t kXex2Magic = 0x58455832;  // "XEX2"
constexpr uint32_t kFileFormatHeader = 0x3FF;
constexpr uint64_t kMaxXexBytes = uint64_t(256) << 20;

uint32_t Be32(const std::string& d, size_t o) {
  const auto* p = reinterpret_cast<const unsigned char*>(d.data()) + o;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t Be16(const std::string& d, size_t o) {
  const auto* p = reinterpret_cast<const unsigned char*>(d.data()) + o;
  return uint32_t(p[0]) << 8 | p[1];
}
}  // namespace

// The XEX2 layout: magic, flags, payload offset, reserved, security offset, optional header count, then (key, value)
// pairs. File format info: size, encryption (0 none), compression (0 none, 1 basic: (data, zeros) block sizes).
XexInfo InspectXex(const fs::path& path) {
  XexInfo info;
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > kMaxXexBytes) return info;
  std::string d;
  if (!ReadFile(path, &d)) return info;
  info.status = XexStatus::kNotXex;
  if (d.size() < 24 || Be32(d, 0) != kXex2Magic) return info;
  const uint64_t payload = Be32(d, 8), security = Be32(d, 16), count = Be32(d, 20);
  if (24 + count * 8 > d.size() || payload > d.size() || security + 8 > d.size()) return info;
  uint64_t format = 0;
  for (uint64_t i = 0; i < count; ++i) {
    if (Be32(d, 24 + i * 8) == kFileFormatHeader) format = Be32(d, 28 + i * 8);
  }
  if (format == 0 || format + 8 > d.size()) return info;
  const uint64_t format_size = Be32(d, format);
  if (format_size < 8 || format + format_size > d.size()) return info;
  const uint32_t encryption = Be16(d, format + 4), compression = Be16(d, format + 6);
  if (encryption != 0) {
    info.status = XexStatus::kEncrypted;
    return info;
  }
  if (compression > 1) {
    info.status = XexStatus::kUnsupportedCompression;
    return info;
  }
  const uint64_t image_size = Be32(d, security + 4);
  if (image_size == 0 || image_size > kMaxXexBytes) return info;
  std::string image;
  image.reserve(image_size);
  if (compression == 0) {
    image.assign(d, payload, std::string::npos);
  } else {
    uint64_t from = payload;
    for (uint64_t o = format + 8; o + 8 <= format + format_size; o += 8) {
      const uint64_t data = Be32(d, o), zeros = Be32(d, o + 4);
      if (from + data > d.size() || image.size() + data + zeros > kMaxXexBytes) return info;
      image.append(d, from, data);
      image.append(zeros, '\0');
      from += data;
    }
  }
  image.resize(image_size, '\0');
  gchar* hex = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(image.data()),
                                           image.size());
  info.image_sha256 = hex;
  g_free(hex);
  info.status = XexStatus::kOk;
  return info;
}

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
    if (f.path == "default.xex") {  // compared by its image below
      c.bytes += fs::file_size(p, ec);
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
  if (c.executable_found) {
    const XexInfo info = InspectXex(exe);
    c.executable_encrypted = info.status == XexStatus::kEncrypted;
    c.executable_known = info.status == XexStatus::kOk && xex && info.image_sha256 == xex->sha256;
  }
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
