#include "runtime/crash_report.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <rex/exception_handler.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#if defined(__linux__)
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <process.h>
#endif

namespace kuf::diag {

namespace {

struct HostFunction {
  uintptr_t host;
  uint32_t guest;
};

std::string g_report_path;
rex::memory::Memory* g_memory = nullptr;
std::vector<HostFunction> g_functions;  // sorted by host address
std::atomic<bool> g_crashing{false};
char g_reason[512] = {};

// Fixed buffer: the report is built without allocating once the fault has happened.
char g_buffer[64 * 1024];
size_t g_used = 0;

void Append(const char* fmt, ...) {
  if (g_used >= sizeof(g_buffer)) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  int n = std::vsnprintf(g_buffer + g_used, sizeof(g_buffer) - g_used, fmt, args);
  va_end(args);
  if (n > 0) {
    g_used = std::min(sizeof(g_buffer), g_used + static_cast<size_t>(n));
  }
}

// Reads guest memory without faulting (the stack walk may follow garbage).
bool ReadGuest32(uint32_t address, uint32_t* out) {
  if (!g_memory) {
    return false;
  }
  const uint8_t* host = g_memory->virtual_membase() + address;
  uint32_t raw = 0;
#if defined(__linux__)
  iovec local{&raw, sizeof(raw)};
  iovec remote{const_cast<uint8_t*>(host), sizeof(raw)};
  if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != sizeof(raw)) {
    return false;
  }
#elif defined(_WIN32)
  SIZE_T read = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), host, &raw, sizeof(raw), &read) || read != sizeof(raw)) {
    return false;
  }
#else
  return false;
#endif
  *out = __builtin_bswap32(raw);
  return true;
}

// Nearest generated function at or below a host address. Only meaningful when the address lies in
// recompiled code; the symbolizer gives the exact instruction.
const HostFunction* NearestFunction(uintptr_t host_pc) {
  auto it = std::upper_bound(g_functions.begin(), g_functions.end(), host_pc,
                             [](uintptr_t pc, const HostFunction& f) { return pc < f.host; });
  if (it == g_functions.begin()) {
    return nullptr;
  }
  return &*(it - 1);
}

void AppendHostAddress(const char* label, uintptr_t address) {
#if defined(__linux__)
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fname) {
    const char* module = std::strrchr(info.dli_fname, '/');
    module = module ? module + 1 : info.dli_fname;
    Append("%s0x%016" PRIxPTR " (%s+0x%" PRIxPTR ")\n", label, address, module,
           address - reinterpret_cast<uintptr_t>(info.dli_fbase));
    return;
  }
#endif
  Append("%s0x%016" PRIxPTR "\n", label, address);
}

void AppendGuestFunction(const char* label, uint32_t guest_address) {
  const uint32_t best = ContainingGuestFunction(guest_address);
  if (best) {
    Append("%s0x%08X  in sub_%08X+0x%X\n", label, guest_address, best, guest_address - best);
  } else {
    Append("%s0x%08X\n", label, guest_address);
  }
}

void AppendGuestState() {
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts || !ts->context()) {
    Append("guest_thread: none (host thread)\n");
    return;
  }
  const PPCContext& c = *ts->context();
  if (rex::system::XThread::IsInThread()) {
    const auto* thread = rex::system::XThread::GetCurrentThread();
    Append("guest_thread: 0x%08X \"%.64s\"\n", ts->thread_id(), thread->name().c_str());
  } else {
    Append("guest_thread: 0x%08X\n", ts->thread_id());
  }
  Append("registers:\n");
  const PPCRegister* gpr[32] = {&c.r0,  &c.r1,  &c.r2,  &c.r3,  &c.r4,  &c.r5,  &c.r6,  &c.r7,
                                &c.r8,  &c.r9,  &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15,
                                &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21, &c.r22, &c.r23,
                                &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31};
  for (int i = 0; i < 32; ++i) {
    Append("  r%-2d 0x%016" PRIX64 "%s", i, gpr[i]->u64, (i % 4 == 3) ? "\n" : "");
  }
  uint32_t cr = (c.cr0.raw() << 28) | (c.cr1.raw() << 24) | (c.cr2.raw() << 20) | (c.cr3.raw() << 16) |
                (c.cr4.raw() << 12) | (c.cr5.raw() << 8) | (c.cr6.raw() << 4) | c.cr7.raw();
  Append("  lr  0x%08" PRIX64 "  ctr 0x%016" PRIX64 "  cr 0x%08X  last_indirect 0x%08X\n", c.lr, c.ctr.u64,
         cr, c.last_indirect_target);

  // Raw guest stack around r1 (callee save areas live just below r1, the frame's back chain at r1).
  Append("guest_stack (r1-0x100 .. r1+0x200):\n");
  for (uint32_t row = c.r1.u32 - 0x100; row < c.r1.u32 + 0x200; row += 16) {
    Append("  %08X:", row);
    for (uint32_t col = 0; col < 16; col += 4) {
      uint32_t word = 0;
      if (ReadGuest32(row + col, &word)) {
        Append(" %08X", word);
      } else {
        Append(" ????????");
      }
    }
    Append("%s\n", row == (c.r1.u32 & ~15u) ? "  <- r1" : "");
  }

  // Guest backtrace: LR first (the caller of the current leaf), then the r1 back chain. Each frame
  // stores the caller's LR at back_chain - 8 (Xbox 360 ABI: stw r12,-8(r1) before stwu).
  Append("guest_backtrace:\n");
  AppendGuestFunction("  #0 lr ", static_cast<uint32_t>(c.lr));
  uint32_t sp = c.r1.u32;
  for (int frame = 1; frame < 32; ++frame) {
    uint32_t back_chain = 0, saved_lr = 0;
    if (!ReadGuest32(sp, &back_chain) || back_chain == 0 || back_chain <= sp ||
        !ReadGuest32(back_chain - 8, &saved_lr)) {
      break;
    }
    char label[32];
    std::snprintf(label, sizeof(label), "  #%d    ", frame);
    AppendGuestFunction(label, saved_lr);
    sp = back_chain;
  }
}

void AppendHostBacktrace() {
#if defined(__linux__)
  void* frames[48];
  int count = backtrace(frames, 48);
  Append("host_backtrace:\n");
  for (int i = 0; i < count; ++i) {
    char label[16];
    std::snprintf(label, sizeof(label), "  #%-2d ", i);
    AppendHostAddress(label, reinterpret_cast<uintptr_t>(frames[i]));
  }
#endif
}

uint64_t HostThreadId() {
#if defined(__linux__)
  return static_cast<uint64_t>(syscall(SYS_gettid));
#elif defined(_WIN32)
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

void WriteReportAndExit() {
#if defined(__linux__)
  int fd = open(g_report_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd >= 0) {
    ssize_t ignored = write(fd, g_buffer, g_used);
    (void)ignored;
    close(fd);
  }
  ssize_t ignored = write(STDERR_FILENO, g_buffer, g_used);
  (void)ignored;
  _exit(kCrashExitCode);
#else
  if (FILE* f = std::fopen(g_report_path.c_str(), "wb")) {
    std::fwrite(g_buffer, 1, g_used, f);
    std::fclose(f);
  }
  std::fwrite(g_buffer, 1, g_used, stderr);
  std::fflush(stderr);
  _exit(kCrashExitCode);
#endif
}

bool LastResortHandler(rex::arch::Exception* ex, void* /*data*/) {
  if (g_crashing.exchange(true)) {
    // Another thread is already writing the report; it will terminate the process.
    for (;;) {
#if defined(__linux__)
      pause();
#else
      Sleep(1000);
#endif
    }
  }
  g_used = 0;
  Append("KUF-COD CRASH REPORT v1\n");
  if (g_reason[0]) {
    Append("reason: %s\n", g_reason);
  }
  const uintptr_t host_pc = static_cast<uintptr_t>(ex->pc());
  if (ex->code() == rex::arch::Exception::Code::kAccessViolation) {
    const char* op = "unknown";
    switch (ex->access_violation_operation()) {
      case rex::arch::Exception::AccessViolationOperation::kRead: op = "read"; break;
      case rex::arch::Exception::AccessViolationOperation::kWrite: op = "write"; break;
      default: break;
    }
    Append("kind: access violation (%s)\n", op);
    const uint64_t fault = ex->fault_address();
    const uint64_t base = g_memory ? reinterpret_cast<uint64_t>(g_memory->virtual_membase()) : 0;
    if (base && fault >= base && fault < base + 0x100000000ull) {
      Append("fault_guest: 0x%08" PRIX64 "\n", fault - base);
    } else {
      Append("fault_host: 0x%016" PRIX64 " (outside guest memory)\n", fault);
    }
  } else if (ex->code() == rex::arch::Exception::Code::kIllegalInstruction) {
    Append("kind: illegal instruction\n");
  } else {
    Append("kind: exception code %d\n", static_cast<int>(ex->code()));
  }
  AppendHostAddress("host_pc: ", host_pc);
  if (const HostFunction* f = NearestFunction(host_pc)) {
    Append("nearest_recompiled_function: sub_%08X (host +0x%" PRIxPTR "; exact instruction: "
           "tools/symbolize_crash.py)\n", f->guest, host_pc - f->host);
  }
  Append("host_thread: %" PRIu64 "\n", HostThreadId());
  AppendGuestState();
  AppendHostBacktrace();
  Append("end of report\n");
  WriteReportAndExit();
  return true;  // not reached
}

}  // namespace

uint32_t ContainingGuestFunction(uint32_t guest_address) {
  // Largest guest function start <= address (g_functions is sorted by host address, not guest).
  uint32_t best = 0;
  for (const auto& f : g_functions) {
    if (f.guest <= guest_address && f.guest > best) {
      best = f.guest;
    }
  }
  return best;
}

[[noreturn]] void FailFast(const char* reason) {
  std::snprintf(g_reason, sizeof(g_reason), "%s", reason);
  __builtin_trap();
}

void InstallCrashHandler(const CrashHandlerConfig& config) {
  g_report_path = config.report_path.string();
  g_memory = config.memory;
  g_functions.clear();
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    g_functions.push_back({reinterpret_cast<uintptr_t>(m->host), static_cast<uint32_t>(m->guest)});
  }
  std::sort(g_functions.begin(), g_functions.end(),
            [](const HostFunction& a, const HostFunction& b) { return a.host < b.host; });
  rex::arch::ExceptionHandler::Install(LastResortHandler, nullptr);
}

}  // namespace kuf::diag
