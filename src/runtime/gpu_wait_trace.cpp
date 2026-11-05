// M12 diagnostics: where the game's render thread waits for the GPU command processor.
//
// The game's D3D (sub_821647B0) blocks until the GPU has passed a fence: device +10896 points at the fence value the
// command processor writes, device +10908 is the last fence issued; the wait spins in sub_82160C90 (a few nops, then
// a time check). sub_82163800 waits for ring buffer space (the read pointer, device +10896 -> +4) and sub_8216AFA8
// for another GPU-written value. With --kuf_trace_gpu_waits=true each of them is timed per call site (the return
// address) and every 10 s the sites that waited are logged ("[kuf-gpu-wait]"): calls, calls that waited, the time
// spent waiting, and for fence waits how far behind the last issued fence the awaited one was. Nothing changes
// otherwise.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(kuf_trace_gpu_waits, false, "KUF/Debug",
                    "M12: time the game's waits for the GPU command processor (fences, ring space) per call site, "
                    "logged every 10 s");

REX_EXTERN(__imp__sub_821647B0);
REX_EXTERN(__imp__sub_82163800);
REX_EXTERN(__imp__sub_8216AFA8);
REX_EXTERN(__imp__sub_8215B768);

namespace {

using Clock = std::chrono::steady_clock;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

struct Site {
  uint64_t calls = 0, waited = 0, wait_ns = 0, max_ns = 0;
  uint64_t behind_sum = 0;  // fence waits: issued - awaited, summed over the calls that waited
};

// Resource locks (sub_8215B768) that waited, per caller, resource type (header word & 0xF) and lock flags.
struct LockKey {
  uint32_t caller, type, flags;
  auto operator<=>(const LockKey&) const = default;
};
struct LockSite {
  uint64_t calls = 0, waited = 0, wait_ns = 0;
  uint32_t last_resource = 0, resources_changed = 0;
};

std::mutex g_mutex;
std::map<std::pair<char, uint32_t>, Site> g_sites;  // (kind, return address)
std::map<LockKey, LockSite> g_locks;
Clock::time_point g_window_start = Clock::now();

void Report(Clock::time_point now) {
  const double window_s = std::chrono::duration<double>(now - g_window_start).count();
  uint64_t total_ns = 0;
  for (const auto& [key, s] : g_sites) total_ns += s.wait_ns;
  REXLOG_INFO("[kuf-gpu-wait] {:.1f} s: waiting {:.0f} ms in all", window_s, total_ns / 1e6);
  for (const auto& [key, s] : g_sites) {
    if (!s.waited) continue;
    REXLOG_INFO("[kuf-gpu-wait]   {} from {:08X}: {} calls, {} waited, {:.1f} ms (max {:.2f} ms){}", key.first,
                key.second, s.calls, s.waited, s.wait_ns / 1e6, s.max_ns / 1e6,
                key.first == 'F' ? fmt::format(", awaited fence {:.1f} behind the last issued",
                                               double(s.behind_sum) / double(s.waited))
                                 : std::string());
  }
  for (const auto& [key, s] : g_locks) {
    if (!s.waited) continue;
    REXLOG_INFO("[kuf-gpu-wait]   lock from {:08X} type {} flags {:04X}: {} calls, {} waited, {:.1f} ms, "
                "resource {:08X} ({} changes)",
                key.caller, key.type, key.flags, s.calls, s.waited, s.wait_ns / 1e6, s.last_resource,
                s.resources_changed);
  }
  g_sites.clear();
  g_locks.clear();
  g_window_start = now;
}

template <typename F>
void Timed(char kind, PPCContext& ctx, uint8_t* base, uint32_t behind, F&& call) {
  const uint32_t site = static_cast<uint32_t>(ctx.lr);
  const auto begin = Clock::now();
  call();
  const auto end = Clock::now();
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
  std::lock_guard lock(g_mutex);
  Site& s = g_sites[{kind, site}];
  ++s.calls;
  if (ns > 20000) {  // a call that returns at once takes well under 20 us
    ++s.waited;
    s.wait_ns += ns;
    s.max_ns = std::max(s.max_ns, ns);
    s.behind_sum += behind;
  }
  if (end - g_window_start >= std::chrono::seconds(10)) Report(end);
}

}  // namespace

// Fence wait: r3 = device, r4 = the fence awaited.
REX_HOOK_RAW(sub_821647B0) {
  if (!REXCVAR_GET(kuf_trace_gpu_waits)) {
    __imp__sub_821647B0(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32, fence = ctx.r4.u32;
  const uint32_t behind = Load32(base, device + 10908) - fence;
  Timed('F', ctx, base, behind, [&] { __imp__sub_821647B0(ctx, base); });
}

// Resource lock: r3 = the resource (its first word's low 4 bits: the type), r10 = the lock flags.
REX_HOOK_RAW(sub_8215B768) {
  if (!REXCVAR_GET(kuf_trace_gpu_waits)) {
    __imp__sub_8215B768(ctx, base);
    return;
  }
  const uint32_t caller = static_cast<uint32_t>(ctx.lr), resource = ctx.r3.u32, flags = ctx.r10.u32;
  const uint32_t type = Load32(base, resource) & 0xF;
  const auto begin = Clock::now();
  __imp__sub_8215B768(ctx, base);
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
  std::lock_guard lock(g_mutex);
  LockSite& s = g_locks[{caller, type, flags}];
  ++s.calls;
  if (ns > 20000) {
    ++s.waited;
    s.wait_ns += ns;
  }
  if (s.last_resource != resource) ++s.resources_changed;
  s.last_resource = resource;
}

// Ring space wait.
REX_HOOK_RAW(sub_82163800) {
  if (!REXCVAR_GET(kuf_trace_gpu_waits)) {
    __imp__sub_82163800(ctx, base);
    return;
  }
  Timed('R', ctx, base, 0, [&] { __imp__sub_82163800(ctx, base); });
}

REX_HOOK_RAW(sub_8216AFA8) {
  if (!REXCVAR_GET(kuf_trace_gpu_waits)) {
    __imp__sub_8216AFA8(ctx, base);
    return;
  }
  Timed('O', ctx, base, 0, [&] { __imp__sub_8216AFA8(ctx, base); });
}
