#include "runtime/controls_screen.h"

// The Controls page (scene 0x028 of d:\data\Menu\UiProfileOption.uitb; MENU_ARCHITECTURE section 2): the controller
// picture 0x280500B (120,106 588x590, with its leader lines), one icon text per button at x 702 (50 wide, the game's
// button-name texts 1000-1012) beside the action's text at x 750 (410 wide), three second lines (Hold: Open Inventory
// Menu, Use Detection) at x 760, the D-pad icon and the co-op "Target Ally" block at the lower left.
//
// With the keyboard prompts the icons became their keys ("[Mouse R]"), too wide for their 50-pixel boxes: they wrapped
// over the actions (the player's screenshot). Keyboard and mouse now get a list: the picture and the icons hidden,
// each action's text with its keys in front ("[Mouse R]  Use X Weapon", the keys bound in gameplay now), the list
// moved to the left half where the picture was, the Target Ally block to the right half. A controller gets the game's
// page back exactly (positions and texts as they were). The page is redone when the device or the keys change.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "input/button_prompts.h"
#include "input/kuf_input_driver.h"
#include "runtime/menu_pointer.h"
#include "runtime/prompt_text.h"

REX_EXTERN(sub_82624570);  // control lookup (node, id)
REX_EXTERN(sub_82624CB8);  // set a control's text (control, string, 0): the control keeps its own copy
REX_EXTERN(sub_82623850);  // control state flags (control, flag, on): 1 shown
REX_EXTERN(sub_82624018);  // the toolkit's base SetRect (control, x, y, width, height)
REX_EXTERN(sub_8218F410);  // the game's allocation

REXCVAR_DECLARE(bool, kuf_mouse_look);

namespace kuf::controls_screen {
namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kControlsScene = 0x02800000;
constexpr uint32_t kSceneVtable = 0x8203BD28;
constexpr uint32_t kTitle = 0x2804001, kPicture = 0x280500B;
constexpr size_t kScratch = 256;

struct Line {
  uint32_t icon;    // the icon text (0: none)
  const char* key;  // the icon's name for IconKeys
  uint32_t text;    // the action's text
  uint32_t second;  // its second line (0: none)
};
// From the layout (icon text numbers: 1000 A, 1001 B, 1002 X, 1003 Y, 1004 LB, 1005 RB, 1006 Start, 1007 Back,
// 1008 LT, 1009 RT, 1010 left stick, 1011 right stick, 1012 D-pad).
constexpr Line kLines[] = {
    {0x2804002, "L_Trigger", 0x280400F, 0},         {0x2804003, "R_Trigger", 0x2804010, 0},
    {0x2804004, "L", 0x2804011, 0x280401F},         {0x2804005, "R", 0x2804012, 0x280401D},
    {0x2804006, "Y", 0x2804013, 0x280401E},         {0x2804007, "B", 0x2804014, 0},
    {0x2804008, "X", 0x2804015, 0},                 {0x2804009, "A", 0x2804016, 0},
    {0x280400B, "Start", 0x2804017, 0},             {0x280400C, "Back", 0x2804018, 0},
    {0x280400D, "APad_R_Small", 0x2804019, 0},      {0x280400E, "APad_L_Small", 0x280401A, 0},
};
constexpr uint32_t kAllyIcon = 0x280400A, kAllyTitle = 0x280401B, kHelp = 0x2804020;
constexpr uint32_t kAllyLines[] = {0x280401C, 0x2804021, 0x2804022};

// The keyboard list: the actions' texts from x 150 (the picture's place), 540 wide; second lines 20 to the right; the
// Target Ally block from x 760 (the actions' old column).
constexpr int32_t kListX = 150, kListWidth = 540, kSecondIndent = 20, kAllyX = 760, kAllyWidth = 400;

struct Saved {
  int32_t x, y, w, h;
  std::string text;
};

uint32_t g_menu = 0;
uint32_t g_scratch = 0;
enum class Mode { kNone, kController, kKeyboard };
Mode g_mode = Mode::kNone;
std::string g_keys;  // the keys the keyboard list shows (redone when they change)
uint32_t g_scene = 0;
bool g_shown = false;
std::array<Saved, 64> g_saved{};  // per control index below, its original rectangle and text
bool g_have_saved = false;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFn fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0,
              uint32_t r6 = 0, uint32_t r7 = 0) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  fn(ctx, base);
  return ctx.r3.u32;
}

uint32_t Find(PPCContext& ctx, uint8_t* base, uint32_t id) { return Call(ctx, base, sub_82624570, g_scene, id); }

std::string TextOf(const uint8_t* base, uint32_t control) {
  const uint32_t str = Load32(base, control + 84);
  if (!kuf::menu_pointer::GuestReadable(str & ~3u, 4)) return {};
  const char* p = reinterpret_cast<const char*>(base + str);
  return std::string(p, strnlen(p, kScratch - 1));
}

void SetText(PPCContext& ctx, uint8_t* base, uint32_t control, const std::string& text) {
  if (!g_scratch) {
    ctx.r3.u64 = kScratch;
    sub_8218F410(ctx, base);
    g_scratch = ctx.r3.u32;
    if (!g_scratch) return;
  }
  const size_t n = std::min(text.size(), kScratch - 1);
  std::memcpy(base + g_scratch, text.data(), n);
  base[g_scratch + n] = 0;
  Call(ctx, base, sub_82624CB8, control, g_scratch, 0);
}

void SetRect(PPCContext& ctx, uint8_t* base, uint32_t control, int32_t x, int32_t y, int32_t w, int32_t h) {
  Call(ctx, base, sub_82624018, control, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h));
}

void Show(PPCContext& ctx, uint8_t* base, uint32_t control, bool shown) {
  Call(ctx, base, sub_82623850, control, 1, shown ? 1 : 0);
}

// The Controls scene instance among the options menu's fields (its scenes at +0x20..+0x30), or 0.
uint32_t FindScene(const uint8_t* base) {
  if (!g_menu || !kuf::menu_pointer::GuestReadable(g_menu, 0x100)) return 0;
  for (uint32_t offset = 0; offset < 0x100; offset += 4) {
    const uint32_t p = Load32(base, g_menu + offset);
    // A scene instance (vtable 0x8203BD28): +4 its record, +8 its ID.
    if (kuf::menu_pointer::GuestReadable(p, 16) && Load32(base, p) == kSceneVtable && Load32(base, p + 8) == kControlsScene) {
      return p;
    }
  }
  return 0;
}

// Every control the page changes, in a fixed order (the index into g_saved).
template <typename F>
void ForEachControl(F&& f) {
  size_t i = 0;
  for (const Line& l : kLines) {
    f(i++, l.icon);
    f(i++, l.text);
    if (l.second) f(i++, l.second);
  }
  f(i++, kAllyIcon);
  f(i++, kAllyTitle);
  for (const uint32_t id : kAllyLines) f(i++, id);
  f(i++, kPicture);
  f(i++, kHelp);
}

// The help line's word after its icon ("@(B)Back" or, looked up with the keyboard prompts, "[Esc] Back": "Back").
std::string HelpWord(const std::string& text) {
  size_t at = 0;
  if (text.rfind("@(", 0) == 0 || text.rfind("[", 0) == 0) {
    at = text.find(text[0] == '@' ? ')' : ']');
    at = at == std::string::npos ? 0 : at + 1;
  }
  while (at < text.size() && text[at] == ' ') ++at;
  return text.substr(at);
}

std::string KeysFor(const char* icon) {
  auto* keyboard = kuf::input::KufInputDriver::Instance();
  if (!keyboard) return {};
  return kuf::input::IconKeys(icon, keyboard->CurrentBindings(), kuf::input::Context::kGameplay,
                              REXCVAR_GET(kuf_mouse_look));
}

// An icon text's markup (its text from the game's button-name table may have been looked up as keys).
std::string IconMarkup(uint32_t id) {
  for (const Line& l : kLines) {
    if (l.icon == id) return std::string("@(") + l.key + ")";
  }
  return id == kAllyIcon ? "@(DPad_Small)" : std::string();
}

void ApplyController(PPCContext& ctx, uint8_t* base) {
  ForEachControl([&](size_t i, uint32_t id) {
    const uint32_t c = Find(ctx, base, id);
    if (!c) return;
    const Saved& s = g_saved[i];
    SetRect(ctx, base, c, s.x, s.y, s.w, s.h);
    const std::string markup = IconMarkup(id);
    if (!markup.empty()) {
      SetText(ctx, base, c, markup);
    } else if (id == kHelp) {
      SetText(ctx, base, c, "@(B)" + HelpWord(s.text));
    } else if (id != kPicture) {
      SetText(ctx, base, c, s.text);
    }
    Show(ctx, base, c, true);
  });
}

void ApplyKeyboard(PPCContext& ctx, uint8_t* base) {
  size_t i = 0;
  for (const Line& l : kLines) {
    const uint32_t icon = Find(ctx, base, l.icon), text = Find(ctx, base, l.text);
    const Saved& icon_saved = g_saved[i++];
    const Saved& text_saved = g_saved[i++];
    (void)icon_saved;
    if (icon) Show(ctx, base, icon, false);
    if (text) {
      const std::string keys = KeysFor(l.key);
      SetRect(ctx, base, text, kListX, text_saved.y, kListWidth, text_saved.h);
      SetText(ctx, base, text, keys.empty() ? text_saved.text : keys + "  " + text_saved.text);
    }
    if (l.second) {
      const Saved& second_saved = g_saved[i++];
      if (const uint32_t second = Find(ctx, base, l.second)) {
        SetRect(ctx, base, second, kListX + kSecondIndent, second_saved.y, kListWidth, second_saved.h);
      }
    }
  }
  if (const uint32_t icon = Find(ctx, base, kAllyIcon)) Show(ctx, base, icon, false);
  ++i;
  const Saved& title_saved = g_saved[i++];
  if (const uint32_t title = Find(ctx, base, kAllyTitle)) {
    const std::string keys = KeysFor("Dpad_Small");
    SetRect(ctx, base, title, kAllyX, title_saved.y, kAllyWidth, title_saved.h);
    SetText(ctx, base, title, keys.empty() ? title_saved.text : keys + "  " + title_saved.text);
  }
  for (const uint32_t id : kAllyLines) {
    const Saved& s = g_saved[i++];
    if (const uint32_t c = Find(ctx, base, id)) SetRect(ctx, base, c, kAllyX + kSecondIndent, s.y, kAllyWidth, s.h);
  }
  if (const uint32_t picture = Find(ctx, base, kPicture)) Show(ctx, base, picture, false);
  ++i;
  if (const uint32_t help = Find(ctx, base, kHelp)) {  // Back is the menu's key
    auto* keyboard = kuf::input::KufInputDriver::Instance();
    const std::string back =
        keyboard ? kuf::input::IconKeys("B", keyboard->CurrentBindings(), kuf::input::Context::kMenu, false) : "";
    SetText(ctx, base, help, (back.empty() ? "@(B)" : back + " ") + HelpWord(g_saved[i].text));
  }
}

}  // namespace

void NoteOptionsMenu(uint32_t menu) { g_menu = menu; }

void OnTick(PPCContext& ctx, uint8_t* base) {
  const uint32_t scene = FindScene(base);
  if (!scene) return;
  if (scene != g_scene) {  // a new page instance: its own layout first
    g_scene = scene;
    g_have_saved = false;
    g_mode = Mode::kNone;
  }
  const uint32_t title = Find(ctx, base, kTitle);
  const bool shown = title && (Load32(base, title + 56) & 1);
  if (!shown) {
    g_shown = false;
    return;
  }
  if (!g_have_saved) {  // the game's own layout, kept for the controller page
    bool complete = true;
    ForEachControl([&](size_t i, uint32_t id) {
      const uint32_t c = Find(ctx, base, id);
      if (!c) {
        complete = false;
        return;
      }
      g_saved[i] = {int32_t(Load32(base, c + 16)), int32_t(Load32(base, c + 20)), int32_t(Load32(base, c + 24)),
                    int32_t(Load32(base, c + 28)), TextOf(base, c)};
    });
    if (!complete) {
      REXLOG_WARN("[kuf-controls] the Controls page is not as expected: left as the game draws it");
      g_menu = 0;
      return;
    }
    g_have_saved = true;
  }
  const bool keyboard = kuf::prompts::Current() == kuf::input::PromptDevice::kKeyboard;
  std::string keys;
  if (keyboard) {
    for (const Line& l : kLines) keys += KeysFor(l.key) + "|";
    keys += KeysFor("Dpad_Small");
  }
  const Mode want = keyboard ? Mode::kKeyboard : Mode::kController;
  if (want == g_mode && keys == g_keys && g_shown) return;
  g_shown = true;
  g_mode = want;
  g_keys = keys;
  if (keyboard) {
    ApplyKeyboard(ctx, base);
  } else {
    ApplyController(ctx, base);
  }
  REXLOG_INFO("[kuf-controls] Controls page: {}", keyboard ? "keyboard and mouse (list of the bound keys)"
                                                            : "controller (the game's picture)");
}

}  // namespace kuf::controls_screen
