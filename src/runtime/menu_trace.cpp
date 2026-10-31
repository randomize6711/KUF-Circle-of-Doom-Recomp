// Diagnostics for the original menus (M6A menu architecture investigation), off unless --kuf_trace_menu=true.
//
// Logs calls of the title menu's SETTINGS functions and the in-game Settings scene's (found from the code
// that looks their controls up by ID, docs/MENU_ARCHITECTURE.md): arguments, the object's selected row
// (+80) and the return value. While one of them runs, calls of the helpers it uses (sound, text, control
// state) are logged too, at most 200 per helper. Every hook calls the original; nothing is changed.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "runtime/pc_settings_menu.h"
#include "runtime/prompt_text.h"
#include "runtime/title_menu.h"

REXCVAR_DEFINE_BOOL(kuf_trace_menu, false, "KUF/Debug",
                    "Log the original Settings menus' functions and the helpers they call (M6A investigation)");
REXCVAR_DEFINE_BOOL(kuf_trace_ui_objects, false, "KUF/Debug",
                    "Log the UI controls the layout loader creates and, after each in-game Settings event, the "
                    "words that changed in a few of them (M6A investigation)");

namespace {

thread_local int t_depth = 0;  // > 0 while a traced menu function runs on this thread

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

uint32_t SelectedRow(const uint8_t* base, uint32_t object) {
  return object >= 0x10000 && object < 0xF0000000 ? Load32(base, object + 80) : 0xFFFFFFFF;
}

// Controls watched by --kuf_trace_ui_objects (in-game Settings: the highlight button, the first row's button,
// the SFX slider tip, the skin name text, and the Vibration row's button and its On / Off buttons).
constexpr std::array<uint32_t, 7> kWatchedIds = {0x2701005, 0x2701001, 0x2705024, 0x2704002,
                                                 0x270100D, 0x270100E, 0x270100F};
constexpr uint32_t kObjectWords = 64;  // first 256 bytes of each object
std::mutex g_objects_mutex;
std::array<uint32_t, kWatchedIds.size()> g_objects{};
std::array<std::array<uint32_t, kObjectWords>, kWatchedIds.size()> g_last{};

// The control lookup returned `object` for `id`: remember it if it is one of the watched controls, and log the
// first time (or when it changes).
void OnControlLookup(uint32_t id, uint32_t object) {
  if (object < 0x10000 || object >= 0xF0000000) {
    return;
  }
  for (uint32_t k = 0; k < kWatchedIds.size(); ++k) {
    if (id == kWatchedIds[k]) {
      std::lock_guard lock(g_objects_mutex);
      if (g_objects[k] != object) {
        g_objects[k] = object;
        g_last[k] = {};
        REXLOG_INFO("[kuf-ui] control {:08X} is object {:08X}", id, object);
      }
    }
  }
}

void DumpWatchedObjects(const uint8_t* base) {
  std::lock_guard lock(g_objects_mutex);
  for (uint32_t k = 0; k < kWatchedIds.size(); ++k) {
    if (!g_objects[k]) {
      continue;
    }
    std::string changes;
    for (uint32_t w = 0; w < kObjectWords; ++w) {
      const uint32_t v = Load32(base, g_objects[k] + 4 * w);
      if (v != g_last[k][w]) {
        changes += fmt::format(" +{}={:08X}", 4 * w, v);
        g_last[k][w] = v;
      }
    }
    if (!changes.empty()) {
      REXLOG_INFO("[kuf-ui] {:08X}{}", kWatchedIds[k], changes);
    }
  }
}

}  // namespace

// A menu function: logged on entry and exit.
#define KUF_TRACE_MENU_FN(addr, label)                                                                   \
  REX_EXTERN(__imp__sub_##addr);                                                                         \
  REX_HOOK_RAW(sub_##addr) {                                                                             \
    if (!REXCVAR_GET(kuf_trace_menu)) {                                                                  \
      __imp__sub_##addr(ctx, base);                                                                      \
      return;                                                                                            \
    }                                                                                                    \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;                   \
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);                                                   \
    REXLOG_INFO("[kuf-menu] > " label " sub_" #addr "(r3={:08X} r4={:08X} r5={:08X} r6={:08X}) lr {:08X} " \
                "row {:X}",                                                                              \
                r3, r4, r5, r6, lr, SelectedRow(base, r3));                                              \
    ++t_depth;                                                                                           \
    __imp__sub_##addr(ctx, base);                                                                        \
    --t_depth;                                                                                           \
    REXLOG_INFO("[kuf-menu] < " label " sub_" #addr " = {:08X} row {:X}", ctx.r3.u32, SelectedRow(base, r3)); \
  }

// A menu function that PC Settings intercepts first (`before` returns true when it handled the call).
#define KUF_TRACE_MENU_FN_AFTER(addr, label, before)                                                     \
  REX_EXTERN(__imp__sub_##addr);                                                                         \
  REX_HOOK_RAW(sub_##addr) {                                                                             \
    if (before(ctx, base)) {                                                                             \
      return;                                                                                            \
    }                                                                                                    \
    if (!REXCVAR_GET(kuf_trace_menu)) {                                                                  \
      __imp__sub_##addr(ctx, base);                                                                      \
      return;                                                                                            \
    }                                                                                                    \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;                   \
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);                                                   \
    REXLOG_INFO("[kuf-menu] > " label " sub_" #addr "(r3={:08X} r4={:08X} r5={:08X} r6={:08X}) lr {:08X} " \
                "row {:X}",                                                                              \
                r3, r4, r5, r6, lr, SelectedRow(base, r3));                                              \
    ++t_depth;                                                                                           \
    __imp__sub_##addr(ctx, base);                                                                        \
    --t_depth;                                                                                           \
    REXLOG_INFO("[kuf-menu] < " label " sub_" #addr " = {:08X} row {:X}", ctx.r3.u32, SelectedRow(base, r3)); \
  }

// A helper: logged only inside a traced menu function, at most 200 times.
#define KUF_TRACE_MENU_HELPER(addr)                                                                      \
  REX_EXTERN(__imp__sub_##addr);                                                                         \
  REX_HOOK_RAW(sub_##addr) {                                                                             \
    static std::atomic<int> count{0};                                                                    \
    if (t_depth > 0 && REXCVAR_GET(kuf_trace_menu) && count.fetch_add(1) < 200) {                        \
      REXLOG_INFO("[kuf-menu]     helper sub_" #addr "(r3={:08X} r4={:08X} r5={:08X} r6={:08X} f1={}) lr {:08X}", \
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.f1.f64, static_cast<uint32_t>(ctx.lr)); \
    }                                                                                                    \
    __imp__sub_##addr(ctx, base);                                                                        \
  }

// Title menu (vtable 0x8203E658): scene registration, and its SETTINGS scene (SN_TITLE_OPTION).
KUF_TRACE_MENU_FN(8267A358, "title:register-scenes")
KUF_TRACE_MENU_FN(82679F00, "title-option:open?")
KUF_TRACE_MENU_FN(826785B8, "title-option:bind-controls")
KUF_TRACE_MENU_FN(82677158, "title-option:show-values?")
KUF_TRACE_MENU_FN(82678F30, "title-option:?->show-values")
KUF_TRACE_MENU_FN(826799A0, "title-option:?->rows")
KUF_TRACE_MENU_FN(82678900, "title-option:rows?")
KUF_TRACE_MENU_FN_AFTER(8267A5E8, "title:event", kuf::title_menu::BeforeMenuEvent)
KUF_TRACE_MENU_FN_AFTER(826791D0, "title:input", kuf::title_menu::BeforeInput)
KUF_TRACE_MENU_FN(82676AB8, "option:slider?")
KUF_TRACE_MENU_FN(82676C38, "option:toggle-a?")
KUF_TRACE_MENU_FN(82676B98, "option:toggle-b?")
// In-game options menu: its Settings scene (SN_PROFILE_OPTION_CONFIG).
KUF_TRACE_MENU_FN(82664AC0, "profile-option:?->open")
KUF_TRACE_MENU_FN(82663E40, "profile-option:open?")
KUF_TRACE_MENU_FN(82662E98, "profile-option:bind-controls")
// The functions that post the options-save message 0x41128 (found from their call sites). 82663FA0 is the
// in-game options menu's event handler (input for the options list and its Settings scene; traced).
REX_EXTERN(__imp__sub_82663FA0);
REX_HOOK_RAW(sub_82663FA0) {  // the in-game options menu's event handler: traced, then the watched controls
  // Also the entry point of PC Settings (src/runtime/pc_settings_menu.cpp).
  const uint32_t menu = ctx.r3.u32;
  if (kuf::pc_menu::BeforeSettingsEvent(ctx, base)) {
    return;
  }
  if (!REXCVAR_GET(kuf_trace_menu) && !REXCVAR_GET(kuf_trace_ui_objects)) {
    __imp__sub_82663FA0(ctx, base);
    return;
  }
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  REXLOG_INFO("[kuf-menu] > profile-option:event sub_82663FA0(r3={:08X} r4={:08X} r5={:08X} r6={:08X}) lr {:08X}",
              r3, r4, r5, r6, static_cast<uint32_t>(ctx.lr));
  if (REXCVAR_GET(kuf_trace_ui_objects) && r5 >= 0x10000 && r5 < 0xF0000000) {
    REXLOG_INFO("[kuf-ui] event at {:08X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", r5,
                Load32(base, r5), Load32(base, r5 + 4), Load32(base, r5 + 8), Load32(base, r5 + 12),
                Load32(base, r5 + 16), Load32(base, r5 + 20), Load32(base, r5 + 24), Load32(base, r5 + 28));
  }
  ++t_depth;
  __imp__sub_82663FA0(ctx, base);
  --t_depth;
  REXLOG_INFO("[kuf-menu] < profile-option:event sub_82663FA0 = {:08X}", ctx.r3.u32);
  if (REXCVAR_GET(kuf_trace_ui_objects)) {
    DumpWatchedObjects(base);
  }
}
KUF_TRACE_MENU_FN(82675F48, "option:posts-save?")
// The title SETTINGS input handler (menu, event); also the entry point of PC Settings in the title.
KUF_TRACE_MENU_FN_AFTER(82678280, "title-option:input", kuf::pc_menu::BeforeTitleSettingsEvent)
// Helpers called from the title SETTINGS input function.
KUF_TRACE_MENU_HELPER(82610168)
// The game's message post (message, data, ...): traced as a helper; also seen by the title menu (M7 UX fix 5: the
// storage prompt answered for the player, fix 6: the Quit Game question).
REX_EXTERN(__imp__sub_825D5560);
REX_HOOK_RAW(sub_825D5560) {
  if (kuf::title_menu::BeforePost(ctx, base)) {
    ctx.r3.u64 = 0;  // not posted (the storage notice)
    return;
  }
  static std::atomic<int> count{0};
  if (t_depth > 0 && REXCVAR_GET(kuf_trace_menu) && count.fetch_add(1) < 200) {
    REXLOG_INFO("[kuf-menu]     helper sub_825D5560(r3={:08X} r4={:08X} r5={:08X} r6={:08X}) lr {:08X}", ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_825D5560(ctx, base);
}
KUF_TRACE_MENU_HELPER(826AFA40)
// The game's other message post (the same arguments; the title's storage notice goes through it).
REX_EXTERN(__imp__sub_825D5A88);
REX_HOOK_RAW(sub_825D5A88) {
  if (kuf::title_menu::BeforePost(ctx, base)) {
    ctx.r3.u64 = 0;  // not posted (the storage notice)
    return;
  }
  static std::atomic<int> count{0};
  if (t_depth > 0 && REXCVAR_GET(kuf_trace_menu) && count.fetch_add(1) < 200) {
    REXLOG_INFO("[kuf-menu]     helper sub_825D5A88(r3={:08X} r4={:08X} r5={:08X} r6={:08X}) lr {:08X}", ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_825D5A88(ctx, base);
}
KUF_TRACE_MENU_HELPER(82657C00)
KUF_TRACE_MENU_HELPER(8261B488)

// Layout loader: the readers of one control record each (by record type; docs/MENU_ARCHITECTURE.md). They return
// definition objects; the live controls are created from them when a scene is instantiated.
#define KUF_TRACE_UI_READER(addr, kind)                                                                  \
  REX_EXTERN(__imp__sub_##addr);                                                                         \
  REX_HOOK_RAW(sub_##addr) {                                                                             \
    static std::atomic<int> count{0};                                                                    \
    __imp__sub_##addr(ctx, base);                                                                        \
    if (REXCVAR_GET(kuf_trace_ui_objects) && count.fetch_add(1) < 20) {                                 \
      REXLOG_INFO("[kuf-ui] read " kind " {:08X} id {:08X}", ctx.r3.u32,                                 \
                  ctx.r3.u32 >= 0x10000 && ctx.r3.u32 < 0xF0000000 ? Load32(base, ctx.r3.u32) : 0);      \
    }                                                                                                    \
  }
KUF_TRACE_UI_READER(8261FD58, "button")
KUF_TRACE_UI_READER(8261FE68, "list box")
KUF_TRACE_UI_READER(82620308, "text")
KUF_TRACE_UI_READER(82620450, "image")

// The control lookup (node, id) -> control, used by every menu to bind its controls.
REX_EXTERN(__imp__sub_82624570);
REX_HOOK_RAW(sub_82624570) {
  const uint32_t id = ctx.r4.u32;
  __imp__sub_82624570(ctx, base);
  if (REXCVAR_GET(kuf_trace_ui_objects)) {
    OnControlLookup(id, ctx.r3.u32);
  }
}

// The text lookup (text manager, text number, format arguments...) -> string: a helper of the menu functions
// (as above), and the first results for the Vibration label (1882), to see the string format. Also supplies the
// label of the PC Settings row, and the keyboard's prompts (M7).
REX_EXTERN(__imp__sub_826CDBF0);
REX_HOOK_RAW(sub_826CDBF0) {
  const uint32_t number = ctx.r4.u32;
  if (const uint32_t text = kuf::pc_menu::TextOverride(number)) {  // the PC Settings row's label
    ctx.r3.u64 = text;
    return;
  }
  static std::atomic<int> helper_count{0};
  if (t_depth > 0 && REXCVAR_GET(kuf_trace_menu) && helper_count.fetch_add(1) < 200) {
    REXLOG_INFO("[kuf-menu]     helper sub_826CDBF0(r3={:08X} r4={:08X} r5={:08X} r6={:08X} f1={}) lr {:08X}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.f1.f64, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_826CDBF0(ctx, base);
  static std::atomic<int> count{0};
  if (REXCVAR_GET(kuf_trace_ui_objects) && number == 1882 && count.fetch_add(1) < 3 && ctx.r3.u32 >= 0x10000 &&
      ctx.r3.u32 < 0xF0000000) {
    std::string bytes;
    for (uint32_t i = 0; i < 48; ++i) {
      bytes += fmt::format("{:02X}", base[ctx.r3.u32 + i]);
    }
    REXLOG_INFO("[kuf-ui] text 1882 -> {:08X}: {}", ctx.r3.u32, bytes);
  }
  // M7 feature 6: with the keyboard in use, its keys in place of the controller icons (prompt_text.cpp).
  const uint32_t result = ctx.r3.u32;
  if (const uint32_t keyboard = kuf::prompts::OnTextLookup(ctx, base, result)) {
    ctx.r3.u64 = keyboard;
  } else {
    ctx.r3.u64 = result;
  }
}
