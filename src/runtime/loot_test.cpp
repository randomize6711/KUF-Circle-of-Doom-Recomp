// M6D K24 (docs/KNOWN_ISSUES.md K24, docs/TESTING.md): a controlled test fixture for the game's loot drops.
//
// sub_8256C280 is the game's drop decision for a unit that has died (on the game thread, once per death): it draws
// four chance rolls from the game thread's CRT rand() (sub_826D47A8), then for a gold drop the amount (sub_826D4818)
// and the pile, or for an item a fifth roll, the pick from the unit's item table (sub_826D4C18) and the item. Which of
// those a fight's deaths get depends on the seed the death happens to meet, so a scripted fight may drop nothing.
//
// --kuf_test_drop_seed (tests only, off by default) sets the game thread's seed when each decision starts: the
// decision runs unchanged (the same rolls, chances and tables), from a known state, so every death of a unit of the
// same kind decides the same way. --kuf_debug_skip_drop_decisions (fault test) skips the decisions: no loot at all.
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "runtime/frame_pacing.h"

REXCVAR_DEFINE_INT32(kuf_test_drop_seed, -1, "KUF/Debug",
                     "M6D K24 test fixture: the game thread's rand() seed set to this value (0 to 2147483647) when each "
                     "dead unit's drop decision starts, so the game's own drop rules decide every death the same way; "
                     "-1: off (the game's own sequence)");
REXCVAR_DEFINE_BOOL(kuf_debug_skip_drop_decisions, false, "KUF/Debug",
                    "M6D K24 fault test: the dead units' drop decisions are skipped (no gold, no items): the loot "
                    "tests must fail");

REX_EXTERN(__imp__sub_827FE1A8);  // the CRT's _getptd(): the calling thread's data, its rand() seed at + 20
REX_EXTERN(__imp__sub_8256C280);  // a dead unit's drop decision: (unit, ...) -> 1

namespace {

constexpr uint32_t kRandMul = 214013, kRandAdd = 2531011;  // the CRT's rand() step (sub_827FACD0)

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

bool Plausible(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000; }

// The calling thread's rand() seed address (0: none).
uint32_t SeedAddress(PPCContext& ctx, uint8_t* base) {
  const uint64_t r3 = ctx.r3.u64, lr = ctx.lr;
  __imp__sub_827FE1A8(ctx, base);
  const uint32_t ptd = ctx.r3.u32;
  ctx.r3.u64 = r3;
  ctx.lr = lr;
  return Plausible(ptd) ? ptd + 20 : 0;
}

// How many rand() steps lead from `from` to `to` (-1: more than 64).
int Steps(uint32_t from, uint32_t to) {
  for (int n = 0; n <= 64; ++n, from = from * kRandMul + kRandAdd) {
    if (from == to) return n;
  }
  return -1;
}

}  // namespace

REX_HOOK_RAW(sub_8256C280) {
  const uint32_t unit = ctx.r3.u32;
  const uint32_t unit_class = Plausible(unit) ? Load32(base, unit) : 0;
  if (REXCVAR_GET(kuf_debug_skip_drop_decisions)) {
    REXLOG_INFO("[kuf-loot] tick {}: drop decision for unit {:08X} (class {:08X}) skipped (fault test)",
                kuf::pacing::TicksCompleted(), unit, unit_class);
    ctx.r3.u64 = 1;
    return;
  }
  const int32_t test_seed = REXCVAR_GET(kuf_test_drop_seed);
  const uint32_t at = test_seed >= 0 ? SeedAddress(ctx, base) : 0;
  if (!at) {
    __imp__sub_8256C280(ctx, base);
    return;
  }
  const uint32_t met = Load32(base, at);
  Store32(base, at, uint32_t(test_seed));
  __imp__sub_8256C280(ctx, base);
  REXLOG_INFO("[kuf-loot] tick {}: drop decision for unit {:08X} (class {:08X}) from the test seed {:08X} (it met "
              "{:08X}): {} rand() draws",
              kuf::pacing::TicksCompleted(), unit, unit_class, uint32_t(test_seed), met,
              Steps(uint32_t(test_seed), Load32(base, at)));
}
