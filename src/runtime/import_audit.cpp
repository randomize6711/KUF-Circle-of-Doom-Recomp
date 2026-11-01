// Import audit (diagnostic build option KUFCOD_IMPORT_AUDIT, Linux only).
//
// The recompiled code calls each kernel/XAM import by its symbol (__imp__Name), implemented in the ReXGlue
// runtime library. In audit builds the executable defines a wrapper for each of the game's imports, which
// the static link binds the generated code to: it counts the call, logs the first call with the guest call
// site, and forwards to the runtime's implementation (dlsym RTLD_NEXT). Counts land in run-summary.json.
// Behaviour is unchanged; never enable this in release builds.

#include "runtime/import_audit.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ostream>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/thread_state.h>

#include <set>
#include <sstream>
#include <string>

REXCVAR_DEFINE_STRING(kuf_audit_log, "", "KUF",
                      "Import audit builds: comma-separated imports to log on every call (with r3-r6)");

#include "coop/coop_runtime.h"
#include "runtime/crash_report.h"
#include "runtime/fatal_trap.h"

namespace kuf::audit {
namespace {

using ImportFn = void (*)(PPCContext&, uint8_t*);

struct Entry {
  const char* name;
  std::atomic<uint64_t> calls{0};
  std::atomic<uint32_t> first_lr{0};
  std::atomic<double> first_call_s{0};
};

#define KUF_IMPORT(name, index) {#name},
Entry g_entries[] = {
#include "runtime/import_audit_table.inc"
};
#undef KUF_IMPORT

const auto g_start = std::chrono::steady_clock::now();

const std::set<std::string>& AlwaysLogged() {
  static const std::set<std::string> names = [] {
    std::set<std::string> out;
    std::stringstream list(REXCVAR_GET(kuf_audit_log));
    for (std::string item; std::getline(list, item, ',');) {
      if (!item.empty()) out.insert(item);
    }
    return out;
  }();
  return names;
}

}  // namespace

ImportFn Resolve(const char* symbol) {
  if (ImportFn override_fn = kuf::diag::FatalImportOverride(symbol)) {
    return override_fn;  // our implementation replaces the runtime's (fatal_trap.cpp)
  }
  if (ImportFn coop_fn = kuf::coop::ImportOverride(symbol)) {
    return coop_fn;  // the developer co-op runtime (forwards to the runtime's when off; coop/coop_runtime.cpp)
  }
  void* fn = dlsym(RTLD_NEXT, symbol);
  if (!fn) {
    REXLOG_ERROR("[kuf-import] runtime has no implementation for {}", symbol);
    kuf::diag::FailFast("import audit: runtime implementation missing");
  }
  return reinterpret_cast<ImportFn>(fn);
}

void Hit(int index, const PPCContext& ctx) {
  Entry& e = g_entries[index];
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint64_t n = e.calls.fetch_add(1, std::memory_order_relaxed);
  if (n == 0) {
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    e.first_lr.store(lr);
    e.first_call_s.store(t);
  }
  if (n < 4 || AlwaysLogged().count(e.name)) {  // first calls (and selected imports) with their arguments
    REXLOG_INFO("[kuf-import] {} #{} tid=0x{:X} lr=0x{:08X} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} r6=0x{:08X} "
                "r7=0x{:08X} r8=0x{:08X} r9=0x{:08X}", e.name, n + 1, rex::runtime::ThreadState::GetThreadID(), lr,
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
  }
}

bool Enabled() { return true; }

void WriteJson(std::ostream& out) {
  out << "  \"imports\": [\n";
  bool first = true;
  for (const Entry& e : g_entries) {
    uint64_t calls = e.calls.load();
    if (!calls) {
      continue;
    }
    out << (first ? "" : ",\n") << "    {\"name\": \"" << e.name << "\", \"calls\": " << calls
        << ", \"first_call_s\": " << e.first_call_s.load() << ", \"first_lr\": " << e.first_lr.load() << "}";
    first = false;
  }
  out << "\n  ],\n";
}

}  // namespace kuf::audit

#define KUF_IMPORT(name, index)                                                     \
  extern "C" void __imp__##name(PPCContext& ctx, uint8_t* base) {                   \
    static const auto real = kuf::audit::Resolve("__imp__" #name);                  \
    kuf::audit::Hit(index, ctx);                                                    \
    real(ctx, base);                                                                \
  }
#include "runtime/import_audit_table.inc"
#undef KUF_IMPORT
