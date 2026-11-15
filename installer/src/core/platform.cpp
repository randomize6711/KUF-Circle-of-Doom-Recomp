#include "core/platform.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace kuf::setup {

std::string PathUtf8(const fs::path& path) {
  const std::u8string u = path.u8string();
  return std::string(u.begin(), u.end());
}

fs::path PathFromUtf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string QuoteWindowsArgument(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
  std::string out = "\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\');  // before the closing quote: all doubled
      break;
    }
    if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');  // doubled, and the quote escaped
    } else {
      out.append(backslashes, '\\');
    }
    out += arg[i];
  }
  return out + "\"";
}

#if defined(_WIN32)

namespace {
std::wstring Wide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
  return out;
}
HANDLE H(void* h) { return static_cast<HANDLE>(h); }
}  // namespace

InFile::~InFile() {
  if (handle_) ::CloseHandle(H(handle_));
}
bool InFile::Open(const fs::path& path) {
  if (handle_) ::CloseHandle(H(handle_));
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  handle_ = h == INVALID_HANDLE_VALUE ? nullptr : h;
  return handle_ != nullptr;
}
bool InFile::ok() const { return handle_ != nullptr; }
int64_t InFile::Read(void* out, size_t size) {
  DWORD got = 0;
  if (!::ReadFile(H(handle_), out, DWORD(size > 0x40000000 ? 0x40000000 : size), &got, nullptr)) return -1;
  return int64_t(got);
}
bool InFile::Seek(uint64_t offset) {
  LARGE_INTEGER at;
  at.QuadPart = LONGLONG(offset);
  return ::SetFilePointerEx(H(handle_), at, nullptr, FILE_BEGIN) != 0;
}
bool InFile::ReadAt(uint64_t offset, void* out, size_t size) {
  auto* p = static_cast<char*>(out);
  while (size > 0) {
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD got = 0;
    const DWORD want = DWORD(size > 0x40000000 ? 0x40000000 : size);
    if (!::ReadFile(H(handle_), p, want, &got, &ov) || got == 0) return false;
    p += got;
    offset += got;
    size -= got;
  }
  return true;
}

OutFile::~OutFile() {
  if (handle_) ::CloseHandle(H(handle_));
}
bool OutFile::Create(const fs::path& path, bool) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  handle_ = h == INVALID_HANDLE_VALUE ? nullptr : h;
  return handle_ != nullptr;
}
bool OutFile::Write(const void* data, size_t size) {
  const auto* p = static_cast<const char*>(data);
  while (size > 0) {
    DWORD done = 0;
    if (!::WriteFile(H(handle_), p, DWORD(size > 0x40000000 ? 0x40000000 : size), &done, nullptr) || done == 0) {
      return false;
    }
    p += done;
    size -= done;
  }
  return true;
}
bool OutFile::Sync() { return handle_ && ::FlushFileBuffers(H(handle_)) != 0; }
bool OutFile::Close() {
  if (!handle_) return false;
  const bool ok = ::CloseHandle(H(handle_)) != 0;
  handle_ = nullptr;
  return ok;
}

fs::path PlatformHomeDir() {
  if (const wchar_t* p = _wgetenv(L"USERPROFILE"); p && *p) return fs::path(p);
  return fs::path(L"C:\\");
}

uint64_t AvailableMemory() {
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof(m);
  return ::GlobalMemoryStatusEx(&m) ? uint64_t(m.ullAvailPhys) : uint64_t(4) << 30;
}

bool ChildProcess::running() const { return process != nullptr; }

bool StartProcess(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log,
                  const std::vector<std::string>& env, ChildProcess* child) {
  std::string line;
  for (const std::string& a : argv) line += (line.empty() ? "" : " ") + QuoteWindowsArgument(a);
  std::wstring wline = Wide(line);
  // The environment block: this process's, with `env` replacing or adding (sorted order is not required).
  std::wstring block;
  {
    std::vector<std::wstring> mine;
    for (const std::string& e : env) mine.push_back(Wide(e));
    wchar_t* base = ::GetEnvironmentStringsW();
    for (const wchar_t* e = base; e && *e; e += std::wcslen(e) + 1) {
      const std::wstring_view entry(e);
      bool replaced = false;
      for (const std::wstring& m : mine) {
        const size_t eq = m.find(L'=');
        if (eq != std::wstring::npos && _wcsnicmp(entry.data(), m.c_str(), eq + 1) == 0) replaced = true;
      }
      if (!replaced) block.append(entry).push_back(L'\0');
    }
    if (base) ::FreeEnvironmentStringsW(base);
    for (const std::wstring& m : mine) block.append(m).push_back(L'\0');
    block.push_back(L'\0');
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out = ::CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE in = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in;
  si.hStdOutput = out;
  si.hStdError = out;
  HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  }
  PROCESS_INFORMATION pi{};
  const std::wstring wcwd = cwd.empty() ? std::wstring() : cwd.wstring();
  const BOOL ok = ::CreateProcessW(nullptr, wline.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | BELOW_NORMAL_PRIORITY_CLASS |
                                       CREATE_SUSPENDED,
                                   block.data(), wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
  if (out != INVALID_HANDLE_VALUE) ::CloseHandle(out);
  if (in != INVALID_HANDLE_VALUE) ::CloseHandle(in);
  if (!ok) {
    if (job) ::CloseHandle(job);
    return false;
  }
  if (job) ::AssignProcessToJobObject(job, pi.hProcess);
  ::ResumeThread(pi.hThread);
  ::CloseHandle(pi.hThread);
  child->process = pi.hProcess;
  child->job = job;
  return true;
}

bool PollProcess(ChildProcess* child, int* exit_code, bool* out_of_memory) {
  if (!child->process) return true;
  if (::WaitForSingleObject(H(child->process), 0) != WAIT_OBJECT_0) return false;
  DWORD code = 1;
  ::GetExitCodeProcess(H(child->process), &code);
  *exit_code = int(code);
  *out_of_memory = false;
  ::CloseHandle(H(child->process));
  if (child->job) ::CloseHandle(H(child->job));
  child->process = child->job = nullptr;
  return true;
}

void KillProcess(ChildProcess* child) {
  if (!child->process) return;
  if (child->job) {
    ::TerminateJobObject(H(child->job), 1);
  } else {
    ::TerminateProcess(H(child->process), 1);
  }
  ::WaitForSingleObject(H(child->process), INFINITE);
  ::CloseHandle(H(child->process));
  if (child->job) ::CloseHandle(H(child->job));
  child->process = child->job = nullptr;
}


bool StartDetached(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log,
                   const std::vector<std::string>& env) {
  ChildProcess c;
  if (!StartProcess(argv, cwd, log, env, &c)) return false;
  // The game at normal priority: StartProcess lowers it for the compiler, and a game below normal loses the processor
  // to any background program (stutter). As on Linux, where a detached program is not lowered.
  ::SetPriorityClass(H(c.process), NORMAL_PRIORITY_CLASS);
  // Out of the kill-on-close job: the game stays when the launcher ends.
  if (c.job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    ::SetInformationJobObject(H(c.job), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    ::CloseHandle(H(c.job));
  }
  ::CloseHandle(H(c.process));
  return true;
}

fs::path UserConfigDir() {
  if (const wchar_t* p = _wgetenv(L"APPDATA"); p && *p) return fs::path(p) / L"kuf-cod-recomp";
  return PlatformHomeDir() / L"AppData" / L"Roaming" / L"kuf-cod-recomp";
}

fs::path GetKnownFolder(KnownFolder which) {
  const KNOWNFOLDERID* id = &FOLDERID_LocalAppData;
  if (which == KnownFolder::kPrograms) id = &FOLDERID_UserProgramFiles;
  if (which == KnownFolder::kStartMenuPrograms) id = &FOLDERID_Programs;
  if (which == KnownFolder::kDesktop) id = &FOLDERID_Desktop;
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(::SHGetKnownFolderPath(*id, KF_FLAG_CREATE, nullptr, &p)) && p) out = fs::path(p);
  if (p) ::CoTaskMemFree(p);
  return out;
}

bool CreateShortcut(const fs::path& link, const fs::path& target, const std::string& arguments,
                    const fs::path& working_dir, const std::string& description) {
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IShellLinkW* sl = nullptr;
  bool ok = SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                         reinterpret_cast<void**>(&sl)));
  if (ok) {
    sl->SetPath(target.c_str());
    sl->SetArguments(Wide(arguments).c_str());
    sl->SetWorkingDirectory(working_dir.c_str());
    sl->SetDescription(Wide(description).c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf)));
    if (ok) {
      ok = SUCCEEDED(pf->Save(link.c_str(), TRUE));
      pf->Release();
    }
    sl->Release();
  }
  if (SUCCEEDED(init)) ::CoUninitialize();
  return ok;
}

#else  // Linux

InFile::~InFile() {
  if (fd_ >= 0) ::close(fd_);
}
bool InFile::Open(const fs::path& path) {
  if (fd_ >= 0) ::close(fd_);
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  return fd_ >= 0;
}
bool InFile::ok() const { return fd_ >= 0; }
int64_t InFile::Read(void* out, size_t size) {
  for (;;) {
    const ssize_t n = ::read(fd_, out, size);
    if (n < 0 && errno == EINTR) continue;
    return int64_t(n);
  }
}
bool InFile::Seek(uint64_t offset) { return ::lseek(fd_, off_t(offset), SEEK_SET) == off_t(offset); }
bool InFile::ReadAt(uint64_t offset, void* out, size_t size) {
  auto* p = static_cast<char*>(out);
  while (size > 0) {
    const ssize_t n = ::pread(fd_, p, size, off_t(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    offset += uint64_t(n);
    size -= size_t(n);
  }
  return true;
}

OutFile::~OutFile() {
  if (fd_ >= 0) ::close(fd_);
}
bool OutFile::Create(const fs::path& path, bool private_mode) {
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, private_mode ? 0600 : 0644);
  return fd_ >= 0;
}
bool OutFile::Write(const void* data, size_t size) {
  const auto* p = static_cast<const char*>(data);
  while (size > 0) {
    const ssize_t n = ::write(fd_, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    size -= size_t(n);
  }
  return true;
}
bool OutFile::Sync() { return fd_ >= 0 && ::fsync(fd_) == 0; }
bool OutFile::Close() {
  if (fd_ < 0) return false;
  const bool ok = ::close(fd_) == 0;
  fd_ = -1;
  return ok;
}

fs::path PlatformHomeDir() {
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
  if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return fs::path(pw->pw_dir);
  return fs::path("/");
}

uint64_t AvailableMemory() {
  std::ifstream in("/proc/meminfo");
  std::string key, unit;
  uint64_t kib = 0;
  while (in >> key >> kib) {
    std::getline(in, unit);
    if (key == "MemAvailable:") return kib << 10;
  }
  return uint64_t(4) << 30;
}

bool ChildProcess::running() const { return pid > 0; }

// Everything the child uses is made before the fork (the setup has threads).
bool StartProcess(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log,
                  const std::vector<std::string>& env, ChildProcess* child) {
  std::vector<char*> args;
  for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  std::vector<char*> envp;
  for (const std::string& e : env) envp.push_back(const_cast<char*>(e.c_str()));
  for (char** e = environ; *e; ++e) {
    bool replaced = false;
    for (const std::string& mine : env) {
      const size_t eq = mine.find('=');
      if (std::string_view(*e).substr(0, eq + 1) == std::string_view(mine).substr(0, eq + 1)) replaced = true;
    }
    if (!replaced) envp.push_back(*e);
  }
  envp.push_back(nullptr);
  const std::string program = argv[0], where = cwd.string(), log_path = log.string();
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL);  // it does not outlive a killed setup
    setpriority(PRIO_PROCESS, 0, 5);   // the desktop stays responsive
    const int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
    }
    if (!where.empty() && chdir(where.c_str()) != 0) _exit(126);
    execve(program.c_str(), args.data(), envp.data());
    _exit(127);
  }
  child->pid = pid;
  return true;
}

bool PollProcess(ChildProcess* child, int* exit_code, bool* out_of_memory) {
  if (child->pid <= 0) return true;
  int status = 0;
  const pid_t r = waitpid(child->pid, &status, WNOHANG);
  if (r == 0) return false;
  child->pid = -1;
  if (r < 0) {
    *exit_code = 1;
    *out_of_memory = false;
    return true;
  }
  *out_of_memory = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
  *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  return true;
}

void KillProcess(ChildProcess* child) {
  if (child->pid <= 0) return;
  kill(-child->pid, SIGKILL);
  waitpid(child->pid, nullptr, 0);
  child->pid = -1;
}

bool StartDetached(const std::vector<std::string>& argv, const fs::path& cwd, const fs::path& log,
                   const std::vector<std::string>& env) {
  // A child that starts its own session and the program from a second fork (no zombie, no tie to the launcher).
  std::vector<char*> args;
  for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  std::vector<std::string> keep;
  std::vector<char*> envp;
  for (const std::string& e : env) envp.push_back(const_cast<char*>(e.c_str()));
  for (char** e = environ; *e; ++e) {
    bool replaced = false;
    for (const std::string& mine : env) {
      const size_t eq = mine.find('=');
      if (std::string_view(*e).substr(0, eq + 1) == std::string_view(mine).substr(0, eq + 1)) replaced = true;
    }
    if (!replaced) envp.push_back(*e);
  }
  envp.push_back(nullptr);
  const std::string program = argv[0], where = cwd.string(), log_path = log.string();
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    if (fork() != 0) _exit(0);
    const int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
    }
    if (!where.empty() && chdir(where.c_str()) != 0) _exit(126);
    execve(program.c_str(), args.data(), envp.data());
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

fs::path UserConfigDir() {
  const char* x = std::getenv("XDG_CONFIG_HOME");
  if (x && *x == '/') return fs::path(x) / "kuf-cod-recomp";
  return PlatformHomeDir() / ".config" / "kuf-cod-recomp";
}

fs::path GetKnownFolder(KnownFolder) { return {}; }
bool CreateShortcut(const fs::path&, const fs::path&, const std::string&, const fs::path&, const std::string&) {
  return false;
}

#endif

std::vector<std::string> Utf8Arguments(int argc, char** argv) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  std::vector<std::string> out;
  int n = 0;
  if (wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n)) {
    for (int i = 0; i < n; ++i) out.push_back(PathUtf8(fs::path(w[i])));
    LocalFree(w);
  }
  return out;
#else
  return std::vector<std::string>(argv, argv + argc);
#endif
}

fs::path SelfExecutable() {
#if defined(_WIN32)
  std::wstring buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
  buf.resize(n);
  return fs::path(buf);
#else
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec);
#endif
}

}  // namespace kuf::setup
