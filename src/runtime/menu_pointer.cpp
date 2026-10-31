// Mouse pointer in the original menus (M7 feature 2, docs/INPUT.md "Mouse", docs/MENU_ARCHITECTURE.md 13).
//
// The UI toolkit gives input to one control, the focused one: the global at 0x82ABBB90 (compared with each control by
// the shared vtable slot 7, sub_82624598). A control's handler (vtable slot 1, sub_826259C8 for buttons) forwards
// input to its scene through sub_826221D8(manager, control + 48, ...): +48 is the scene, whose +0 is the scene ID and
// +8 its root control node. A node keeps its children in a list at +32 (head node +36, count +40; a list node: +0
// next, +8 the child). A live control: +8 ID, +12 type (1 button, 2 list box, 4 text, 5 image), +16..+28 its
// rectangle in the 1280 x 720 layout space, +56 state (1 shown, 0x10 unavailable).
//
// Depending on the menu the focused control is the selected row itself (the title list) or a highlight button drawn
// over it (Settings, the character slots); either way its rectangle is the selected row's. The plan
// (menu_pointer_math.cpp) compares it with the row under the pointer. At each tick's start (OnTick) the steps towards
// it are made at once by calling the focused control's input handler (vtable slot 1: sub_826259C8 for buttons,
// sub_82627DD0 for list boxes) with an input event {+4 0x44C, +12 code}, as the game's input update does for a D-pad
// press: the menus keep their own navigation, cursor sound and bounds. (M7 feature 2 pressed a virtual D-pad instead:
// a press and a release poll per row at 30 polls per second, 67 ms per row; UX fix 1.) A step that does not move the
// selection falls back to one virtual press; a click confirms with a virtual A once the row is selected.
//
// Every read is checked against the guest heap's page access first (a stale pointer never faults).
// --kuf_trace_menu_pointer logs, for each input, the focused scene and its buttons.

#include "runtime/menu_pointer.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

#include "input/kuf_input_driver.h"
#include "runtime/menu_mouse.h"
#include "runtime/pc_settings_menu.h"
#include "runtime/prompt_text.h"
#include "runtime/title_menu.h"

REXCVAR_DECLARE(bool, kuf_title_arrow);
REXCVAR_DEFINE_BOOL(kuf_selection_arrow_auto, true, "KUF/Settings",
                    "M7 menu overhaul: the Xbox selection arrow only with a controller (keyboard and mouse: the "
                    "highlight alone)");
REXCVAR_DEFINE_BOOL(kuf_trace_scene_dump, false, "KUF/Debug",
                    "M7: when the focused scene changes, log all its controls (type, ID, rectangle, state, text)");
REXCVAR_DEFINE_BOOL(kuf_trace_menu_pointer, false, "KUF/Debug",
                    "M7: log, for each menu input, the focused scene and its buttons (rectangles, states)");

REX_EXTERN(__imp__sub_826221D8);  // the toolkit's input dispatch to a scene: (manager, scene, w0, message, w2, code)
REX_EXTERN(sub_8218F410);         // the game's allocation

namespace {

constexpr uint32_t kFocusedControl = 0x82ABBB90;
constexpr uint32_t kMessageInput = 0x44C;
constexpr uint32_t kTypeButton = 1;
constexpr uint32_t kTypeListBox = 2;
constexpr size_t kMaxButtons = 400;

rex::memory::Memory* g_memory = nullptr;

bool Readable(uint32_t address, uint32_t size) {
  if (!g_memory || address < 0x10000 || address >= 0xF0000000 || (address & 3)) return false;
  auto* heap = g_memory->LookupHeap(address);
  if (!heap) return false;
  const auto access = heap->QueryRangeAccess(address, address + size - 1);
  return (uint32_t(access) & uint32_t(rex::memory::PageAccess::kReadOnly)) != 0;
}

uint32_t Load32(uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, g_memory->virtual_membase() + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

bool ReadControl(uint32_t c, kuf::menu_pointer::UiButton* out, uint32_t* type) {
  if (!Readable(c, 64)) return false;
  *type = Load32(c + 12);
  out->id = Load32(c + 8);
  out->x = int32_t(Load32(c + 16));
  out->y = int32_t(Load32(c + 20));
  out->w = int32_t(Load32(c + 24));
  out->h = int32_t(Load32(c + 28));
  out->state = Load32(c + 56);
  return true;
}

void CollectButtons(uint32_t node, int depth, std::vector<kuf::menu_pointer::UiButton>* out) {
  if (depth > 6 || out->size() >= kMaxButtons || !Readable(node, 44)) return;
  const uint32_t head = Load32(node + 36), n = Load32(node + 40);
  if (!Readable(head, 12) || n > 1024) return;
  uint32_t it = Load32(head);
  for (uint32_t i = 0; i < n && it != head && Readable(it, 12) && out->size() < kMaxButtons; ++i, it = Load32(it)) {
    const uint32_t c = Load32(it + 8);
    kuf::menu_pointer::UiButton b;
    uint32_t type = 0;
    if (!ReadControl(c, &b, &type)) continue;
    if (type == kTypeButton) out->push_back(b);
    CollectButtons(c, depth + 1, out);
  }
}

// The focused control and its scene's root node; false without a readable focused control.
bool Focus(kuf::menu_pointer::UiButton* focus, uint32_t* type, uint32_t* root, uint32_t* scene_id) {
  if (!Readable(kFocusedControl, 4)) return false;
  const uint32_t c = Load32(kFocusedControl);
  if (!ReadControl(c, focus, type)) return false;
  const uint32_t scene = Load32(c + 48);
  if (!Readable(scene, 12)) return false;
  *scene_id = Load32(scene);
  *root = Load32(scene + 8);
  return true;
}

// --kuf_trace_scene_dump: every control under a node (all types), with its text when it has one.
void DumpControls(uint32_t node, int depth, std::string* out, int* count) {
  if (depth > 6 || *count > 400 || !Readable(node, 44)) return;
  const uint32_t head = Load32(node + 36), n = Load32(node + 40);
  if (!Readable(head, 12) || n > 1024) return;
  uint32_t it = Load32(head);
  for (uint32_t i = 0; i < n && it != head && Readable(it, 12) && *count < 400; ++i, it = Load32(it)) {
    const uint32_t c = Load32(it + 8);
    kuf::menu_pointer::UiButton b;
    uint32_t type = 0;
    if (!ReadControl(c, &b, &type)) continue;
    ++*count;
    std::string text;
    if (Readable(c + 84, 4)) {
      const uint32_t str = Load32(c + 84);
      if (Readable(str & ~3u, 64)) {
        const char* p = reinterpret_cast<const char*>(g_memory->virtual_membase() + str);
        text = std::string(p, strnlen(p, 120));
      }
    }
    *out += fmt::format("\n    {}{:08X} type {} ({},{} {}x{}) state {:X} vt {:08X} text#{} '{}'", std::string(depth * 2, ' '),
                        b.id, type, b.x, b.y, b.w, b.h, b.state, Load32(c), int32_t(Load32(c + 80)), text);
    DumpControls(c, depth + 1, out, count);
  }
}
uint32_t g_dumped_scene = 0;

// The selection arrow (M7 UI follow-up): the UI manager's update (sub_82612DE0, once per update of its scenes) shows
// the arrow beside the focused button or list, unless the control's state has bit 0x80 (the game's own title rows
// and message box button carry it), and hides it otherwise. The decision for the device in use is made in that call
// (the hook below): with the keyboard and mouse the focused control counts as having the bit, with a controller the
// title list's rows count as not having it; the state is changed for the call only and put back. (Before: the bit was
// set at each tick's start on the focused button, so a row the focus reached later in the tick showed the arrow
// until the next tick, and the title rows kept a cleared bit from the last controller use.)
constexpr uint32_t kNoArrow = 0x80;
constexpr uint32_t kTitleListScene = 0x03900000;

constexpr uint32_t kCodeSelect = 0, kCodeUp = 6, kCodeDown = 7, kCodeLeft = 8, kCodeRight = 9;
constexpr int kMaxSteps = 32;  // more rows than any menu has
constexpr uint32_t kMessageBoxScene = 0x02500000;  // the game's message box (one button: A answers, B the other)
uint32_t g_event = 0;          // the guest input event handed to the handlers (allocated once)
// Fallback stepping (virtual presses) continues at the next ticks without a new movement: up to kMaxSteps ticks.
int g_goal_ticks = 0;
bool g_confirm = false;    // a click waits for the fallback steps to reach its row
uint32_t g_goal_pos = 0;   // the selection's position before the last virtual press (no change: give up)

// Rows whose value a click changes (Right, wrapping) instead of confirming: the game type screen's Difficulty.
constexpr uint32_t kValueRows[] = {0x02F01001, 0x02F01005};

std::string FocusText() {
  if (!Readable(kFocusedControl, 4)) return {};
  const uint32_t c = Load32(kFocusedControl);
  if (!Readable(c + 84, 4)) return {};
  const uint32_t str = Load32(c + 84);
  if (!Readable(str & ~3u, 64)) return {};
  const char* p = reinterpret_cast<const char*>(g_memory->virtual_membase() + str);
  return std::string(p, strnlen(p, 128));
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

// The focused control's position (x << 16 | y), or 0.
uint32_t FocusPosition() {
  kuf::menu_pointer::UiButton focus;
  uint32_t type = 0, root = 0, scene_id = 0;
  if (!Focus(&focus, &type, &root, &scene_id)) return 0;
  if (type == kTypeListBox) return 0x80000000u | Load32(Load32(kFocusedControl) + 408);  // the list's index
  return uint32_t(focus.x) << 16 | uint32_t(focus.y & 0xFFFF);
}

}  // namespace

namespace kuf::menu_pointer {

bool Dispatch(PPCContext& ctx, uint8_t* base, uint32_t code) {
  if (!Readable(kFocusedControl, 4)) return false;
  const uint32_t control = Load32(kFocusedControl);
  if (!Readable(control, 64)) return false;
  const uint32_t vtable = Load32(control);
  if (!Readable(vtable, 8)) return false;
  const uint32_t handler = Load32(vtable + 4);
  PPCFunc* fn = rex::runtime::ResolveIndirectFunction(handler);
  if (!fn) return false;
  if (!g_event) {
    ctx.r3.u64 = 16;
    sub_8218F410(ctx, base);
    g_event = ctx.r3.u32;
    if (!g_event) return false;
  }
  Store32(base, g_event, 0);
  Store32(base, g_event + 4, kMessageInput);
  Store32(base, g_event + 8, 0);
  Store32(base, g_event + 12, code);
  ctx.r3.u64 = control;
  ctx.r4.u64 = g_event;
  fn(ctx, base);
  return true;
}

void Start(rex::memory::Memory* memory) { g_memory = memory; }

bool GuestReadable(uint32_t address, uint32_t size) { return Readable(address, size); }


void OnTick(PPCContext& ctx, uint8_t* base) {
  kuf::menu_mouse::OnTick(ctx, base);
  if (REXCVAR_GET(kuf_trace_scene_dump) && g_memory) {
    UiButton f;
    uint32_t type = 0, root = 0, scene_id = 0;
    const uint32_t key = Focus(&f, &type, &root, &scene_id)
                             ? scene_id ^ (uint32_t(f.y) << 1) ^ uint32_t(std::hash<std::string>{}(FocusText()))
                             : 0;
    if (key && key != g_dumped_scene) {
      g_dumped_scene = key;  // the scene, the focused row or its label changed
      std::string out;
      int count = 0;
      DumpControls(root, 0, &out, &count);
      std::string words;  // the focused control's first 64 words
      const uint32_t c = Load32(kFocusedControl);
      for (uint32_t i = 0; i < 64 && Readable(c + 4 * i, 4); ++i) words += fmt::format(" {:X}", Load32(c + 4 * i));
      REXLOG_INFO("[kuf-pointer] scene {:08X} (focus {:08X} type {} at {},{}): {} controls; focus words{}{}", scene_id,
                  f.id, type, f.x, f.y, count, words, out);
    }
  }
  auto* driver = kuf::input::KufInputDriver::Instance();
  if (!driver || !g_memory) return;
  const auto w = driver->TakePointerWork();
  if (!w.active) {
    kuf::pc_menu::PointerSlider(ctx, base, 0, 0, false, false);  // ends a drag
    kuf::menu_mouse::Pointer(ctx, base, 0, 0, false, false, false);  // and a held arrow
    return;
  }
  // PC Settings sliders: a click on a track sets the value, a drag follows the pointer (no hover, no A meanwhile).
  if (kuf::pc_menu::PointerSlider(ctx, base, w.at.x, w.at.y, w.click, w.held, w.moved)) return;
  // The level-up dialog's arrows and footer, the inventory's tabs (menu_mouse.cpp).
  if (kuf::menu_mouse::Pointer(ctx, base, w.at.x, w.at.y, w.moved, w.click, w.held)) return;
  if (w.moved || w.click) {
    g_goal_ticks = 0;
    g_confirm = false;
  }
  if (!w.moved && !w.click && g_goal_ticks == 0) return;
  if (g_goal_ticks > 0 && driver->PendingPulses()) return;  // the last virtual press is still being delivered
  if (g_goal_ticks > 0 && !w.moved && !w.click && FocusPosition() == g_goal_pos) {
    g_goal_ticks = 0;  // the virtual press did not move the selection either (a bound, or a refused row): stop
    g_confirm = false;
    return;
  }
  int direct = 0, pressed = 0;
  Step plan = PlanAt(w.at.x, w.at.y);
  for (int i = 0; i < kMaxSteps && plan.over_row && plan.available && !plan.on_selected && (plan.dx || plan.dy); ++i) {
    const uint32_t code = plan.dy < 0 ? kCodeUp : plan.dy > 0 ? kCodeDown : plan.dx < 0 ? kCodeLeft : kCodeRight;
    const uint32_t before = FocusPosition();
    if (!Dispatch(ctx, base, code) || FocusPosition() == before) {
      // The handler did not move it (a menu that reads its input elsewhere, or a bound): one virtual press instead,
      // and the rest at the next ticks' plans.
      using kuf::input::Control;
      g_goal_pos = before;
      driver->PulseControl(code == kCodeUp ? Control::kDpadUp : code == kCodeDown ? Control::kDpadDown
                           : code == kCodeLeft ? Control::kDpadLeft : Control::kDpadRight);
      ++pressed;
      break;
    }
    ++direct;
    plan = PlanAt(w.at.x, w.at.y);
  }
  const bool confirm = w.click || g_confirm;
  if (pressed && g_goal_ticks < kMaxSteps) {
    ++g_goal_ticks;
    g_confirm = confirm;  // after the virtual presses have arrived
  } else {
    g_goal_ticks = 0;
    g_confirm = false;
    uint32_t scene_id = 0;
    {
      UiButton f;
      uint32_t type = 0, root = 0;
      Focus(&f, &type, &root, &scene_id);
    }
    UiButton focus_now;
    {
      uint32_t type = 0, root = 0, id = 0;
      Focus(&focus_now, &type, &root, &id);
    }
    const bool value_row =
        std::find(std::begin(kValueRows), std::end(kValueRows), focus_now.id) != std::end(kValueRows);
    if (confirm && plan.over_row && plan.on_selected && value_row) {
      // A value row: the next value (the game's Right), from the last back to the first (its Left).
      const std::string was = FocusText();
      Dispatch(ctx, base, kCodeRight);
      if (FocusText() == was) {
        for (int i = 0; i < 16; ++i) {
          const std::string before_left = FocusText();
          Dispatch(ctx, base, kCodeLeft);
          if (FocusText() == before_left) break;
        }
      }
      REXLOG_INFO("[kuf-pointer] value row clicked: '{}' -> '{}'", was, FocusText());
    } else if (confirm && plan.over_row && plan.on_selected) {
      // The selected row: the game's own Select to it (the event its handler gets from A); A if there is no handler.
      if (!Dispatch(ctx, base, kCodeSelect)) driver->PulseControl(kuf::input::Control::kA);
    } else if (confirm && !plan.over_row && focus_now.id == 0 && scene_id != kMessageBoxScene) {
      driver->PulseControl(kuf::input::Control::kA);  // nothing has the focus (a screen without buttons): the plain A
    }  // a row that cannot be reached, or a click beside the rows of a menu or dialog: nothing (M7 UI follow-up: it
       // was an A, which confirmed the level-up dialog or a message box from a click outside it)
  }
  if (REXCVAR_GET(kuf_trace_menu_pointer) && (direct || pressed || confirm)) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w.moved_at).count();
    UiButton f;
    uint32_t type = 0, root = 0, scene = 0;
    Focus(&f, &type, &root, &scene);
    REXLOG_INFO("[kuf-pointer] at ({},{}): {} direct step(s), {} virtual press(es){}, {:.1f} ms after the last move; "
                "plan over {} available {} selected {} step {},{}; focus {:08X} ({},{} {}x{}) in {:08X}",
                w.at.x, w.at.y, direct, pressed, confirm ? ", click" : "", ms, plan.over_row, plan.available,
                plan.on_selected, plan.dx, plan.dy, f.id, f.x, f.y, f.w, f.h, scene);
  }
}

namespace {
uint32_t FindUnder(uint32_t node, uint32_t id, int depth) {
  if (depth > 6 || !Readable(node, 44)) return 0;
  const uint32_t head = Load32(node + 36), n = Load32(node + 40);
  if (!Readable(head, 12) || n > 1024) return 0;
  uint32_t it = Load32(head);
  for (uint32_t i = 0; i < n && it != head && Readable(it, 12); ++i, it = Load32(it)) {
    const uint32_t c = Load32(it + 8);
    if (!Readable(c, 64)) continue;
    if (Load32(c + 8) == id) return c;
    if (const uint32_t found = FindUnder(c, id, depth + 1)) return found;
  }
  return 0;
}
}  // namespace

uint32_t FocusedSceneId() {
  if (!g_memory) return 0;
  UiButton f;
  uint32_t type = 0, root = 0, scene_id = 0;
  return Focus(&f, &type, &root, &scene_id) ? scene_id : 0;
}

uint32_t FindInFocusedScene(uint32_t id) {
  if (!g_memory) return 0;
  UiButton f;
  uint32_t type = 0, root = 0, scene_id = 0;
  return Focus(&f, &type, &root, &scene_id) ? FindUnder(root, id, 0) : 0;
}

}  // namespace kuf::menu_pointer

REX_EXTERN(__imp__sub_82612DE0);
REX_HOOK_RAW(sub_82612DE0) {  // the UI manager's selection arrow (r3: the manager)
  using namespace kuf::menu_pointer;
  const uint64_t manager = ctx.r3.u64;
  kuf::title_menu::OnUiUpdate(ctx, base);  // the storage question, answered before it is drawn
  ctx.r3.u64 = manager;
  uint32_t focus = 0, toggled = 0;
  if (REXCVAR_GET(kuf_selection_arrow_auto) && g_memory && Readable(kFocusedControl, 4)) {
    focus = Load32(kFocusedControl);
    if (Readable(focus, 64)) {
      const uint32_t state = Load32(focus + 56);
      const bool keyboard = kuf::prompts::Current() == kuf::input::PromptDevice::kKeyboard;
      uint32_t scene_id = 0;
      if (const uint32_t record = Load32(focus + 48); Readable(record, 4)) scene_id = Load32(record);
      if (keyboard && !(state & kNoArrow)) {
        toggled = kNoArrow;  // no arrow: the highlight and the pointer show the selection
      } else if (!keyboard && (state & kNoArrow) && scene_id == kTitleListScene && REXCVAR_GET(kuf_title_arrow) &&
                 Load32(focus + 12) == kTypeButton) {
        toggled = kNoArrow;  // the title rows: the arrow as in the game's other lists
      }
      if (toggled) Store32(base, focus + 56, state ^ toggled);
    }
  }
  __imp__sub_82612DE0(ctx, base);
  if (toggled && Readable(focus, 64)) Store32(base, focus + 56, Load32(focus + 56) ^ toggled);
}

namespace kuf::menu_pointer {

Step PlanAt(int32_t px, int32_t py, UiButton* focus_out) {
  if (focus_out) *focus_out = {};
  UiButton focus;
  uint32_t type = 0, root = 0, scene_id = 0;
  if (!Focus(&focus, &type, &root, &scene_id)) return {};
  if (type == kTypeListBox) {
    const uint32_t c = Load32(kFocusedControl);
    if (!Readable(c + 376, 36)) return {};
    ListBox l{focus.x, focus.y, focus.w, focus.h};
    const uint32_t first = Load32(c + 392), last = Load32(c + 396);
    l.count = first && last >= first ? int32_t((last - first) / 4) : 0;
    l.index = int32_t(Load32(c + 408));
    l.top = int32_t(Load32(c + 404));
    l.visible = int32_t(Load32(c + 384));
    l.row_height = int32_t(Load32(c + 380));
    if (focus_out) {  // the selected row's position, for the caller's progress check
      *focus_out = focus;
      focus_out->y = l.index;
    }
    return PlanList(l, px, py);
  }
  if (type != kTypeButton) return {};
  if (focus_out) *focus_out = focus;
  std::vector<UiButton> buttons;
  CollectButtons(root, 0, &buttons);
  return Plan(buttons, focus, px, py);
}

}  // namespace kuf::menu_pointer

REX_HOOK_RAW(sub_826221D8) {
  if (REXCVAR_GET(kuf_trace_menu_pointer) && ctx.r6.u32 == kMessageInput) {
    kuf::menu_pointer::UiButton focus;
    uint32_t type = 0, root = 0, scene_id = 0;
    std::string text;
    if (Focus(&focus, &type, &root, &scene_id)) {
      std::vector<kuf::menu_pointer::UiButton> buttons;
      CollectButtons(root, 0, &buttons);
      for (const auto& b : buttons) {
        text += fmt::format("\n    {:08X} ({},{} {}x{}) state {:X}", b.id, b.x, b.y, b.w, b.h, b.state);
      }
      if (type == kTypeListBox) {
        const uint32_t c = Load32(kFocusedControl);
        if (Readable(c + 376, 36)) {
          text += fmt::format(" list: index {} top {} visible {} row height {} items {}", Load32(c + 408), Load32(c + 404),
                              Load32(c + 384), Load32(c + 380), (Load32(c + 396) - Load32(c + 392)) / 4);
        }
      }
      REXLOG_INFO("[kuf-pointer] input {} in scene {:08X}: focus {:08X} type {} ({},{} {}x{}), {} buttons{}", ctx.r8.u32,
                  scene_id, focus.id, type, focus.x, focus.y, focus.w, focus.h, buttons.size(), text);
    }
  }
  __imp__sub_826221D8(ctx, base);
}
