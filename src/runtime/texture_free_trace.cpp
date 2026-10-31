// Diagnostics for KNOWN_ISSUES K18 (an invalid physical free from the game's D3D resource release).
//
// sub_8215B568 releases a D3D resource (r3); its type is header & 0xF. For a texture (type 3, the case at
// 0x8215B634) it frees two physical blocks with MmFreePhysicalMemory: the base at +0x20 and the mips at
// +0x30 (page-aligned; a zero mip address is skipped). K18 is the second free failing ("not a region
// start", page state free). With --kuf_trace_texture_free=true every texture release is logged before the
// game's own frees run: the base, and for one with a mip address both blocks and the guest heap
// allocations they lie in, i.e. whether the mips lie inside the base allocation (so the first free already
// released them) or in a block of their own. The release itself is unchanged.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(kuf_trace_texture_free, false, "KUF/Debug",
                    "K18: log a D3D resource release whose physical free will fail, with the previous release of "
                    "that block");
REXCVAR_DEFINE_BOOL(kuf_trace_texture_free_all, false, "KUF/Debug",
                    "K18: also log every D3D resource release (changes timing; K18 then rarely reproduces)");

REX_EXTERN(__imp__sub_8215B568);

namespace {

constexpr uint32_t kBaseOffset = 0x20;
constexpr uint32_t kMipOffset = 0x30;
constexpr uint32_t kPageMask = ~0xFFFu;

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

struct Region {
  bool known = false;
  uint32_t start = 0;
  uint32_t size = 0;
  uint32_t state = 0;
};

Region QueryRegion(rex::memory::Memory* memory, uint32_t address) {
  Region r;
  auto* heap = memory->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  if (heap && heap->QueryRegionInfo(address, &info)) {
    r.known = true;
    r.start = info.allocation_base;
    r.size = info.allocation_size;
    r.state = info.state;
  }
  return r;
}

// The physical block(s) each resource type's case frees (cases decoded from the jump table at 0x820FED90):
// types 1, 2, 7, 8 free +0x18 (type 1 with the low two bits cleared), 6 frees +0x20, 3 (texture) frees +0x20
// and +0x30 page-aligned; the others free no physical block. All of them free through the call at
// 0x8215B674, so a failed free with lr 8215B678 can come from any of these types.
struct LastRelease {
  uint32_t resource = 0;
  uint32_t type = 0;
  uint32_t lr = 0;
  uint32_t serial = 0;
};
std::mutex g_mutex;
std::unordered_map<uint32_t, LastRelease> g_last;  // physical block -> its last release
uint32_t g_serial = 0;

int FreedBlocks(const uint8_t* base, uint32_t resource, uint32_t type, uint32_t out[2]) {
  switch (type) {
    case 1: out[0] = LoadGuest32(base, resource + 0x18) & ~3u; return 1;
    case 2: case 7: case 8: out[0] = LoadGuest32(base, resource + 0x18); return 1;
    case 6: out[0] = LoadGuest32(base, resource + 0x20); return 1;
    case 3:
      out[0] = LoadGuest32(base, resource + kBaseOffset) & kPageMask;
      out[1] = LoadGuest32(base, resource + kMipOffset) & kPageMask;
      return 2;
    default: return 0;
  }
}

}  // namespace

REX_HOOK_RAW(sub_8215B568) {
  const uint32_t resource = static_cast<uint32_t>(ctx.r3.u64);
  auto* kernel = REX_KERNEL_STATE();
  if (REXCVAR_GET(kuf_trace_texture_free) && resource && kernel && kernel->memory()) {
    const uint32_t type = LoadGuest32(base, resource) & 0xF;
    uint32_t blocks[2] = {0, 0};
    const int n = FreedBlocks(base, resource, type, blocks);
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    for (int k = 0; k < n; ++k) {
      if (!blocks[k]) continue;  // a zero address is skipped by the game's free
      const Region r = QueryRegion(kernel->memory(), blocks[k]);
      const bool start = r.known && r.start == blocks[k] && r.state != 0;
      LastRelease prev;
      uint32_t serial;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        serial = ++g_serial;
        auto it = g_last.find(blocks[k]);
        if (it != g_last.end()) prev = it->second;
        g_last[blocks[k]] = {resource, type, lr, serial};
      }
      if (!start || REXCVAR_GET(kuf_trace_texture_free_all)) {
        REXLOG_INFO("[k18] release #{} resource {:08X} type {} lr {:08X}: frees {:08X} (allocation {:08X} + {:X}, "
                    "state {:X}){}{}; this block's previous release: #{} resource {:08X} type {} lr {:08X}",
                    serial, resource, type, lr, blocks[k], r.start, r.size, r.state,
                    type == 3 ? (k ? " [mips]" : " [base]") : "",
                    start ? "" : " NOT AN ALLOCATION START: this free will fail", prev.serial, prev.resource,
                    prev.type, prev.lr);
      }
    }
  }
  __imp__sub_8215B568(ctx, base);
}
