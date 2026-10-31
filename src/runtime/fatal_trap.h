// Fatal guest kernel calls (KNOWN_ISSUES K12): KeBugCheck, KeBugCheckEx and DbgBreakPoint end the run with a
// crash report instead of returning to the game. See fatal_trap.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace kuf::diag {

using GuestImportFn = void (*)(PPCContext&, uint8_t*);

// The implementations (never return). Normal builds bind __imp__KeBugCheck, __imp__KeBugCheckEx and
// __imp__DbgBreakPoint to these.
[[noreturn]] void GuestKeBugCheck(PPCContext& ctx, uint8_t* base);
[[noreturn]] void GuestKeBugCheckEx(PPCContext& ctx, uint8_t* base);
[[noreturn]] void GuestDbgBreakPoint(PPCContext& ctx, uint8_t* base);

// Our implementation of a guest import that must not use the runtime's, or nullptr. Import-audit builds
// wrap every import and forward to this before falling back to the runtime library.
GuestImportFn FatalImportOverride(const char* symbol);

}  // namespace kuf::diag
