#include "core/util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <random>

#if defined(__linux__)
#include <gnu/libc-version.h>
#include <link.h>
#endif

#include "core/platform.h"
#include "core/sha256.h"

namespace kuf::setup {

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

fs::path HomeDir() { return PlatformHomeDir(); }

fs::path XdgDataHome() {
  const std::string v = Env("XDG_DATA_HOME");
  return !v.empty() && fs::path(v).is_absolute() ? fs::path(v) : HomeDir() / ".local/share";
}

fs::path XdgConfigHome() {
  const std::string v = Env("XDG_CONFIG_HOME");
  return !v.empty() && fs::path(v).is_absolute() ? fs::path(v) : HomeDir() / ".config";
}

std::optional<fs::path> DesktopDir() {
  fs::path dir = HomeDir() / "Desktop";
  std::string text;
  if (ReadFile(XdgConfigHome() / "user-dirs.dirs", &text)) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
      const std::string key = "XDG_DESKTOP_DIR=";
      if (line.rfind(key, 0) != 0) continue;
      std::string value = line.substr(key.size());
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
      if (value.rfind("$HOME", 0) == 0) {
        dir = HomeDir() / fs::path(value.substr(5)).relative_path();
      } else if (fs::path(value).is_absolute()) {
        dir = value;
      }
    }
  }
  std::error_code ec;
  // A desktop that is the home folder itself means no desktop (the XDG user-dirs convention): nothing is put into
  // the home folder.
  if (!fs::is_directory(dir, ec) || fs::equivalent(dir, HomeDir(), ec)) return std::nullopt;
  return dir;
}

std::string ShellQuote(std::string_view s) {
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out + "'";
}

std::string DesktopExecArg(std::string_view arg) {
  bool quote = arg.empty();
  for (const char c : arg) {
    if (std::strchr(" \t\n\"'\\><~|&;$*?#()`", c)) quote = true;
  }
  std::string word;
  if (quote) word += '"';
  for (const char c : arg) {
    if (quote && (c == '"' || c == '`' || c == '$' || c == '\\')) word += '\\';
    if (c == '%') word += '%';  // a field code otherwise
    word += c;
  }
  if (quote) word += '"';
  // The string level: every \ doubled (the Exec value is unescaped as a string first).
  std::string out;
  for (const char c : word) {
    if (c == '\\') out += '\\';
    out += c;
  }
  return out;
}

std::string DesktopString(std::string_view s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += c;
    }
  }
  return out;
}

fs::path ExistingAncestor(const fs::path& path) {
  std::error_code ec;
  fs::path p = fs::absolute(path, ec);
  while (!p.empty() && !fs::exists(p, ec)) {
    if (p == p.parent_path()) break;
    p = p.parent_path();
  }
  return p;
}

uint64_t FreeBytes(const fs::path& path) {
  std::error_code ec;
  const fs::space_info s = fs::space(ExistingAncestor(path), ec);
  return ec ? 0 : uint64_t(s.available);
}

bool CanCreateIn(const fs::path& path) {
  const fs::path dir = ExistingAncestor(path);
  std::error_code ec;
  for (int attempt = 0; attempt < 8; ++attempt) {
    const fs::path test = dir / (".kuf-setup-write-test-" + std::to_string(std::random_device{}()));
    if (fs::exists(test, ec)) continue;
    OutFile f;
    if (!f.Create(test, true)) return false;
    f.Close();
    fs::remove(test, ec);
    return true;
  }
  return false;
}

bool AtomicWrite(const fs::path& path, std::string_view content, fs::perms mode) {
  fs::path tmp = path;
  tmp += ".kuf-setup-tmp";
  std::error_code ec;
  {
    OutFile f;
    if (!f.Create(tmp, true)) return false;
    if (!f.Write(content.data(), content.size()) || !f.Sync() || !f.Close()) {
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::permissions(tmp, mode, ec);
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool ReadFile(const fs::path& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream s;
  s << in.rdbuf();
  *out = s.str();
  return true;
}

std::string Sha256File(const fs::path& path) {
  InFile in(path);
  if (!in.ok()) return {};
  Sha256 sum;
  std::vector<unsigned char> buf(1 << 20);
  for (;;) {
    const int64_t n = in.Read(buf.data(), buf.size());
    if (n < 0) return {};
    if (n == 0) break;
    sum.Update(buf.data(), size_t(n));
  }
  return sum.HexDigest();
}

std::string CopyHashed(const fs::path& from, const fs::path& to, const std::function<void(uint64_t)>& progress,
                       const std::function<bool()>& cancelled) {
  return CopyRangeHashed(from, 0, kToEnd, to, progress, cancelled);
}

std::string CopyRangeHashed(const fs::path& from, uint64_t offset, uint64_t length, const fs::path& to,
                            const std::function<void(uint64_t)>& progress, const std::function<bool()>& cancelled) {
  fs::path part = to;
  part += ".part";
  InFile in(from);
  if (!in.ok()) return {};
  if (offset != 0 && !in.Seek(offset)) return {};
  OutFile out;
  if (!out.Create(part)) return {};
  Sha256 sum;
  std::vector<unsigned char> buf(4 << 20);
  bool ok = true;
  for (;;) {
    if (cancelled && cancelled()) {
      ok = false;
      break;
    }
    if (length == 0) break;
    const int64_t n = in.Read(buf.data(), size_t(std::min<uint64_t>(buf.size(), length)));
    if (n < 0 || (n == 0 && length != kToEnd)) {  // a range that ends past the file is a failure
      ok = false;
      break;
    }
    if (n == 0) break;
    if (length != kToEnd) length -= uint64_t(n);
    sum.Update(buf.data(), size_t(n));
    if (!out.Write(buf.data(), size_t(n))) {
      ok = false;
      break;
    }
    if (progress) progress(uint64_t(n));
  }
  if (ok && !out.Sync()) ok = false;
  if (!out.Close()) ok = false;
  std::string hex = ok ? sum.HexDigest() : "";
  std::error_code ec;
  if (ok) {
    fs::rename(part, to, ec);
    if (ec) hex.clear();
  }
  if (hex.empty()) fs::remove(part, ec);
  return hex;
}

Requirements ParseRequirements(std::string_view text) {
  Requirements r;
  std::istringstream in{std::string(text)};
  std::string key, value;
  while (in >> key >> value) {
    if (key == "glibc") r.glibc = value;
    if (key == "glibcxx") r.glibcxx = value;
    if (key == "glibcxx-bundled") r.glibcxx_bundled = value;
  }
  return r;
}

int CompareVersions(std::string_view a, std::string_view b) {
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    unsigned long x = 0, y = 0;
    while (i < a.size() && a[i] != '.') x = x * 10 + unsigned(a[i++] - '0');
    while (j < b.size() && b[j] != '.') y = y * 10 + unsigned(b[j++] - '0');
    if (x != y) return x < y ? -1 : 1;
    ++i;
    ++j;
  }
  return 0;
}

#if defined(__linux__)
std::string HostGlibc() { return gnu_get_libc_version(); }
#else
std::string HostGlibc() { return ""; }  // not a glibc system (Windows): no glibc requirement applies
#endif

std::string HostGlibcxx() {
#if !defined(__linux__)
  return "";
#else
  // The loaded libstdc++.so.6's file: its version names (GLIBCXX_3.4.N) are in its .gnu.version_d strings.
  std::string path;
  dl_iterate_phdr(
      [](struct dl_phdr_info* info, size_t, void* data) {
        if (info->dlpi_name && std::strstr(info->dlpi_name, "libstdc++.so.6")) {
          *static_cast<std::string*>(data) = info->dlpi_name;
          return 1;
        }
        return 0;
      },
      &path);
  if (path.empty()) {  // linked statically (the AppImage's setup): the system's own
    for (const char* c : {"/usr/lib/x86_64-linux-gnu/libstdc++.so.6", "/lib/x86_64-linux-gnu/libstdc++.so.6",
                          "/usr/lib64/libstdc++.so.6", "/usr/lib/libstdc++.so.6"}) {
      std::error_code ec;
      if (fs::exists(c, ec)) {
        path = c;
        break;
      }
    }
  }
  std::string bytes;
  if (path.empty() || !ReadFile(path, &bytes)) return {};
  std::string best;
  const std::string tag = "GLIBCXX_3.4.";
  for (size_t at = bytes.find(tag); at != std::string::npos; at = bytes.find(tag, at + 1)) {
    size_t end = at + tag.size();
    while (end < bytes.size() && bytes[end] >= '0' && bytes[end] <= '9') ++end;
    if (end == at + tag.size() || (end < bytes.size() && bytes[end] != '\0')) continue;
    const std::string v = bytes.substr(at + 8, end - at - 8);
    if (best.empty() || CompareVersions(v, best) > 0) best = v;
  }
  return best;
#endif
}

std::vector<std::string> CompatibilityProblems(const Requirements& need, const std::string& glibc,
                                               const std::string& glibcxx) {
  std::vector<std::string> out;
  if (!need.glibc.empty() && CompareVersions(glibc, need.glibc) < 0) {
    out.push_back("the C library (glibc) is " + glibc + "; the game needs " + need.glibc + " or newer");
  }
  const bool bundled = !need.glibcxx_bundled.empty() &&
                       (need.glibcxx.empty() || CompareVersions(need.glibcxx_bundled, need.glibcxx) >= 0);
  if (!bundled && !need.glibcxx.empty() && (glibcxx.empty() || CompareVersions(glibcxx, need.glibcxx) < 0)) {
    out.push_back("the C++ library (libstdc++) provides " + (glibcxx.empty() ? std::string("an unknown version")
                                                                              : "GLIBCXX " + glibcxx) +
                  "; the game needs GLIBCXX " + need.glibcxx + " or newer");
  }
  return out;
}

std::string HumanBytes(uint64_t bytes) {
  char buf[32];
  if (bytes >= (uint64_t(1) << 30)) {
    std::snprintf(buf, sizeof(buf), "%.1f GB", double(bytes) / double(uint64_t(1) << 30));
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f MB", double(bytes) / double(1 << 20));
  }
  return buf;
}

}  // namespace kuf::setup
