// The crash handler must turn an unhandled guest fault into one report and exit status 70 (never a retry
// loop). A forked child runs the game's recompiled adler32 on an unmapped buffer at guest 0x1A4, the
// address of the M1/M2 null-object fault (KNOWN_ISSUES K2/K3).

#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xmemory.h>

#include "guest_env.h"
#include "runtime/crash_report.h"

extern "C" void sub_8224C420(PPCContext&, uint8_t*);  // zlib adler32(adler, buf, len)

TEST_CASE("unhandled guest fault produces a crash report and exit 70", "[image][crash]") {
  namespace fs = std::filesystem;
  const fs::path report = fs::temp_directory_path() / ("kuf_crash_test_" + std::to_string(::getpid()) + ".txt");
  fs::remove(report);

  auto& env = kuf::test::GuestEnv::Get();  // guest memory exists before the fork
  pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    kuf::test::GuestEnv::RestoreRuntimeSignalHandlers();
    kuf::diag::InstallCrashHandler({report, env.memory()});
    env.Call(sub_8224C420, 1, 0x1A4, 16);  // reads guest 0x1A4: unmapped
    _exit(0);                              // reaching here means the fault was swallowed
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == kuf::diag::kCrashExitCode);

  std::ifstream in(report);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(text.find("KUF-COD CRASH REPORT v1") != std::string::npos);
  CHECK(text.find("kind: access violation (read)") != std::string::npos);
  CHECK(text.find("fault_guest: 0x000001A4") != std::string::npos);
  CHECK(text.find("nearest_recompiled_function: sub_8224C420") != std::string::npos);
  CHECK(text.find("end of report") != std::string::npos);
  fs::remove(report);
}
