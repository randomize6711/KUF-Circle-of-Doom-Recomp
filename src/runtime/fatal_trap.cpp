// Fatal guest kernel calls (KNOWN_ISSUES K12).
//
// The runtime implements KeBugCheck/KeBugCheckEx and DbgBreakPoint with rex::debug::Break(). Without a
// debugger the first call simply returns to the game, as if the fatal error had not happened, and a second
// call kills the process with an unreported SIGTRAP. On a retail console both are fatal: a bug check halts
// the system, and a breakpoint with no debugger attached is an unhandled STATUS_BREAKPOINT exception.
//
// The recompiled code calls imports by symbol (__imp__<name>), and the executable's definitions bind ahead
// of the runtime library's, so these end the run through the crash handler (exit 70). The report's reason
// line carries the bug-check code and parameters, the guest call site (the `bl` before the return address)
// with its containing function, and the guest thread; the report adds registers and the guest backtrace.
// Import-audit builds define a wrapper for every import and reach these through FatalImportOverride().

#include "runtime/fatal_trap.h"

#include <cstdio>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

#include "runtime/crash_report.h"

namespace kuf::diag {
namespace {

void DescribeCaller(const PPCContext& ctx, char* out, size_t size) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t call_site = lr - 4;
  const uint32_t function = ContainingGuestFunction(call_site);
  const auto* ts = rex::runtime::ThreadState::Get();
  char where[48] = "";
  if (function) {
    std::snprintf(where, sizeof(where), " (sub_%08X+0x%X)", function, call_site - function);
  }
  std::snprintf(out, size, "called from guest 0x%08X%s, guest thread 0x%08X", call_site, where,
                ts ? ts->thread_id() : 0u);
}

[[noreturn]] void BugCheck(PPCContext& ctx, bool extended) {
  char caller[128];
  DescribeCaller(ctx, caller, sizeof(caller));
  char reason[384];
  if (extended) {
    std::snprintf(reason, sizeof(reason),
                  "guest KeBugCheckEx(code=0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X) %s; a bug check is fatal "
                  "on the console (KNOWN_ISSUES K12)",
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, caller);
  } else {
    std::snprintf(reason, sizeof(reason),
                  "guest KeBugCheck(code=0x%08X) %s; a bug check is fatal on the console (KNOWN_ISSUES K12)",
                  ctx.r3.u32, caller);
  }
  REXLOG_ERROR("[kuf-fatal] {}", reason);
  FailFast(reason);
}

}  // namespace

[[noreturn]] void GuestKeBugCheck(PPCContext& ctx, uint8_t* /*base*/) { BugCheck(ctx, false); }

[[noreturn]] void GuestKeBugCheckEx(PPCContext& ctx, uint8_t* /*base*/) { BugCheck(ctx, true); }

[[noreturn]] void GuestDbgBreakPoint(PPCContext& ctx, uint8_t* /*base*/) {
  char caller[128];
  DescribeCaller(ctx, caller, sizeof(caller));
  char reason[256];
  std::snprintf(reason, sizeof(reason),
                "guest DbgBreakPoint %s; without a debugger this is an unhandled breakpoint exception "
                "(KNOWN_ISSUES K12)", caller);
  REXLOG_ERROR("[kuf-fatal] {}", reason);
  FailFast(reason);
}

GuestImportFn FatalImportOverride(const char* symbol) {
  if (std::strcmp(symbol, "__imp__KeBugCheck") == 0) return GuestKeBugCheck;
  if (std::strcmp(symbol, "__imp__KeBugCheckEx") == 0) return GuestKeBugCheckEx;
  if (std::strcmp(symbol, "__imp__DbgBreakPoint") == 0) return GuestDbgBreakPoint;
  return nullptr;
}

}  // namespace kuf::diag

#if !defined(KUFCOD_IMPORT_AUDIT)
REX_HOOK_RAW(__imp__KeBugCheck) { kuf::diag::GuestKeBugCheck(ctx, base); }
REX_HOOK_RAW(__imp__KeBugCheckEx) { kuf::diag::GuestKeBugCheckEx(ctx, base); }
REX_HOOK_RAW(__imp__DbgBreakPoint) { kuf::diag::GuestDbgBreakPoint(ctx, base); }
#endif
