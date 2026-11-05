#include "runtime/menu_mouse.h"

// The level-up dialog (scene 0x00F, d:\data\Menu\UiLevelUp.uitb; traced with --kuf_trace_scene_dump): its panel is
// nine images 0xF05001-09 (centre 0xF05005 at 309,126 662 x 454), the three attribute rows (HP, SP, Luck) are buttons
// 0xF01001-03 with hidden twins 0xF01007-09 (80 x 30 at x 359, y 156 / 205 / 254), each row's value arrows the
// images 0xF0502F / 30, 0xF05032 / 34 and 0xF05035 / 37, the footer the text 0xF04004 (357,356 566 x 30: text 1284
// "@(A)Confirm   @(X)Reset   @(B)Back"). The player's screenshot showed the panel see-through over the status menu.
//
// Layout (a copy, never the game's file): a backdrop under the panel (its centre image stretched over the screen,
// translucent: what is behind stays visible, dimmed), the nine panel images drawn twice (more opaque), and three
// footer texts, one per entry, shown instead of the footer with the keyboard and mouse (clickable each).
//
// The inventory (scene 0x003): category tabs 0x301001-03 (shown when not the current one) and 0x301004-06 (the
// current one), 50 x 42 in a line; the game changes the category with LT / RT (its texts 1008 / 1009 beside the tabs).
// A click on a tab presses LT or RT as often as the tabs between.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "input/button_prompts.h"
#include "input/kuf_input_driver.h"
#include "runtime/menu_pointer.h"
#include "runtime/prompt_text.h"
#include "runtime/ui_layout.h"

REXCVAR_DEFINE_BOOL(kuf_menu_mouse, true, "KUF/Input",
                    "M7 menu overhaul: the mouse in the level-up dialog (arrows, Confirm / Reset / Back) and the "
                    "inventory's tabs");
REXCVAR_DEFINE_BOOL(kuf_levelup_opaque, true, "KUF/Interface",
                    "M7 menu overhaul: the level-up dialog's panel more opaque, over a dimmed backdrop");
REXCVAR_DEFINE_INT32(kuf_levelup_backdrop_alpha, 150, "KUF/Interface",
                     "M7 menu overhaul: the level-up dialog's backdrop alpha (0-255; 0: none)");

REX_EXTERN(sub_82624CB8);  // set a control's text (control, string, 0): the control keeps its own copy
REX_EXTERN(sub_82623850);  // control state flags (control, flag, on): 1 shown
REX_EXTERN(sub_8218F410);  // the game's allocation
REX_EXTERN(sub_82624018);  // the toolkit's base SetRect (control, x, y, width, height)
REX_EXTERN(sub_82624570);  // control lookup (node, id)
REX_EXTERN(sub_826724D8);  // status menu: show the current tab (its buttons, title, rows)
REX_EXTERN(sub_82671B30);  // status menu: the rows for the selection (their texts and icons)
REX_EXTERN(sub_8266FB78);  // status menu: the highlights on the selected row (texts, shown states)

namespace kuf::menu_mouse {
namespace {

using kuf::input::Control;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kLevelUpScene = 0x00F00000;
constexpr uint32_t kPanelFirst = 0xF05001, kPanelCentre = 0xF05005, kPanelCount = 9;
constexpr uint32_t kBackdrop = 0xF05080, kPanelCopy = 0xF05081;  // the copies: 0xF05081-89
constexpr uint32_t kRows[] = {0xF01001, 0xF01002, 0xF01003};
constexpr uint32_t kArrows[3][2] = {{0xF0502F, 0xF05030}, {0xF05032, 0xF05034}, {0xF05035, 0xF05037}};
constexpr int32_t kArrowReach = 8;  // pixels around an arrow that still take its click
constexpr auto kRepeatDelay = std::chrono::milliseconds(400), kRepeatEvery = std::chrono::milliseconds(90);


constexpr size_t kScratch = 256;
uint32_t g_scratch = 0;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

struct Rect {
  int32_t x = 0, y = 0, w = 0, h = 0;
  bool Contains(int32_t px, int32_t py, int32_t pad = 0) const {
    return w > 0 && px >= x - pad && px < x + w + pad && py >= y - pad && py < y + h + pad;
  }
};
Rect RectOf(const uint8_t* base, uint32_t c) {
  return {int32_t(Load32(base, c + 16)), int32_t(Load32(base, c + 20)), int32_t(Load32(base, c + 24)),
          int32_t(Load32(base, c + 28))};
}
bool Shown(const uint8_t* base, uint32_t c) { return c && (Load32(base, c + 56) & 1); }

void Call(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t r3, uint32_t r4, uint32_t r5) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  fn(ctx, base);
}
void Show(PPCContext& ctx, uint8_t* base, uint32_t c, bool shown) {
  if (c && Shown(base, c) != shown) Call(ctx, base, sub_82623850, c, 1, shown ? 1 : 0);
}
std::string TextOf(const uint8_t* base, uint32_t c) {
  const uint32_t str = Load32(base, c + 84);
  if (!kuf::menu_pointer::GuestReadable(str & ~3u, 4)) return {};
  const char* p = reinterpret_cast<const char*>(base + str);
  return std::string(p, strnlen(p, kScratch - 1));
}
void SetText(PPCContext& ctx, uint8_t* base, uint32_t c, const std::string& text) {
  if (!g_scratch) {
    ctx.r3.u64 = kScratch;
    sub_8218F410(ctx, base);
    g_scratch = ctx.r3.u32;
    if (!g_scratch) return;
  }
  const size_t n = std::min(text.size(), kScratch - 1);
  std::memcpy(base + g_scratch, text.data(), n);
  base[g_scratch + n] = 0;
  Call(ctx, base, sub_82624CB8, c, g_scratch, 0);
}

// --- Clickable footers (keyboard and mouse) -------------------------------------------------------------------------
//
// A footer is one text naming several buttons ("[Enter] Change Item  [X] Auto-equip  ..."). With the keyboard and
// mouse it is shown as one text per entry instead (copies of the footer's text control added to the layout), laid
// out side by side over the footer's own place (each as wide as its text), so each can be clicked: a click is the
// entry's button as the game's own input event to the focused control (the handler the game's input would reach),
// the same as the key. With a controller the game's footer shows.

struct FooterSpec {
  uint32_t scene, source, first, max;
};
constexpr FooterSpec kFooters[] = {
    {kLevelUpScene, 0xF04004, 0xF04080, 3},  // "@(A)Confirm   @(X)Reset   @(B)Back"
    {0x00300000, 0x304006, 0x304080, 7},     // the equipment screen: Change Item, Auto-equip, Details, ..., Back
    {0x00400000, 0x404001, 0x404080, 5},     // the item list ("Change Item"): Equip, Destroy, Details, Back
};

struct Footer {
  uint32_t source = 0;  // the footer control laid out for (another one: a new screen)
  bool keyboard = false;
  std::string text;     // the footer's text the entries were made from
  Rect place;           // and its place (the screens slide in)
  std::array<Control, 8> controls{};
  std::array<bool, 8> known{};
  size_t count = 0;
};
Footer g_footer;
const FooterSpec* g_footer_spec = nullptr;

void SetRect(PPCContext& ctx, uint8_t* base, uint32_t c, const Rect& r) {
  ctx.r3.u64 = c;
  ctx.r4.s64 = r.x;
  ctx.r5.s64 = r.y;
  ctx.r6.s64 = r.w;
  ctx.r7.s64 = r.h;
  sub_82624018(ctx, base);
}

void UpdateFooter(PPCContext& ctx, uint8_t* base, const FooterSpec& spec) {
  const uint32_t source = kuf::menu_pointer::FindInFocusedScene(spec.source);
  std::array<uint32_t, 8> entries{};
  for (uint32_t i = 0; i < spec.max; ++i) {
    entries[i] = kuf::menu_pointer::FindInFocusedScene(spec.first + i);
    if (!entries[i]) return;  // the layout was not extended
  }
  if (!source) return;
  if (source != g_footer.source || g_footer_spec != &spec) {
    g_footer = Footer{source};
    g_footer_spec = &spec;
  }
  const bool keyboard = kuf::prompts::Current() == kuf::input::PromptDevice::kKeyboard;
  if (!keyboard) {
    if (g_footer.keyboard || Shown(base, entries[0])) {
      for (uint32_t i = 0; i < spec.max; ++i) Show(ctx, base, entries[i], false);
      Show(ctx, base, source, true);
      g_footer.keyboard = false;
    }
    return;
  }
  // The game's footer text (it changes with the selection: another row, another tab) and place.
  const bool game_showed = Shown(base, source);
  const std::string text = game_showed || !g_footer.keyboard ? TextOf(base, source) : g_footer.text;
  const Rect place = RectOf(base, source);
  if (g_footer.keyboard && text == g_footer.text && place.x == g_footer.place.x && place.y == g_footer.place.y &&
      !game_showed) {
    return;
  }
  const auto parts = kuf::input::SplitHelpBar(text);
  if (parts.empty() || parts.size() > spec.max) return;  // not a footer as expected: left as the game shows it
  auto* driver = kuf::input::KufInputDriver::Instance();
  const auto bindings = driver ? driver->CurrentBindings() : kuf::input::Bindings::Defaults();
  size_t total = 0;
  for (const auto& p : parts) total += kuf::input::VisibleLength(p) + 2;
  int32_t x = place.x;
  for (size_t i = 0; i < spec.max; ++i) {
    if (i >= parts.size()) {
      Show(ctx, base, entries[i], false);
      continue;
    }
    const int32_t w = i + 1 == parts.size() ? place.x + place.w - x
                                            : int32_t(place.w * (kuf::input::VisibleLength(parts[i]) + 2) / total);
    SetRect(ctx, base, entries[i], {x, place.y, w, place.h});
    x += w;
    const auto control = kuf::input::EntryControl(parts[i], bindings);
    g_footer.known[i] = control.has_value();
    if (control) g_footer.controls[i] = *control;
    SetText(ctx, base, entries[i], kuf::prompts::ForCurrentDevice(parts[i]));
    Show(ctx, base, entries[i], true);
  }
  Show(ctx, base, source, false);
  if (!g_footer.keyboard || text != g_footer.text) {
    REXLOG_INFO("[kuf-mouse] footer {:08X}: {} clickable entries", spec.scene, parts.size());
  }
  g_footer.keyboard = true;
  g_footer.text = text;
  g_footer.place = place;
  g_footer.count = parts.size();
}

// The game's input event code for a button (the codes its menus' handlers get: MENU_ARCHITECTURE 2.5, the status
// menu's handler sub_82673B08); 0xFFFFFFFF: none (a pad press then).
uint32_t EventCode(Control c) {
  switch (c) {
    case Control::kA: return 0;
    case Control::kB: return 1;
    case Control::kX: return 2;
    case Control::kY: return 3;
    case Control::kLeftShoulder: return 4;
    case Control::kRightShoulder: return 5;
    default: return 0xFFFFFFFFu;
  }
}

void Press(PPCContext& ctx, uint8_t* base, kuf::input::KufInputDriver* driver, Control c) {
  const uint32_t code = EventCode(c);
  if (code == 0xFFFFFFFFu || !kuf::menu_pointer::Dispatch(ctx, base, code)) driver->PulseControl(c);
}

bool FooterPointer(PPCContext& ctx, uint8_t* base, kuf::input::KufInputDriver* driver, int32_t px, int32_t py,
                   bool click) {
  if (!g_footer.keyboard || !g_footer_spec || kuf::menu_pointer::FocusedSceneId() != g_footer_spec->scene) {
    return false;
  }
  for (uint32_t i = 0; i < g_footer.count; ++i) {
    const uint32_t entry = kuf::menu_pointer::FindInFocusedScene(g_footer_spec->first + i);
    if (!Shown(base, entry) || !RectOf(base, entry).Contains(px, py)) continue;
    if (!click) return true;  // over an entry: no row hover beneath it
    if (!g_footer.known[i]) return true;  // a direction entry (or unknown): nothing to press
    REXLOG_INFO("[kuf-mouse] footer entry '{}' clicked", TextOf(base, entry));
    Press(ctx, base, driver, g_footer.controls[i]);
    return true;
  }
  return false;
}

// --- Pointer -------------------------------------------------------------------------------------------------------

struct Repeat {
  bool active = false;
  uint32_t arrow = 0;
  Control control = Control::kDpadRight;
  Clock::time_point next;
};
Repeat g_repeat;

// The row the selection is on (index into kRows by its top), or -1.
int FocusedRow(const uint8_t* base) {
  constexpr uint32_t kFocusedControl = 0x82ABBB90;
  const uint32_t focus = Load32(base, kFocusedControl);
  if (!kuf::menu_pointer::GuestReadable(focus, 64)) return -1;
  const int32_t y = int32_t(Load32(base, focus + 20));
  for (int r = 0; r < 3; ++r) {
    const uint32_t row = kuf::menu_pointer::FindInFocusedScene(kRows[r]);
    if (row && int32_t(Load32(base, row + 20)) == y) return r;
  }
  return -1;
}

bool LevelUpPointer(const uint8_t* base, kuf::input::KufInputDriver* driver, int32_t px, int32_t py, bool click,
                    bool held) {
  if (g_repeat.active) {
    const uint32_t arrow = kuf::menu_pointer::FindInFocusedScene(g_repeat.arrow);
    if (!held || !arrow || !RectOf(base, arrow).Contains(px, py, kArrowReach)) {
      g_repeat.active = false;
      return held;  // the press stays the arrow's until released
    }
    if (Clock::now() >= g_repeat.next && !driver->PendingPulses()) {
      driver->PulseControl(g_repeat.control);
      g_repeat.next = Clock::now() + kRepeatEvery;
    }
    return true;
  }
  if (!click) return false;  // hover: the generic row hover (selection only)
  // A modal dialog: every click inside it is taken here, so only its arrows and its footer entries act (a click on a
  // row selects it; elsewhere, inside or outside the panel, nothing: no Confirm, no Back, nothing behind it).
  for (int r = 0; r < 3; ++r) {
    uint32_t pair[2] = {kuf::menu_pointer::FindInFocusedScene(kArrows[r][0]),
                        kuf::menu_pointer::FindInFocusedScene(kArrows[r][1])};
    for (int k = 0; k < 2; ++k) {
      const uint32_t arrow = pair[k], other = pair[1 - k];
      if (!Shown(base, arrow) || !RectOf(base, arrow).Contains(px, py, kArrowReach)) continue;
      const bool left = other && RectOf(base, arrow).x < RectOf(base, other).x;
      const int from = FocusedRow(base);
      if (from < 0) return true;  // the selection is not on a row (taken: no stray confirm)
      for (int i = from; i != r; i += r > from ? 1 : -1) {
        driver->PulseControl(r > from ? Control::kDpadDown : Control::kDpadUp);
      }
      const Control step = left ? Control::kDpadLeft : Control::kDpadRight;
      driver->PulseControl(step);
      g_repeat = {true, kArrows[r][k], step, Clock::now() + kRepeatDelay};
      REXLOG_INFO("[kuf-mouse] level-up: row {} {} arrow", r, left ? "left" : "right");
      return true;
    }
  }
  for (int r = 0; r < 3; ++r) {  // a row: selected (its own Select would be the dialog's Confirm)
    const uint32_t row = kuf::menu_pointer::FindInFocusedScene(kRows[r]);
    if (!row || !RectOf(base, row).Contains(px, py)) continue;
    const int from = FocusedRow(base);
    for (int i = from; from >= 0 && i != r; i += r > from ? 1 : -1) {
      driver->PulseControl(r > from ? Control::kDpadDown : Control::kDpadUp);
    }
    return true;
  }
  return true;
}

// --- The equipment screen (the status menu, sub_82673B08) ----------------------------------------------------------
//
// Its object (the handler's r3): +32 the equipment scene (0x003), +48 the scene of row 0 (0x010), +196 the current
// tab (0-2), +216 + 4 row the rows' buttons (row 0 in scene 0x010; rows 1-4 the equipment slots 0x301007-0A),
// +236..+256 the tab buttons (0x301001-06: not current / current, same rectangles), +376 the selected row, +380 the
// last row. The game's own events move it: Up / Down a row (row 0 only when it may be chosen), Left / Right the tab;
// Select acts on the row (the item list, or a notice when nothing fits). The pointer brings the game's state to what
// is under it with those events and checks the result in the object (never written here); the generic row rule is
// not used on this screen (its two icon columns belong to different tabs: a "Right" would change the tab).

std::atomic<uint32_t> g_status_menu{0};
constexpr uint32_t kStatusScene = 0x00300000;
constexpr uint32_t kTab = 196, kRowsAt = 216, kTabsAt = 236, kSelected = 376, kLastRowAt = 380;
constexpr int32_t kRowReachLeft = 110;  // the row's button icons left of its slot are part of the row
constexpr uint32_t kCodeUp = 6, kCodeDown = 7, kCodeLeft = 8, kCodeRight = 9, kCodeSelect = 0;

// The status menu when one of its scenes has the focus (0 otherwise).
uint32_t StatusMenu(const uint8_t* base) {
  const uint32_t menu = g_status_menu.load();
  if (!menu || !kuf::menu_pointer::GuestReadable(menu, 400)) return 0;
  constexpr uint32_t kFocusedControl = 0x82ABBB90;
  const uint32_t focus = Load32(base, kFocusedControl);
  if (!kuf::menu_pointer::GuestReadable(focus, 64)) return 0;
  const uint32_t record = Load32(base, focus + 48);
  for (const uint32_t at : {32u, 48u}) {
    const uint32_t scene = Load32(base, menu + at);
    if (kuf::menu_pointer::GuestReadable(scene, 12) && Load32(base, scene + 4) == record) {
      return at == 32 && Load32(base, scene + 8) != kStatusScene ? 0 : menu;
    }
  }
  return 0;
}

// Steps the game's selection (Up / Down) or tab (Left / Right) towards `target` with its own events; true if reached.
bool StepTo(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t field, int32_t target, uint32_t less,
            uint32_t more) {
  for (int i = 0; i < 8; ++i) {
    const int32_t now = int32_t(Load32(base, menu + field));
    if (now == target) return true;
    if (!kuf::menu_pointer::Dispatch(ctx, base, target < now ? less : more)) return false;
    if (int32_t(Load32(base, menu + field)) == now) return false;  // refused (row 0 not available, or the end)
  }
  return int32_t(Load32(base, menu + field)) == target;
}

// The equipment screen's button icons (M7 UI follow-up; the player's screenshot): each slot row has small icon boxes
// (40 x 34: +308..+320 and +324..+336, the highlights +360 / +364 over the selected one) whose text is one button
// icon ("@(A)": which pad button uses that slot), and the tabs have the trigger icons 0x304001 / 0x304002 beside them.
// The keyboard prompts rewrite each to its key ("[Enter]", "[Mouse R]"), far wider than the box: they spilled over the
// slots. With the keyboard and mouse those icon texts are left empty (the rows, the tabs and the footer are clickable;
// the keys still work) and the trigger icons hidden; with a controller the game's own refresh (as its tab change runs
// it) looks them up again as icons.
constexpr uint32_t kIconFields[] = {308, 312, 316, 320, 324, 328, 332, 336, 360, 364};
constexpr uint32_t kTriggerIcons[] = {0x304001, 0x304002};
bool g_icons_cleared = false;

void StatusPrompts(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  const bool keyboard = kuf::prompts::Current() == kuf::input::PromptDevice::kKeyboard;
  const uint32_t scene = Load32(base, menu + 32);
  // StatusMenu may match through the row-0 scene (+48); a menu being torn down (the return to the title after a save)
  // can leave junk here (0x2A seen, a crash in the control lookup), so the equipment scene is checked too.
  if (!kuf::menu_pointer::GuestReadable(scene, 12) || Load32(base, scene + 8) != kStatusScene) return;
  auto trigger = [&](uint32_t id) {
    ctx.r3.u64 = scene;
    ctx.r4.u64 = id;
    sub_82624570(ctx, base);
    return ctx.r3.u32;
  };
  if (keyboard) {
    for (const uint32_t at : kIconFields) {
      const uint32_t c = Load32(base, menu + at);
      if (!kuf::menu_pointer::GuestReadable(c, 96)) continue;
      if (!TextOf(base, c).empty()) SetText(ctx, base, c, "");
    }
    for (const uint32_t id : kTriggerIcons) Show(ctx, base, trigger(id), false);
    if (!g_icons_cleared) REXLOG_INFO("[kuf-mouse] equipment: button icons cleared (keyboard and mouse)");
    g_icons_cleared = true;
  } else if (g_icons_cleared) {
    for (const uint32_t id : kTriggerIcons) Show(ctx, base, trigger(id), true);
    for (auto* fn : {sub_826724D8, sub_82671B30, sub_8266FB78}) {
      ctx.r3.u64 = menu;
      fn(ctx, base);
    }
    g_icons_cleared = false;
    REXLOG_INFO("[kuf-mouse] equipment: button icons back (controller)");
  }
}

bool StatusPointer(PPCContext& ctx, uint8_t* base, uint32_t menu, int32_t px, int32_t py, bool moved, bool click) {
  // Tabs: a click changes to that tab.
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t tab = Load32(base, menu + kTabsAt + 4 * i);
    if (!kuf::menu_pointer::GuestReadable(tab, 64) || !Shown(base, tab) || !RectOf(base, tab).Contains(px, py)) {
      continue;
    }
    const int32_t target = int32_t((Load32(base, tab + 8) & 0xF) - 1) % 3;  // 0x301001-06: tabs 0, 1, 2 twice
    if (click) {
      const int32_t was = int32_t(Load32(base, menu + kTab));
      const bool ok = StepTo(ctx, base, menu, kTab, target, kCodeLeft, kCodeRight);
      REXLOG_INFO("[kuf-mouse] equipment: tab {} -> {}{}", was, target, ok ? "" : " (not reached)");
    }
    return true;
  }
  // Rows: hover selects, a click acts on the row (Select).
  const int32_t last = std::min<int32_t>(int32_t(Load32(base, menu + kLastRowAt)), 8);
  for (int32_t r = 0; r <= last; ++r) {
    const uint32_t row = Load32(base, menu + kRowsAt + 4 * uint32_t(r));
    if (!kuf::menu_pointer::GuestReadable(row, 64) || !Shown(base, row)) continue;
    Rect hit = RectOf(base, row);
    if (r > 0) {
      hit.x -= kRowReachLeft;
      hit.w += kRowReachLeft;
    }
    if (!hit.Contains(px, py)) continue;
    if (!moved && !click) return true;
    const bool reached = StepTo(ctx, base, menu, kSelected, r, kCodeUp, kCodeDown);
    if (click && reached) {
      REXLOG_INFO("[kuf-mouse] equipment: row {} clicked", r);
      kuf::menu_pointer::Dispatch(ctx, base, kCodeSelect);
    }
    return true;
  }
  return true;  // elsewhere on this screen: nothing (no generic step that could change the tab)
}

}  // namespace

void NoteStatusMenu(uint32_t menu) {
  if (menu >= 0x10000 && menu < 0xF0000000) g_status_menu.store(menu);
}

bool IsExtendedLayout(std::string_view lower_name) {
  constexpr std::string_view kSuffix = ".uitb";  // any menu layout: the scenes decide (ExtendLayout)
  return lower_name.size() >= kSuffix.size() && lower_name.substr(lower_name.size() - kSuffix.size()) == kSuffix;
}

bool ExtendLayout(std::string_view name, std::vector<uint8_t>* data, std::string* why) {
  if (!IsExtendedLayout(name)) return false;
  std::string ignored;
  const auto layout = kuf::ui::Parse(*data, &ignored);
  if (!layout) return false;  // not one this code knows: left alone
  auto has = [&](uint32_t scene_id, uint32_t control) {
    for (const auto& scene : layout->scenes) {
      if (scene.id() != scene_id) continue;
      for (const auto& c : scene.controls) {
        if (c.id == control) return true;
      }
    }
    return false;
  };
  std::vector<uint8_t> out = *data;
  bool changed = false;
  if (REXCVAR_GET(kuf_levelup_opaque) && has(kLevelUpScene, kPanelCentre)) {
    std::vector<kuf::ui::ImageClone> clones;
    const int32_t alpha = std::clamp(int32_t(REXCVAR_GET(kuf_levelup_backdrop_alpha)), 0, 255);
    if (alpha > 0) clones.push_back({kPanelCentre, kBackdrop, true, 0, 0, 1280, 720, alpha});
    for (uint32_t i = 0; i < kPanelCount; ++i) clones.push_back({kPanelFirst + i, kPanelCopy + i});
    auto with = kuf::ui::AddImageClones(out, kLevelUpScene, clones, kPanelFirst, why);
    if (!with) return false;
    out = std::move(*with);
    changed = true;
  }
  if (REXCVAR_GET(kuf_menu_mouse)) {
    for (const FooterSpec& f : kFooters) {
      if (!has(f.scene, f.source)) continue;
      for (uint32_t i = 0; i < f.max; ++i) {  // placed by UpdateFooter
        auto with = kuf::ui::AddTextClone(out, f.scene, f.source, f.first + i, 0, 0, 10, 10, 0, why);
        if (!with) return false;
        out = std::move(*with);
      }
      changed = true;
    }
  }
  // The game's message box: the update offer's three buttons (src/runtime/title_menu.cpp; hidden in other boxes).
  if (has(kuf::ui::kMessageBoxScene, kuf::ui::kMessageBoxButton)) {
    std::vector<kuf::ui::ButtonClone> clones;
    for (const uint32_t id : kuf::ui::kUpdateBoxButtons) clones.push_back({id, 423, 424});  // placed by the offer
    auto with = kuf::ui::AddButtonClones(out, kuf::ui::kMessageBoxScene, kuf::ui::kMessageBoxButton, clones, why);
    if (!with) return false;
    out = std::move(*with);
    changed = true;
  }
  if (changed) *data = std::move(out);
  return changed;
}

void OnTick(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(kuf_menu_mouse)) return;
  const uint32_t scene = kuf::menu_pointer::FocusedSceneId();
  for (const FooterSpec& f : kFooters) {
    if (f.scene == scene) UpdateFooter(ctx, base, f);
  }
  if (const uint32_t menu = StatusMenu(base)) StatusPrompts(ctx, base, menu);
}

bool Pointer(PPCContext& ctx, uint8_t* base, int32_t px, int32_t py, bool moved, bool click, bool held) {
  if (!REXCVAR_GET(kuf_menu_mouse)) return false;
  auto* driver = kuf::input::KufInputDriver::Instance();
  if (!driver) return false;
  const uint32_t scene = kuf::menu_pointer::FocusedSceneId();
  if (scene != kLevelUpScene) g_repeat.active = false;
  if (FooterPointer(ctx, base, driver, px, py, click)) return true;
  if (scene == kLevelUpScene) return LevelUpPointer(base, driver, px, py, click, held);
  if (const uint32_t menu = StatusMenu(base)) return StatusPointer(ctx, base, menu, px, py, moved, click);
  return false;
}

}  // namespace kuf::menu_mouse

// The status menu's event handler (menu, scene instance, event): its object is kept for the equipment screen's
// pointer (StatusMenu checks it against the focused scene before any use).
REX_EXTERN(__imp__sub_82673B08);
REX_HOOK_RAW(sub_82673B08) {
  kuf::menu_mouse::NoteStatusMenu(ctx.r3.u32);
  __imp__sub_82673B08(ctx, base);
}
