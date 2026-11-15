#include "core/game_data.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "core/platform.h"
#include "core/sha256.h"
#include "core/xex_decrypt.h"

namespace kuf::setup {

namespace {
constexpr uint32_t kXex2Magic = 0x58455832;  // "XEX2"
constexpr uint32_t kFileFormatHeader = 0x3FF;
constexpr uint32_t kExecutionInfoHeader = 0x40006;  // 24 bytes: media, version, base version, title, ..., discs
constexpr uint32_t kModulePatchFlags = 0x10 | 0x20 | 0x40;  // module patch, full patch, delta patch

uint32_t Be32(std::string_view d, size_t o) {
  const auto* p = reinterpret_cast<const unsigned char*>(d.data()) + o;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t Be16(std::string_view d, size_t o) {
  const auto* p = reinterpret_cast<const unsigned char*>(d.data()) + o;
  return uint32_t(p[0]) << 8 | p[1];
}
std::string Sha256Of(std::string_view d) { return Sha256Hex(d); }
std::string Hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08X", v);
  return b;
}
bool ParseHex32(const std::string& s, uint32_t* out) {
  if (s.empty() || s.size() > 8) return false;
  uint32_t v = 0;
  for (char c : s) {
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    v = v << 4 | uint32_t(d);
  }
  *out = v;
  return true;
}
std::string Fold(std::string_view s) {
  std::string f(s);
  for (char& c : f) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  return f;
}

// A picked default.xex (any case, or any file starting with the XEX2 magic) stands for its folder.
bool IsPickedXex(const fs::path& picked) {
  std::error_code ec;
  if (!fs::is_regular_file(picked, ec)) return false;
  if (Fold(picked.filename().string()) == "default.xex") return true;
  std::ifstream in(picked, std::ios::binary);
  char magic[4] = {};
  return in.read(magic, 4) && std::string_view(magic, 4) == "XEX2";
}
}  // namespace

// The XEX2 layout: magic, flags, payload offset, reserved, security offset, optional header count, then (key, value)
// pairs. File format info: size, encryption (0 none), compression (0 none, 1 basic: (data, zeros) block sizes).
// Execution info: media ID, version, base version, title ID, platform, executable type, disc number, disc count.
XexInfo InspectXexData(std::string_view d) {
  XexInfo info;
  if (d.size() > kMaxXexBytes) return info;
  info.file_sha256 = Sha256Of(d);
  info.status = XexStatus::kNotXex;
  if (d.size() < 24 || Be32(d, 0) != kXex2Magic) return info;
  info.module_flags = Be32(d, 4);
  const uint64_t payload = Be32(d, 8), security = Be32(d, 16), count = Be32(d, 20);
  if (24 + count * 8 > d.size() || payload > d.size() || security + 8 > d.size()) return info;
  uint64_t format = 0, execution = 0;
  for (uint64_t i = 0; i < count; ++i) {
    const uint32_t key = Be32(d, 24 + i * 8);
    if (key == kFileFormatHeader) format = Be32(d, 28 + i * 8);
    if (key == kExecutionInfoHeader) execution = Be32(d, 28 + i * 8);
  }
  if (execution != 0 && execution + 24 <= d.size()) {
    info.has_execution_info = true;
    info.media_id = Be32(d, execution);
    info.version = Be32(d, execution + 4);
    info.base_version = Be32(d, execution + 8);
    info.title_id = Be32(d, execution + 12);
    info.disc_number = uint8_t(d[execution + 18]);
    info.disc_count = uint8_t(d[execution + 19]);
  }
  if (format == 0 || format + 8 > d.size()) return info;
  const uint64_t format_size = Be32(d, format);
  if (format_size < 8 || format + format_size > d.size()) return info;
  info.encryption = uint16_t(Be16(d, format + 4));
  info.compression = uint16_t(Be16(d, format + 6));
  if (info.encryption != 0) {
    info.status = XexStatus::kEncrypted;
    return info;
  }
  if (info.compression > 1) {
    info.status = XexStatus::kUnsupportedCompression;
    return info;
  }
  const uint64_t image_size = Be32(d, security + 4);
  if (image_size == 0 || image_size > kMaxXexBytes) return info;
  std::string image;
  image.reserve(image_size);
  if (info.compression == 0) {
    image.assign(d.substr(payload));
  } else {
    uint64_t from = payload;
    for (uint64_t o = format + 8; o + 8 <= format + format_size; o += 8) {
      const uint64_t data = Be32(d, o), zeros = Be32(d, o + 4);
      if (from + data > d.size() || image.size() + data + zeros > kMaxXexBytes) return info;
      image.append(d.substr(from, data));
      image.append(zeros, '\0');
      from += data;
    }
  }
  image.resize(image_size, '\0');
  info.image_sha256 = Sha256Of(image);
  info.status = XexStatus::kOk;
  return info;
}

XexInfo InspectXex(const fs::path& path) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > kMaxXexBytes) return {};
  std::string d;
  if (!ReadFile(path, &d)) return {};
  return InspectXexData(d);
}

std::string XexVersionString(uint32_t v) {
  return std::to_string(v >> 28) + "." + std::to_string((v >> 24) & 0xF) + "." + std::to_string((v >> 8) & 0xFFFF) +
         "." + std::to_string(v & 0xFF);
}

Executable JudgeExecutable(const XexInfo& info, const GameManifest& m) {
  if (info.status == XexStatus::kUnreadable) return Executable::kUnreadable;
  if (info.status == XexStatus::kNotXex) return Executable::kNotXex;
  if (info.has_execution_info && m.title_id != 0 && info.title_id != m.title_id) return Executable::kOtherGame;
  if (info.module_flags & kModulePatchFlags) return Executable::kTitleUpdate;
  if (info.has_execution_info && m.title_id != 0 && (info.version != m.version || info.media_id != m.media_id)) {
    return Executable::kOtherVersion;
  }
  if (info.status == XexStatus::kEncrypted) {
    return !m.retail_xex_sha256.empty() && info.file_sha256 == m.retail_xex_sha256 ? Executable::kRetailEncrypted
                                                                                  : Executable::kEncrypted;
  }
  if (info.status == XexStatus::kUnsupportedCompression) return Executable::kUnsupportedCompression;
  const GameFile* xex = m.Find("default.xex");
  return xex && info.image_sha256 == xex->sha256 ? Executable::kSupported : Executable::kModified;
}

std::string ExecutableMessage(Executable e, const XexInfo& info, const GameManifest& m) {
  const std::string supported = "version " + XexVersionString(m.version) + ", media ID " + Hex8(m.media_id);
  switch (e) {
    case Executable::kMissing: return "default.xex was not found.";
    case Executable::kUnreadable: return "default.xex cannot be read.";
    case Executable::kNotXex: return "default.xex is not an Xbox 360 executable.";
    case Executable::kOtherGame:
      return "This default.xex belongs to another game (title ID " + Hex8(info.title_id) +
             "). This setup is for Kingdom Under Fire: Circle of Doom (title ID " + Hex8(m.title_id) + ").";
    case Executable::kTitleUpdate:
      return "This is a title update (a patch), not the game's executable. Title updates are not supported: use the "
             "original disc's default.xex.";
    case Executable::kOtherVersion:
      return "This is another version or release of the game (version " + XexVersionString(info.version) +
             ", media ID " + Hex8(info.media_id) + "). Only the original Xbox 360 release is supported (" + supported +
             ").";
    case Executable::kRetailEncrypted:
      if (RetailDecryptionAvailable()) {
        return "Your retail copy of the supported version was recognised, but its encrypted default.xex could not be "
               "decrypted (the file may be damaged). You can add an unencrypted default.xex of the same version "
               "separately.";
      }
      return "Your retail copy of the supported version was recognised, but its default.xex is encrypted. This build "
             "cannot decrypt it. You can add an unencrypted default.xex of the same version separately.";
    case Executable::kEncrypted:
      return "This default.xex is encrypted, and it is not the known retail file, so it cannot be decrypted. You "
             "can add an unencrypted default.xex of the supported version separately.";
    case Executable::kUnsupportedCompression:
      return "This default.xex uses a compression the setup cannot read (LZX). Only an uncompressed or "
             "basic-compressed executable can be used.";
    case Executable::kModified:
      return "This default.xex is the right game and version, but its content differs from the supported release (it "
             "is modified or damaged).";
    case Executable::kDecryptedMismatch:
      return "This default.xex was decrypted, but its content differs from the supported release (it is modified or "
             "damaged).";
    case Executable::kSupported: return "";
  }
  return "";
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
    if (!line.empty() && line.back() == '\r') line.pop_back();  // a list with Windows line ends
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '@') {
      std::istringstream words(line);
      std::string key, a, b, c;
      words >> key >> a >> b >> c;
      if (key == "@title") {
        if (!ParseHex32(a, &m.title_id) || !ParseHex32(b, &m.media_id) || !ParseHex32(c, &m.version)) return {};
      } else if (key == "@retail-xex") {
        if (a.size() != 64) return {};
        m.retail_xex_sha256 = a;
        try {
          m.retail_xex_size = std::stoull(b);
        } catch (...) {
          return {};
        }
      }  // other '@' keys: ignored (a newer list)
      continue;
    }
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

// ---- sources ----------------------------------------------------------------------------------------------------

GameSource GameSource::Folder(const fs::path& folder) {
  GameSource s;
  s.kind_ = Kind::kFolder;
  s.path_ = folder;
  std::error_code ec;
  s.ok_ = fs::is_directory(folder, ec);
  return s;
}

GameSource GameSource::Iso(const fs::path& image) {
  GameSource s;
  s.kind_ = Kind::kIso;
  s.path_ = image;
  IsoImage iso = ReadXiso(image);
  s.ok_ = iso.status == IsoStatus::kOk;
  if (iso.status == IsoStatus::kUnreadable) s.error_ = "The disc image cannot be read.";
  if (iso.status == IsoStatus::kNotXiso) s.error_ = "This file is not an Xbox 360 disc image (no XDVDFS volume found).";
  if (iso.status == IsoStatus::kMalformed) s.error_ = "The disc image is damaged: " + iso.error + ".";
  s.iso_ = std::move(iso.files);
  return s;
}

GameSource GameSource::Open(const fs::path& picked) {
  std::error_code ec;
  if (fs::is_regular_file(picked, ec) && !IsPickedXex(picked)) return Iso(picked);
  return Folder(FindGameRoot(picked));
}

GameSource::Entry GameSource::Find(std::string_view rel) const {
  Entry e;
  if (!ok_) return e;
  std::error_code ec;
  if (kind_ == Kind::kIso) {
    const IsoFile* folded = nullptr;
    const std::string want = Fold(rel);
    for (const IsoFile& f : iso_) {
      if (f.path == rel) {
        folded = &f;
        break;
      }
      if (!folded && Fold(f.path) == want) folded = &f;
    }
    if (!folded) return e;
    return {true, path_, folded->offset, folded->size, folded->path};
  }
  fs::path p = path_ / fs::path(std::string(rel));
  if (!fs::is_regular_file(p, ec)) {  // each component, ignoring case
    p = path_;
    std::string part;
    std::istringstream parts{std::string(rel)};
    while (std::getline(parts, part, '/')) {
      if (fs::exists(p / part, ec)) {
        p /= part;
        continue;
      }
      fs::path match;
      for (const auto& d : fs::directory_iterator(p, ec)) {
        if (Fold(d.path().filename().string()) == Fold(part)) {
          match = d.path();
          break;
        }
      }
      if (match.empty()) return e;
      p = match;
    }
    if (!fs::is_regular_file(p, ec)) return e;
  }
  e.found = true;
  e.file = p;
  e.size = fs::file_size(p, ec);
  e.path = fs::relative(p, path_, ec).generic_string();
  return e;
}

bool GameSource::HasRetailLayout() const {
  if (!ok_) return false;
  if (kind_ == Kind::kIso) {
    for (const IsoFile& f : iso_) {
      if (Fold(f.path).rfind("$systemupdate/", 0) == 0) return true;
    }
    return false;
  }
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(path_, ec)) {
    if (d.is_directory(ec) && Fold(d.path().filename().string()) == "$systemupdate") return true;
  }
  return false;
}

bool GameSource::Read(const Entry& e, std::string* out, uint64_t max) const {
  if (!e.found || e.size > max) return false;
  if (kind_ == Kind::kFolder) return ReadFile(e.file, out);
  InFile f(e.file);
  if (!f.ok()) return false;
  out->assign(e.size, '\0');
  return f.ReadAt(e.offset, out->data(), e.size);
}

std::string GameSource::Copy(const Entry& e, const fs::path& to, const std::function<void(uint64_t)>& progress,
                             const std::function<bool()>& cancelled) const {
  if (!e.found) return {};
  if (kind_ == Kind::kFolder) return CopyHashed(e.file, to, progress, cancelled);
  return CopyRangeHashed(e.file, e.offset, e.size, to, progress, cancelled);
}

// ---- checks -----------------------------------------------------------------------------------------------------

GameCheck CheckGame(const GameSource& source, const GameManifest& manifest, const fs::path& executable) {
  GameCheck c;
  c.kind = source.kind();
  c.source_error = source.error();
  if (!source.ok()) return c;
  c.folder_ok = true;
  c.retail_layout = source.HasRetailLayout();
  for (const GameFile& f : manifest.files) {
    const GameSource::Entry e = source.Find(f.path);
    if (f.path == "default.xex") {  // judged by its identity and image below
      if (e.found) c.bytes += e.size;
      continue;
    }
    if (!e.found) {
      c.missing.push_back(f.path);
      continue;
    }
    if (e.size != f.size) {
      c.wrong_size.push_back(f.path);
      continue;
    }
    c.bytes += f.size;
  }
  const GameSource::Entry exe = source.Find("default.xex");
  c.executable_found = exe.found;
  if (exe.found) {
    std::string data;
    c.dump_xex = source.Read(exe, &data, kMaxXexBytes) ? InspectXexData(data) : XexInfo{};
    c.dump_verdict = JudgeExecutable(c.dump_xex, manifest);
    // An encrypted executable of the supported title and version: decrypted in memory, accepted if its image is the
    // supported one (the installation decrypts it again from the source and checks the same).
    if (NeedsSeparateExecutable(c.dump_verdict) && RetailDecryptionAvailable()) {
      const XexDecrypt r = DecryptRetailXex(&data);
      const Executable plain = r == XexDecrypt::kOk ? JudgeExecutable(InspectXexData(data), manifest)
                                                    : Executable::kMissing;
      if (plain == Executable::kSupported) {
        c.dump_verdict = Executable::kSupported;
        c.decrypt = true;
      } else if (r == XexDecrypt::kOk) {
        c.dump_verdict = Executable::kDecryptedMismatch;
      }
    }
  }
  c.verdict = c.dump_verdict;
  if (!executable.empty()) {
    c.separate = true;
    std::error_code ec;
    c.separate_xex = fs::is_regular_file(executable, ec) ? InspectXex(executable) : XexInfo{};
    c.separate_verdict =
        fs::is_regular_file(executable, ec) ? JudgeExecutable(c.separate_xex, manifest) : Executable::kMissing;
    c.verdict = c.separate_verdict;
    c.executable_found = c.separate_verdict != Executable::kMissing;
  }
  c.executable_known = c.verdict == Executable::kSupported;
  c.executable_encrypted = c.dump_xex.status == XexStatus::kEncrypted && !c.executable_known;
  return c;
}

GameCheck CheckGameFolder(const fs::path& folder, const GameManifest& manifest) {
  return CheckGame(GameSource::Folder(folder), manifest);
}

std::string GameCheckProblem(const GameCheck& c, const GameManifest& m) {
  if (!c.folder_ok) {
    return c.kind == GameSource::Kind::kIso && !c.source_error.empty() ? c.source_error
                                                                       : "The game folder cannot be read.";
  }
  if (c.separate && c.separate_verdict != Executable::kSupported) {
    return c.separate_verdict == Executable::kMissing
               ? "The separate default.xex was not found."
               : "The separate default.xex: " + ExecutableMessage(c.separate_verdict, c.separate_xex, m);
  }
  if (!c.separate && c.dump_verdict != Executable::kSupported) {
    return c.dump_verdict == Executable::kMissing ? "default.xex was not found in the game files."
                                                  : ExecutableMessage(c.dump_verdict, c.dump_xex, m);
  }
  if (!c.missing.empty() || !c.wrong_size.empty()) {
    return "Game files are missing or have the wrong size (" + std::to_string(c.missing.size()) + " missing, " +
           std::to_string(c.wrong_size.size()) + " with the wrong size).";
  }
  return "";
}

fs::path FindGameRoot(const fs::path& picked) {
  std::error_code ec;
  if (IsPickedXex(picked)) return picked.has_parent_path() ? picked.parent_path() : fs::path(".");
  if (fs::is_regular_file(picked, ec)) return picked;
  if (fs::is_regular_file(picked / "default.xex", ec)) return picked;
  fs::path found;
  int count = 0;
  for (const auto& e : fs::directory_iterator(picked, ec)) {
    if (e.is_directory(ec) && GameSource::Folder(e.path()).Find("default.xex").found) {
      found = e.path();
      ++count;
    }
  }
  if (count == 1) return found;
  if (GameSource::Folder(picked).Find("default.xex").found) return picked;  // Default.xex
  return picked;
}

}  // namespace kuf::setup
