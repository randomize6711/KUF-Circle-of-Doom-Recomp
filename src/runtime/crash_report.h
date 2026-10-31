// Fail-fast crash reporting for the recompiled game.
//
// The SDK's POSIX signal handler returns without changing the thread context when no registered handler
// claims a fault, so an invalid guest access re-executes forever (KNOWN_ISSUES K3). InstallCrashHandler()
// registers a last-resort handler, after the SDK's MMIO/write-watch handlers, that claims every remaining
// access violation or illegal instruction, writes one deterministic report and terminates the process with
// kCrashExitCode. tools/symbolize_crash.py turns the host frames in the report into the original Xbox 360
// instruction addresses.
#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::memory {
class Memory;
}

namespace kuf::diag {

inline constexpr int kCrashExitCode = 70;

struct CrashHandlerConfig {
  std::filesystem::path report_path;   // written once, on the first fault
  rex::memory::Memory* memory = nullptr;  // guest memory, for address translation and the stack walk
};

// Must be called after the runtime has installed its own exception handlers (ReXApp::OnPostSetup), so
// that MMIO and write-watch faults are still handled by the SDK first.
void InstallCrashHandler(const CrashHandlerConfig& config);

// Start address of the recompiled guest function containing `guest_address`, or 0 if unknown (before
// InstallCrashHandler, or outside the image).
uint32_t ContainingGuestFunction(uint32_t guest_address);

// Terminates through the crash handler (illegal-instruction trap) with `reason` recorded in the report.
// For conditions the runtime cannot handle correctly, so they never continue silently.
[[noreturn]] void FailFast(const char* reason);

}  // namespace kuf::diag
