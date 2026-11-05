// Small helpers of the setup (installer/README.md): paths of the user's session (XDG), quoting for shell scripts and
// desktop entries, free space, atomic writes and verified copies. No GTK here: the core is unit-tested (installer/tests).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kuf::setup {

namespace fs = std::filesystem;

// The environment's value, or "" when unset.
std::string Env(const char* name);
fs::path HomeDir();
fs::path XdgDataHome();    // $XDG_DATA_HOME or ~/.local/share
fs::path XdgConfigHome();  // $XDG_CONFIG_HOME or ~/.config
// The desktop folder: XDG_DESKTOP_DIR from $XDG_CONFIG_HOME/user-dirs.dirs ("$HOME/..." or an absolute path), else
// ~/Desktop. nullopt when the configured (or default) folder does not exist or is the home folder itself (the
// convention for "no desktop"): nothing is created on a desktop that is not there.
std::optional<fs::path> DesktopDir();

// `s` as one word for sh / bash ('...' with ' as '\'').
std::string ShellQuote(std::string_view s);
// One argument of a desktop entry's Exec key, already escaped as a desktop-entry string: quoted when it contains a
// space or a reserved character, with ", `, $ and \ escaped inside the quotes, % doubled, then \ doubled for the
// string level (Desktop Entry Specification, "The Exec key").
std::string DesktopExecArg(std::string_view arg);
// A desktop entry string value (\, new lines and tabs escaped).
std::string DesktopString(std::string_view s);

// Free bytes for a new file under `path` (its nearest existing ancestor); 0 if unknown.
uint64_t FreeBytes(const fs::path& path);
// True if a file can be created in `path`'s nearest existing ancestor (tried with a temporary file, removed).
bool CanCreateIn(const fs::path& path);
// The nearest existing ancestor of `path` (or `path`).
fs::path ExistingAncestor(const fs::path& path);

// Writes `content` to `path` through a temporary file in the same folder, flushed and renamed over it; `mode` the
// permission bits. False (and nothing changed) on failure.
bool AtomicWrite(const fs::path& path, std::string_view content, fs::perms mode = fs::perms(0644));
bool ReadFile(const fs::path& path, std::string* out);

// SHA-256 of a file as lower-case hex ("" on a read error).
std::string Sha256File(const fs::path& path);
// Copies `from` to `to` (through `to`.part, renamed when complete), hashing what is read; `progress(bytes)` after each
// block; `cancelled()` stops it. Returns the SHA-256 of the data copied ("" on failure or when cancelled: then
// nothing is left at `to`).
std::string CopyHashed(const fs::path& from, const fs::path& to, const std::function<void(uint64_t)>& progress,
                       const std::function<bool()>& cancelled);

// The game package's system requirements (its REQUIRES file, written by package_linux.sh from the binaries;
// payload/requires.txt in the AppImage): "glibc 2.31", "glibcxx 3.4.33" and, when the package carries its own
// libstdc++ (bin/compat, used by the launcher when the system's is older), "glibcxx-bundled 3.4.33". The check
// compares them with this system's C library and C++ library. `problems` lists what is too old (empty: compatible).
struct Requirements {
  std::string glibc;            // "2.31"
  std::string glibcxx;          // "3.4.33"
  std::string glibcxx_bundled;  // "3.4.33", or "" (the system's libstdc++ must do)
};
Requirements ParseRequirements(std::string_view text);
// -1, 0, 1 for dotted numeric versions ("2.39" < "2.44").
int CompareVersions(std::string_view a, std::string_view b);
std::string HostGlibc();
// The newest GLIBCXX_3.4.x version this system's libstdc++ defines: the loaded one, else the one in the usual
// library folders ("" if none).
std::string HostGlibcxx();
std::vector<std::string> CompatibilityProblems(const Requirements& need, const std::string& glibc,
                                               const std::string& glibcxx);

// "4.4 GB", "512 MB"
std::string HumanBytes(uint64_t bytes);

}  // namespace kuf::setup
