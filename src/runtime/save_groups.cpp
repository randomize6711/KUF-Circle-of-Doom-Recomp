// One logical save = one commit (KNOWN_ISSUES K22, docs/SAVES.md §3).
//
// sub_826EFA38 is a message handler of the game's save/storage manager (v1): r3 = the manager, r4 = the
// message, whose id is at +8. Message 0x41127 is the game's save (every autosave and the Quit save): it
// writes the character file and calls XamContentFlush (sub_826B0F68), checks that the storage device is
// still there, then writes GlobalSetting, the counters shared by all characters, and flushes again
// (sub_826B0D68). On the console each flush is durable on its own, so an interruption between the two
// leaves a new character file beside the previous GlobalSetting. Here the whole message is one flush
// group: both files are committed together when the handler returns, and an interruption before that
// keeps the previous complete save. The files written and their content are the game's own; only the
// commit point moves to the end of the save.

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#include "runtime/save_groups.h"

REXCVAR_DEFINE_BOOL(kuf_atomic_saves, true, "KUF/Saves",
                    "Commit the game's save message (character file + GlobalSetting) as one transaction; "
                    "false: commit at each of the game's flushes, as the console does");

REX_EXTERN(__imp__sub_826EFA38);

namespace {

constexpr uint32_t kSaveMessage = 0x41127;
constexpr uint32_t kMessageIdOffset = 8;

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

class FlushGroup {
 public:
  explicit FlushGroup(rex::system::xam::ContentManager* manager) : manager_(manager) {
    manager_->BeginFlushGroup("save message 0x41127");
  }
  ~FlushGroup() { manager_->EndFlushGroup(); }
  FlushGroup(const FlushGroup&) = delete;
  FlushGroup& operator=(const FlushGroup&) = delete;

 private:
  rex::system::xam::ContentManager* manager_;
};

}  // namespace

namespace kuf::saves {
// A save message is being handled (M7 UX fix 6: Quit Game waits for it).
std::atomic<int> g_saving{0};
bool SaveInProgress() { return g_saving.load() > 0; }
}  // namespace kuf::saves

REX_HOOK_RAW(sub_826EFA38) {
  const uint32_t message = static_cast<uint32_t>(ctx.r4.u64);
  auto* kernel = REX_KERNEL_STATE();
  const bool save = message != 0 && LoadGuest32(base, message + kMessageIdOffset) == kSaveMessage;
  struct Saving {  // counted for the whole handler, either path
    bool on;
    explicit Saving(bool s) : on(s) { if (on) kuf::saves::g_saving.fetch_add(1); }
    ~Saving() { if (on) kuf::saves::g_saving.fetch_sub(1); }
  } saving(save);
  if (!save || !kernel || !kernel->content_manager() || !REXCVAR_GET(kuf_atomic_saves)) {
    __imp__sub_826EFA38(ctx, base);
    return;
  }
  FlushGroup group(kernel->content_manager());
  __imp__sub_826EFA38(ctx, base);
}
