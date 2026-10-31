// KeBugCheck, KeBugCheckEx and DbgBreakPoint must end the run with a crash report and exit 70 instead of
// returning to the game (KNOWN_ISSUES K12). Each case calls the import the way recompiled code does
// (__imp__<name> with the guest context; this executable links src/runtime/fatal_trap.cpp like the game)
// in a forked child, from the game's real KeBugCheck call site in sub_821667C8 (bl at 0x821668F0).

#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc.h>

#include "guest_env.h"
#include "runtime/crash_report.h"
#include "runtime/fatal_trap.h"

extern "C" void __imp__KeBugCheck(PPCContext&, uint8_t*);
extern "C" void __imp__KeBugCheckEx(PPCContext&, uint8_t*);
extern "C" void __imp__DbgBreakPoint(PPCContext&, uint8_t*);

namespace {

constexpr uint32_t kReturnAddress = 0x821668F4;  // after `bl KeBugCheck` in sub_821667C8

struct ChildResult {
  int exit_code = -1;
  std::string report;
};

// Runs `import` in a forked child with r3..r7 set; the child exits 0 if the call returns.
ChildResult RunImport(kuf::diag::GuestImportFn import, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0,
                      uint32_t r6 = 0, uint32_t r7 = 0) {
  namespace fs = std::filesystem;
  static int counter = 0;
  const fs::path report = fs::temp_directory_path() /
                          ("kuf_fatal_test_" + std::to_string(::getpid()) + "_" + std::to_string(++counter) + ".txt");
  fs::remove(report);
  auto& env = kuf::test::GuestEnv::Get();
  pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    kuf::test::GuestEnv::RestoreRuntimeSignalHandlers();
    kuf::diag::InstallCrashHandler({report, env.memory()});
    PPCContext ctx{};
    ctx.r1.u64 = kuf::test::GuestEnv::kStackTop - 0x100;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    ctx.lr = kReturnAddress;
    import(ctx, env.base());
    _exit(0);  // reaching here means the fatal call returned to the game
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  ChildResult result;
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }
  std::ifstream in(report);
  result.report.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  fs::remove(report);
  return result;
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("KeBugCheck ends the run with a crash report", "[image][crash][k12]") {
  auto r = RunImport(__imp__KeBugCheck, 0xF5);
  CHECK(r.exit_code == kuf::diag::kCrashExitCode);
  CHECK(Has(r.report, "KUF-COD CRASH REPORT v1"));
  CHECK(Has(r.report, "reason: guest KeBugCheck(code=0x000000F5) called from guest 0x821668F0 "
                      "(sub_821667C8+0x128)"));
  CHECK(Has(r.report, "kind: illegal instruction"));
  CHECK(Has(r.report, "end of report"));
}

TEST_CASE("KeBugCheckEx reports the code and all four parameters", "[image][crash][k12]") {
  auto r = RunImport(__imp__KeBugCheckEx, 0xDEADBEEF, 0x11111111, 0x22222222, 0x33333333, 0x44444444);
  CHECK(r.exit_code == kuf::diag::kCrashExitCode);
  CHECK(Has(r.report, "reason: guest KeBugCheckEx(code=0xDEADBEEF, 0x11111111, 0x22222222, 0x33333333, "
                      "0x44444444) called from guest 0x821668F0 (sub_821667C8+0x128)"));
}

TEST_CASE("DbgBreakPoint is fatal without a debugger", "[image][crash][k12]") {
  auto r = RunImport(__imp__DbgBreakPoint, 0);
  CHECK(r.exit_code == kuf::diag::kCrashExitCode);
  CHECK(Has(r.report, "reason: guest DbgBreakPoint called from guest 0x821668F0 (sub_821667C8+0x128)"));
  CHECK(Has(r.report, "unhandled breakpoint exception"));
}

TEST_CASE("only the fatal imports are overridden", "[image][k12]") {
  CHECK(kuf::diag::FatalImportOverride("__imp__KeBugCheck") == &kuf::diag::GuestKeBugCheck);
  CHECK(kuf::diag::FatalImportOverride("__imp__KeBugCheckEx") == &kuf::diag::GuestKeBugCheckEx);
  CHECK(kuf::diag::FatalImportOverride("__imp__DbgBreakPoint") == &kuf::diag::GuestDbgBreakPoint);
  CHECK(kuf::diag::FatalImportOverride("__imp__RtlRaiseException") == nullptr);
  CHECK(kuf::diag::FatalImportOverride("__imp__NtCreateFile") == nullptr);
}
