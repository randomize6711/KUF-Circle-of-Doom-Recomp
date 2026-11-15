// The setup core's few system calls, for Linux and Windows (M16, docs/WINDOWS.md): files written with a flush to the
// disk, the home folder, child processes (the local build) and the available memory. Everything else in the core is
// standard C++.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kuf::setup {

namespace fs = std::filesystem;

// A file opened for reading, with positioned reads (disc images) and sequential ones.
class InFile {
 public:
  InFile() = default;
  explicit InFile(const fs::path& path) { Open(path); }
  ~InFile();
  InFile(const InFile&) = delete;
  InFile& operator=(const InFile&) = delete;
  bool Open(const fs::path& path);
  bool ok() const;
  // Sequential: bytes read, 0 at the end, -1 on an error.
  int64_t Read(void* out, size_t size);
  // Exactly `size` bytes at `offset` (false on an error or a short read).
  bool ReadAt(uint64_t offset, void* out, size_t size);
  bool Seek(uint64_t offset);

 private:
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

// A new file (replacing one), written, flushed to the disk (Sync) and closed. Close() reports a failed close.
class OutFile {
 public:
  OutFile() = default;
  ~OutFile();
  OutFile(const OutFile&) = delete;
  OutFile& operator=(const OutFile&) = delete;
  bool Create(const fs::path& path, bool private_mode = false);  // private: 0600 on Linux
  bool Write(const void* data, size_t size);
  bool Sync();
  bool Close();

 private:
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

// Paths as UTF-8 text and back. On Windows fs::path::string() uses the ANSI code page and loses characters it does not
// have (a Unicode folder name); the core keeps text in UTF-8 and converts only here.
std::string PathUtf8(const fs::path& path);
fs::path PathFromUtf8(const std::string& text);

// The user's home folder (Linux $HOME, else the password database; Windows %USERPROFILE%).
fs::path PlatformHomeDir();

// The memory still available to new programs (bytes); a guess of 4 GiB when unknown.
uint64_t AvailableMemory();

// A started child process (the local build: the code generator, the compiler, the linker).
struct ChildProcess {
#if defined(_WIN32)
  void* process = nullptr;
  void* job = nullptr;  // a Job Object: the child and its children end with the setup
#else
  int pid = -1;
#endif
  bool running() const;
};

// Starts `argv` (argv[0] a full path) in `cwd`, at a lower priority, its output appended to `log`; `env` entries
// (NAME=value) replace or add variables. False when it could not start.
bool StartProcess(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log,
                  const std::vector<std::string>& env, ChildProcess* child);

// Whether it has ended; then `exit_code` is its exit status, and `out_of_memory` whether the system ended it for lack
// of memory (Linux: SIGKILL; never on Windows, which fails an allocation instead).
bool PollProcess(ChildProcess* child, int* exit_code, bool* out_of_memory);

// Ends it and everything it started, and waits for that.
void KillProcess(ChildProcess* child);

// Starts a program that outlives the caller (the game, from the launcher): its own session on Linux, a detached process
// on Windows; output to `log` (appended). False when it could not start.
bool StartDetached(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log,
                   const std::vector<std::string>& env);

// The user's configuration folder for this project (Linux $XDG_CONFIG_HOME/kuf-cod-recomp, Windows
// %APPDATA%\kuf-cod-recomp): only where a portable folder cannot be written (its chosen data folder is remembered).
fs::path UserConfigDir();

// Windows (M16 setup): the folders a setup uses, and shell links (.lnk). On Linux: empty paths and false (the Linux
// setup writes desktop entries itself, core/install).
enum class KnownFolder { kLocalAppData, kPrograms, kStartMenuPrograms, kDesktop };
fs::path GetKnownFolder(KnownFolder which);
bool CreateShortcut(const fs::path& link, const fs::path& target, const std::string& arguments,
                    const fs::path& working_dir, const std::string& description);

// Windows: one argument quoted for CreateProcess's command line (the rules of CommandLineToArgvW). For tests on every
// system.
std::string QuoteWindowsArgument(const std::string& arg);

// A program's arguments as UTF-8 (Windows: from the wide command line, main() would get the ANSI code page).
std::vector<std::string> Utf8Arguments(int argc, char** argv);

// This program's own file (Windows: GetModuleFileNameW; Linux: /proc/self/exe).
fs::path SelfExecutable();

}  // namespace kuf::setup
