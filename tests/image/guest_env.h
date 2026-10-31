// Minimal guest environment for calling individual recompiled functions in tests.
//
// Maps the decrypted v1 image at its load address and gives each test a scratch heap and a stack, using the
// same rex::memory::Memory class as the real runtime. Only pure functions (no kernel imports, no indirect
// calls through the runtime's function table) may be called this way.
#pragma once

#include <cstdint>
#include <span>

#include <rex/ppc.h>

namespace rex::memory {
class Memory;
}

namespace kuf::test {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

class GuestEnv {
 public:
  static GuestEnv& Get();

  uint8_t* base() const { return base_; }
  rex::memory::Memory* memory() const { return memory_; }

  // Copies bytes into the scratch region and returns their guest address. Scratch is reset by Reset().
  uint32_t Put(std::span<const uint8_t> bytes);
  void Reset() { scratch_top_ = kScratchBase; }

  // The runtime installs its SIGILL/SIGSEGV handlers once per process, when guest memory is created. Catch2
  // replaces them with its own for the duration of each later test case, so a forked child that relies on
  // the runtime's fault handling (crash-report tests) restores them first.
  static void RestoreRuntimeSignalHandlers();

  // Calls a recompiled function with r3..r5 set; returns r3.
  uint32_t Call(GuestFunc fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0);

  static constexpr uint32_t kImageBase = 0x82000000;
  static constexpr uint32_t kImageSize = 0x00B80000;
  static constexpr uint32_t kScratchBase = 0x40000000;
  static constexpr uint32_t kScratchSize = 0x01000000;
  static constexpr uint32_t kStackTop = 0x41F00000;

 private:
  GuestEnv();
  uint8_t* base_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  uint32_t scratch_top_ = kScratchBase;
};

}  // namespace kuf::test
