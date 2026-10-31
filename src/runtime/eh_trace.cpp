// Guest exception handling and non-local jumps (KNOWN_ISSUES K4/K5).
//
// setjmp/longjmp (v1: 0x82A184F0 / 0x828008C0) are configured in config/v1/codegen.toml, so every call site
// uses ReXGlue's native ppc_setjmp/ppc_longjmp. The guest bodies must therefore never run: if they do,
// control reached them some other way and the jump would resume in the wrong host frame, so fail fast.
//
// RaiseException (0x82828398) is the game's static Win32 wrapper around RtlRaiseException. The runtime
// handles only thread naming (0x406D1388); for any other code (C++ `throw` is 0xE06D7363) it calls
// debug::Break() and *returns*, which resumes after the throw site with corrupted state. Until guest
// exception dispatch exists, such exceptions end the run with a crash report naming the thrown type.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "runtime/crash_report.h"

REX_EXTERN(__imp__sub_82828398);  // RaiseException(code, flags, nargs, args)

namespace {

constexpr uint32_t kSetThreadNameException = 0x406D1388;
constexpr uint32_t kCppException = 0xE06D7363;

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

// MSVC C++ EH metadata (32-bit pointers on the Xbox 360): ThrowInfo+12 -> CatchableTypeArray,
// array+4 -> first CatchableType, CatchableType+4 -> TypeDescriptor, TypeDescriptor+8 -> mangled name.
void ThrownTypeName(const uint8_t* base, uint32_t throw_info, char* out, size_t size) {
  std::snprintf(out, size, "?");
  if (!throw_info) {
    return;
  }
  uint32_t array = LoadGuest32(base, throw_info + 12);
  if (!array || LoadGuest32(base, array) == 0) {
    return;
  }
  uint32_t catchable = LoadGuest32(base, array + 4);
  uint32_t type = catchable ? LoadGuest32(base, catchable + 4) : 0;
  if (type) {
    std::snprintf(out, size, "%.120s", reinterpret_cast<const char*>(base + type + 8));
  }
}

}  // namespace

REX_HOOK_RAW(sub_82828398) {
  const uint32_t code = ctx.r3.u32;
  if (code == kSetThreadNameException) {
    __imp__sub_82828398(ctx, base);
    return;
  }
  char reason[384];
  if (code == kCppException && ctx.r5.u32 >= 3 && ctx.r6.u32) {
    char type[160];
    ThrownTypeName(base, LoadGuest32(base, ctx.r6.u32 + 8), type, sizeof(type));
    std::snprintf(reason, sizeof(reason),
                  "guest C++ exception thrown (type %s) from guest lr=0x%08X; guest exception dispatch is "
                  "not implemented (KNOWN_ISSUES K4)", type, static_cast<uint32_t>(ctx.lr));
  } else {
    std::snprintf(reason, sizeof(reason),
                  "guest RaiseException code=0x%08X flags=0x%X from guest lr=0x%08X; guest exception dispatch "
                  "is not implemented (KNOWN_ISSUES K4)", code, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  REXLOG_ERROR("[kuf-eh] {}", reason);
  kuf::diag::FailFast(reason);
}

REX_HOOK_RAW(sub_82A184F0) {
  (void)base;
  char reason[160];
  std::snprintf(reason, sizeof(reason), "guest setjmp body reached (from lr=0x%08X); all call sites should use "
                "ppc_setjmp", static_cast<uint32_t>(ctx.lr));
  kuf::diag::FailFast(reason);
}

REX_HOOK_RAW(sub_828008C0) {
  (void)base;
  char reason[160];
  std::snprintf(reason, sizeof(reason), "guest longjmp body reached (from lr=0x%08X); all call sites should use "
                "ppc_longjmp", static_cast<uint32_t>(ctx.lr));
  kuf::diag::FailFast(reason);
}
