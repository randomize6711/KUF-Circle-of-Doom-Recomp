#include "guest_env.h"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <rex/system/xmemory.h>

#if defined(__unix__)
#include <csignal>
#endif

namespace kuf::test {

namespace {

#if defined(__unix__)
// The runtime's fault handlers, recorded right after guest memory installed them (see
// RestoreRuntimeSignalHandlers).
struct sigaction g_runtime_sigill {};
struct sigaction g_runtime_sigsegv {};
#endif

constexpr uint32_t kReserveCommit =
    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
constexpr uint32_t kReadWrite = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

void AllocOrThrow(rex::memory::Memory& memory, uint32_t address, uint32_t size) {
  auto* heap = memory.LookupHeap(address);
  if (!heap || !heap->AllocFixed(address, size, 0x1000, kReserveCommit, kReadWrite)) {
    throw std::runtime_error("guest allocation failed");
  }
}

}  // namespace

GuestEnv& GuestEnv::Get() {
  static GuestEnv env;
  return env;
}

GuestEnv::GuestEnv() {
  static rex::memory::Memory memory;
  if (!memory.Initialize()) {
    throw std::runtime_error("failed to initialize guest memory");
  }
  AllocOrThrow(memory, kImageBase, kImageSize);
  AllocOrThrow(memory, kScratchBase, kScratchSize);
  AllocOrThrow(memory, kStackTop - 0x100000, 0x100000);
  base_ = memory.virtual_membase();
  memory_ = &memory;
#if defined(__unix__)
  sigaction(SIGILL, nullptr, &g_runtime_sigill);
  sigaction(SIGSEGV, nullptr, &g_runtime_sigsegv);
#endif

  std::ifstream in(KUFCOD_IMAGE_BIN, std::ios::binary);
  if (!in) {
    throw std::runtime_error("missing image: run tools/xeximage.py (see docs/TESTING.md): " KUFCOD_IMAGE_BIN);
  }
  std::vector<char> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (image.size() != kImageSize) {
    throw std::runtime_error("unexpected image size");
  }
  std::memcpy(base_ + kImageBase, image.data(), image.size());
}

void GuestEnv::RestoreRuntimeSignalHandlers() {
#if defined(__unix__)
  sigaction(SIGILL, &g_runtime_sigill, nullptr);
  sigaction(SIGSEGV, &g_runtime_sigsegv, nullptr);
#endif
}

uint32_t GuestEnv::Put(std::span<const uint8_t> bytes) {
  uint32_t address = scratch_top_;
  if (bytes.size() > kScratchBase + kScratchSize - address) {
    throw std::runtime_error("scratch exhausted");
  }
  std::memcpy(base_ + address, bytes.data(), bytes.size());
  scratch_top_ = (address + static_cast<uint32_t>(bytes.size()) + 15) & ~15u;
  return address;
}

uint32_t GuestEnv::Call(GuestFunc fn, uint32_t r3, uint32_t r4, uint32_t r5) {
  PPCContext ctx{};
  ctx.fpscr.loadFromHost();
  ctx.r1.u64 = kStackTop - 0x100;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  fn(ctx, base_);
  return ctx.r3.u32;
}

}  // namespace kuf::test
