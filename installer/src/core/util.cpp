#include "core/util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <gnu/libc-version.h>
#include <link.h>

#include <glib.h>

namespace kuf::setup {

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

fs::path HomeDir() {
  const std::string home = Env("HOME");
  return home.empty() ? fs::path(g_get_home_dir()) : fs::path(home);
}

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
  struct statvfs st {};
  if (statvfs(ExistingAncestor(path).c_str(), &st) != 0) return 0;
  return uint64_t(st.f_bavail) * st.f_frsize;
}

bool CanCreateIn(const fs::path& path) {
  const fs::path dir = ExistingAncestor(path);
  std::string tmpl = (dir / ".kuf-setup-write-test-XXXXXX").string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const int fd = mkstemp(buf.data());
  if (fd < 0) return false;
  close(fd);
  unlink(buf.data());
  return true;
}

bool AtomicWrite(const fs::path& path, std::string_view content, fs::perms mode) {
  const fs::path tmp = path.string() + ".kuf-setup-tmp";
  {
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < content.size()) {
      const ssize_t n = write(fd, content.data() + done, content.size() - done);
      if (n <= 0) {
        close(fd);
        unlink(tmp.c_str());
        return false;
      }
      done += size_t(n);
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
      unlink(tmp.c_str());
      return false;
    }
  }
  std::error_code ec;
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
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  GChecksum* sum = g_checksum_new(G_CHECKSUM_SHA256);
  std::vector<unsigned char> buf(1 << 20);
  bool ok = true;
  for (;;) {
    const ssize_t n = read(fd, buf.data(), buf.size());
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    g_checksum_update(sum, buf.data(), gssize(n));
  }
  close(fd);
  std::string hex = ok ? g_checksum_get_string(sum) : "";
  g_checksum_free(sum);
  return hex;
}

std::string CopyHashed(const fs::path& from, const fs::path& to, const std::function<void(uint64_t)>& progress,
                       const std::function<bool()>& cancelled) {
  const fs::path part = to.string() + ".part";
  const int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return {};
  const int out = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    close(in);
    return {};
  }
  GChecksum* sum = g_checksum_new(G_CHECKSUM_SHA256);
  std::vector<unsigned char> buf(4 << 20);
  bool ok = true;
  for (;;) {
    if (cancelled && cancelled()) {
      ok = false;
      break;
    }
    const ssize_t n = read(in, buf.data(), buf.size());
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    g_checksum_update(sum, buf.data(), gssize(n));
    ssize_t done = 0;
    while (done < n) {
      const ssize_t w = write(out, buf.data() + done, size_t(n - done));
      if (w <= 0) {
        ok = false;
        break;
      }
      done += w;
    }
    if (!ok) break;
    if (progress) progress(uint64_t(n));
  }
  close(in);
  if (ok && fsync(out) != 0) ok = false;
  if (close(out) != 0) ok = false;
  std::string hex = ok ? g_checksum_get_string(sum) : "";
  g_checksum_free(sum);
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

std::string HostGlibc() { return gnu_get_libc_version(); }

std::string HostGlibcxx() {
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
}

std::vector<std::string> CompatibilityProblems(const Requirements& need, const std::string& glibc,
                                               const std::string& glibcxx) {
  std::vector<std::string> out;
  if (!need.glibc.empty() && CompareVersions(glibc, need.glibc) < 0) {
    out.push_back("the C library (glibc) is " + glibc + "; the game needs " + need.glibc + " or newer");
  }
  if (!need.glibcxx.empty() && (glibcxx.empty() || CompareVersions(glibcxx, need.glibcxx) < 0)) {
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
