// The multiplayer menus on the game type screen (docs/MULTIPLAYER.md "Menus"; the model: src/coop/lobby_menu.cpp).
// Opt-in (--kuf_lobby_menus) until verified in the game: off, the screen is the game's own.
//
// The game type screen (scene 0x02F, UiStartGame.uitb): six rows, each a shown button (menu +260 + 4 row) and a
// highlighted twin (+284 + 4 row): 0 Difficulty (a value row, its text written by sub_826678D8 from +324: texts
// 70-72), 1 single player, 2 Host Private, 3 Host Public, 4 Quick Match, 5 Co-op Lobby; +320 the selected row,
// +308 the description line. sub_8266A3F8(menu, event) is its input handler (event +12: 0 Select, 1 Back, 6-9
// Up, Down, Left, Right), sub_82669010(menu) its per-tick update (with the co-op poll), sub_82667780(menu) shows
// the selected row (its twin, the focus, the description).
//
// With the menus on, every page is drawn in these rows: row 0 keeps the game's Difficulty on Home (solo play keeps
// it) and is the page title elsewhere; rows 1-5 show five of the page's rows (scrolling with the selection). All of
// the screen's input goes to the model; its steps are the game's own rows: Continue Solo Game selects row 1, Start
// Game row 3 / 2 (Host Public / Private: the game creates its co-op session, published by the co-op runtime), a
// guest's handover row 4 (Quick Match: the game's search finds the host's session), Back the game's own Back. The
// mouse works through the menu pointer as on any screen (hover steps the selection, a click selects).

#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "coop/coop_runtime.h"
#include "coop/lobby_menu.h"
#include "coop/lobby_service.h"
#include "input/kuf_input_driver.h"
#include "runtime/lobby_screen.h"
#include "runtime/menu_pointer.h"
#include "runtime/ui_layout.h"

REXCVAR_DECLARE(bool, kuf_trace_text_input);
REXCVAR_DEFINE_BOOL(kuf_lobby_flash, true, "KUF/Multiplayer",
                    "The selected row's label and value in the game's selected-text font (FA_GAMEOVER_L_SEL: left-"
                    "aligned like the columns), as the game's own selected buttons (FA_BUTTON_R_SEL)");
REXCVAR_DEFINE_BOOL(kuf_trace_selection_flash, false, "KUF/Debug",
                    "Log, for 2 s after each selection change on the game type screen, the focused (selected) button's "
                    "and the selected row's label text's colour, alpha and state fields every 2 ticks: how the game "
                    "animates its selected text ([kuf-flash])");
REXCVAR_DEFINE_BOOL(kuf_lobby_menus, true, "KUF/Multiplayer",
                    "The multiplayer menus (Create Lobby, Browse Lobbies, Join by Code) on the game type screen "
                    "(seen working in the game by the player, 2026-11-01; =false hides them)");

#if !defined(_WIN32)

REX_EXTERN(__imp__sub_8266A3F8);  // the game type screen's input handler (menu, event)
REX_EXTERN(__imp__sub_82669010);  // its per-tick update (menu)
REX_EXTERN(sub_82667780);         // show the selected row (menu)
REX_EXTERN(sub_826678D8);         // write the Difficulty row's text (menu)
REX_EXTERN(sub_82624CB8);         // set a control's text (control, string, 0): the control keeps a copy
REX_EXTERN(sub_82623850);         // a control's state flag (control, flag, on): 1 shown, 0x10 unavailable
REX_EXTERN(sub_8218F410);         // the game's allocation

namespace kuf::lobby_screen {
namespace {

using kuf::coop::Action;
using kuf::coop::LobbyMenu;
using kuf::coop::MenuEffect;
using kuf::coop::MenuInput;
using kuf::coop::MenuPage;
using kuf::coop::MenuRow;
using kuf::coop::MenuView;
using GuestFn = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kScene = 0x02F00000;
constexpr uint32_t kRowButtons = 260, kRowTwins = 284, kSelected = 320, kDifficulty = 324, kDescription = 308;
constexpr uint32_t kRows = 6;            // on screen: row 0 (Difficulty / title) and rows 1-5 (the page)
constexpr uint32_t kPageRows = kRows - 1;
constexpr uint32_t kRowSolo = 1, kRowHostPrivate = 2, kRowHostPublic = 3, kRowQuickMatch = 4;
constexpr uint32_t kCodeSelect = 0, kCodeBack = 1, kCodeUp = 6, kCodeDown = 7, kCodeLeft = 8, kCodeRight = 9;
constexpr uint32_t kEventCode = 12, kEventMessage = 4, kMessageInput = 1100;
constexpr uint32_t kFlagShown = 1, kFlagUnavailable = 0x10;
constexpr uint32_t kScratch = 512;
// A control's font per state (SEL, DEF, NONE: sub_82624690 draws with +288 + 4 * state). The selected look: the
// game's own selected-text font, left-aligned at the columns' size; no colour of its own (as FA_BUTTON_R_SEL, the
// rows' selected font: the renderer colours, and animates, selected text).
constexpr uint32_t kStateFonts = 288;
constexpr uint32_t kSelectedFont = 83;  // FA_GAMEOVER_L_SEL: @(scale=0.8,0.8)@(offsety=-3), left
constexpr size_t kValueChars = 28;  // the value column's width in characters (an edited field: kFieldChars)
constexpr uint32_t kProbeCode = 0x7FFF;  // an input code no menu uses: only brings the menu object to the handler

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFn fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  fn(ctx, base);
  return ctx.r3.u32;
}

struct State {
  std::unique_ptr<LobbyMenu> menu;
  uint32_t screen = 0;      // the game type screen's menu object (its last update)
  uint32_t scratch = 0;     // a guest string buffer
  uint32_t event = 0;       // a guest input event for the game's own steps
  bool difficulty = false;  // Home: the game's Difficulty row (row 0) has the selection
  std::string drawn;        // what was drawn last (redrawn on change)
  std::optional<MenuEffect> pending;  // a game step waiting for the screen
  bool on_screen = false;   // the screen was shown at the last tick
  uint32_t ticks = 0;
  std::string difficulty_name;
  bool difficulty_logged = false;
  bool update_logged = false;  // the screen's own update hook, observed once per screen
  std::map<uint32_t, std::array<uint32_t, 3>> fonts;  // the columns' own state fonts (restored when not selected)
  bool probe_failed = false;
  const uint8_t* base = nullptr;
};
State g;

LobbyMenu& Menu() {
  if (!g.menu) {
    g.menu = std::make_unique<LobbyMenu>(kuf::coop::Lobby(), kuf::coop::PlayerName());
    g.menu->SetVisible(kPageRows);  // Home; the other pages: all six rows (Sync)
    g.menu->Open();
  }
  return *g.menu;
}

bool OnScreen() { return kuf::menu_pointer::FocusedSceneId() == kScene; }

bool Live(uint32_t menu) {
  using kuf::menu_pointer::GuestReadable;
  if (!g.base) return false;
  if (!menu || !GuestReadable(menu, 340)) return false;
  for (uint32_t i = 0; i < kRows; ++i) {
    if (!GuestReadable(Load32(g.base, menu + kRowButtons + 4 * i), 64)) return false;
  }
  return true;
}

void SetText(PPCContext& ctx, uint8_t* base, uint32_t control, const std::string& text) {
  if (!g.scratch) {
    g.scratch = Call(ctx, base, sub_8218F410, kScratch);
    if (!g.scratch) return;
  }
  // The menus' text is UTF-8; the game's controls draw single-byte (Latin-1) text: Å Ä Ö as their Latin-1 bytes,
  // anything beyond as '?'.
  std::string latin1;
  for (size_t i = 0; i < text.size() && latin1.size() < kScratch - 1;) {
    const unsigned char b = static_cast<unsigned char>(text[i]);
    if (b < 0x80) {
      latin1.push_back(char(b));
      ++i;
    } else if ((b >> 5) == 0x6 && i + 1 < text.size()) {
      const uint32_t c = uint32_t(b & 0x1F) << 6 | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      latin1.push_back(c <= 0xFF ? char(c) : '?');
      i += 2;
    } else {
      latin1.push_back('?');
      ++i;
      while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
    }
  }
  std::memcpy(base + g.scratch, latin1.data(), latin1.size());
  base[g.scratch + latin1.size()] = 0;
  Call(ctx, base, sub_82624CB8, control, g.scratch, 0);
}

void Flag(PPCContext& ctx, uint8_t* base, uint32_t control, uint32_t flag, bool on) {
  Call(ctx, base, sub_82623850, control, flag, on ? 1 : 0);
}

std::string TextOf(const uint8_t* base, uint32_t control) {
  const uint32_t str = Load32(base, control + 84);
  if (!kuf::menu_pointer::GuestReadable(str & ~3u, 4)) return {};
  const char* p = reinterpret_cast<const char*>(base + str);
  return std::string(p, strnlen(p, 120));
}

// A column's font: the selected look, or its own fonts back (kept the first time it is changed).
void SetLook(uint8_t* base, uint32_t control, bool selected) {
  auto it = g.fonts.find(control);
  if (it == g.fonts.end()) {
    if (!selected) return;  // never changed: its own fonts
    std::array<uint32_t, 3> own{};
    for (uint32_t i = 0; i < 3; ++i) own[i] = Load32(base, control + kStateFonts + 4 * i);
    it = g.fonts.emplace(control, own).first;
  }
  for (uint32_t i = 0; i < 3; ++i) Store32(base, control + kStateFonts + 4 * i, selected ? kSelectedFont : it->second[i]);
}

std::string Label(const MenuRow& r, bool editing) {
  std::string s = r.label;
  const std::string value = editing ? r.value + "_" : r.value;
  if (!value.empty()) s += "   " + value;
  return s;
}

// The page in the game's rows (only when something changed).
void Draw(PPCContext& ctx, uint8_t* base, uint32_t menu, bool force) {
  LobbyMenu& m = Menu();
  const MenuView v = m.View();
  const bool home = v.page == MenuPage::kHome;
  std::string key = std::to_string(int(v.page)) + "|" + std::to_string(v.selected) + "|" + std::to_string(v.top) + "|" +
                    std::to_string(g.difficulty) + "|" + std::to_string(v.editing) + "|" + v.message;
  for (const MenuRow& r : v.rows) key += "|" + r.label + "=" + r.value + (r.enabled ? "" : "!");
  if (!force && key == g.drawn) return;
  const bool page_changed = g.drawn.empty() || g.drawn.compare(0, 2, key, 0, 2) != 0;
  g.drawn = key;
  if (page_changed) {
    std::string rows;
    for (const MenuRow& r : v.rows) {
      // The log never has a password (the host's is shown on screen only).
      const std::string value = r.label == "Password:" && r.value != "None" ? "(not logged)" : r.value;
      const bool colon = !r.label.empty() && r.label.back() == ':';
      rows += " | " + r.label + (value.empty() ? "" : (colon ? " " : ": ") + value);
    }
    REXLOG_INFO("[kuf-lobby] page {}{}", v.title, rows);
  }
  if (!force) {
    REXLOG_INFO("[kuf-lobby] {} selected '{}'{}{}", v.title,
                g.difficulty ? "Difficulty" : v.selected < v.rows.size() ? v.rows[v.selected].label : "",
                v.editing ? " (typing)" : "", v.message.empty() ? "" : ": " + v.message);
  }
  // Home: row 0 is the game's Difficulty and the page uses rows 1-5; every other page uses all six rows (its title
  // goes to the description line).
  const uint32_t first_row = home ? 1 : 0;
  const uint32_t shown_row = g.difficulty ? 0 : uint32_t(first_row + v.selected - v.top);
  Store32(base, menu + kSelected, shown_row);
  Call(ctx, base, sub_82667780, menu);  // the game's highlight, focus and its description (replaced below)
  if (home) Call(ctx, base, sub_826678D8, menu);  // the game's own Difficulty row
  for (uint32_t slot = 0; slot < kRows; ++slot) {
    // The columns (fixed text controls on the row's line; absent if the layout was not extended: then the old way).
    const uint32_t label_c = kuf::menu_pointer::FindInFocusedScene(kuf::ui::kLobbyLabelText + slot);
    const uint32_t value_c = kuf::menu_pointer::FindInFocusedScene(kuf::ui::kLobbyValueText + slot);
    const bool columns = label_c && value_c;
    if (slot < first_row) {  // Home's row 0: the game's own Difficulty row
      if (columns) {
        SetText(ctx, base, label_c, "");
        SetText(ctx, base, value_c, "");
      }
      continue;
    }
    const uint32_t button = Load32(base, menu + kRowButtons + 4 * slot);
    const uint32_t twin = Load32(base, menu + kRowTwins + 4 * slot);
    const size_t index = v.top + (slot - first_row);
    if (index >= v.rows.size()) {
      Flag(ctx, base, button, kFlagShown, false);
      Flag(ctx, base, twin, kFlagShown, false);
      if (columns) {
        SetText(ctx, base, label_c, "");
        SetText(ctx, base, value_c, "");
        SetLook(base, label_c, false);
        SetLook(base, value_c, false);
      }
      continue;
    }
    const MenuRow& r = v.rows[index];
    const bool selected = !g.difficulty && index == v.selected;
    if (columns) {
      for (const auto& [c, value_column] : {std::pair{label_c, false}, std::pair{value_c, true}}) {
        const bool look = REXCVAR_GET(kuf_lobby_flash) && kuf::coop::SelectedLook(selected, v.editing, value_column);
        SetLook(base, c, look);
      }
      const kuf::coop::RowText t = kuf::coop::SplitRow(r, kValueChars);
      SetText(ctx, base, button, t.button);  // the button: its frame, highlight and hit box (text only for actions)
      SetText(ctx, base, twin, t.button);
      SetText(ctx, base, label_c, t.label);
      SetText(ctx, base, value_c, t.value);
    } else {
      const std::string text = Label(r, selected && v.editing);
      SetText(ctx, base, button, text);
      SetText(ctx, base, twin, text);
    }
    Flag(ctx, base, button, kFlagShown, !selected);
    Flag(ctx, base, twin, kFlagShown, selected);
    Flag(ctx, base, button, kFlagUnavailable, !r.enabled || r.kind == MenuRow::kInfo);
    Flag(ctx, base, twin, kFlagUnavailable, !r.enabled);
  }
  // The description: the result of the last action, else the selected row's short description; while a field is
  // edited, the editing hint.
  std::string note = v.message;
  if (note.empty() && !g.difficulty && v.selected < v.rows.size()) note = v.rows[v.selected].note;
  if (v.editing) note = v.help;
  if (!note.empty() || !home) SetText(ctx, base, Load32(base, menu + kDescription), note);
}

// One of the game's own steps: its row `row` selected and Select (or Back) given to its own handler.
void GameStep(PPCContext& ctx, uint8_t* base, uint32_t menu, int row, uint32_t code) {
  if (!g.event) {
    g.event = Call(ctx, base, sub_8218F410, 64);
    if (!g.event) return;
  }
  std::memset(base + g.event, 0, 64);
  Store32(base, g.event + kEventMessage, kMessageInput);
  Store32(base, g.event + kEventCode, code);
  if (row >= 0) {
    Store32(base, menu + kSelected, uint32_t(row));
    Call(ctx, base, sub_82667780, menu);
  }
  g.drawn.clear();
  Call(ctx, base, __imp__sub_8266A3F8, menu, g.event);
}

void Apply(PPCContext& ctx, uint8_t* base, uint32_t menu, const MenuEffect& e) {
  switch (e.action) {
    case Action::kSolo:
      REXLOG_INFO("[kuf-lobby] Continue Solo Game: the game's single-player row");
      GameStep(ctx, base, menu, kRowSolo, kCodeSelect);
      break;
    case Action::kLeave:
      GameStep(ctx, base, menu, -1, kCodeBack);
      break;
    case Action::kHostGame:
      REXLOG_INFO("[kuf-lobby] Start Game: the game's Host {} row", e.public_session ? "Public" : "Private");
      GameStep(ctx, base, menu, e.public_session ? kRowHostPublic : kRowHostPrivate, kCodeSelect);
      break;
    case Action::kJoinGame:
      REXLOG_INFO("[kuf-lobby] joining the host's game: the game's Quick Match row");
      GameStep(ctx, base, menu, kRowQuickMatch, kCodeSelect);
      break;
    case Action::kCopy:
      if (auto* d = kuf::input::KufInputDriver::Instance()) d->CopyToClipboard(e.text);
      break;
    case Action::kNone:
      break;
  }
}

void Sync() {  // the rows a page has on screen: Home five (under the Difficulty row), the others six
  LobbyMenu& m = Menu();
  m.SetVisible(m.Page() == MenuPage::kHome ? kPageRows : kRows);
}
// The game's Difficulty text (written by the game into row 0; read until found), markup removed.
std::string Plain(std::string t) {
  for (size_t at; (at = t.find("@(")) != std::string::npos;) {
    const size_t end = t.find(')', at);
    t.erase(at, end == std::string::npos ? std::string::npos : end - at + 1);
  }
  if (t.rfind("Difficulty", 0) == 0) t.erase(0, 10);  // the row reads "Difficulty ... <name>": the name
  while (!t.empty() && (t.front() == ' ' || t.front() == ':' || t.front() == '\t')) t.erase(0, 1);
  return t;
}

// The keyboard while a field is edited: text entry on (the platform's text input, every key the field's), its events
// to the model; off again as soon as the edit ends.
void TakeText() {
  auto* driver = kuf::input::KufInputDriver::Instance();
  if (!driver) return;
  LobbyMenu& m = Menu();
  if (m.Editing() != driver->TextEntryActive()) {
    if (m.Editing()) {
      driver->BeginTextEntry();
    } else {
      driver->EndTextEntry();
    }
  }
  if (!m.Editing()) return;
  const auto events = driver->TakeTextEntry();
  if (events.empty()) return;
  if (REXCVAR_GET(kuf_trace_text_input)) {
    REXLOG_INFO("[kuf-text] {} text event(s) delivered to the field (contents not logged)", events.size());
  }
  const MenuEffect e = m.Text(events);
  if (e.action == Action::kCopy) {
    driver->CopyToClipboard(e.text);
  }
}

}  // namespace

bool Active() { return REXCVAR_GET(kuf_lobby_menus) && OnScreen(); }

bool IsLayout(const std::string& path) {
  static const std::string kName = "\\uistartgame.uitb";
  return REXCVAR_GET(kuf_lobby_menus) && path.size() >= kName.size() &&
         path.compare(path.size() - kName.size(), kName.size(), kName) == 0;
}

bool ExtendLayout(std::vector<uint8_t>* data, std::string* why) {
  auto out = kuf::ui::AddLobbyColumns(*data, why);
  if (!out) return false;
  *data = std::move(*out);
  return true;
}

bool RowZeroIsPageRow() { return Active() && g.menu && g.menu->Page() != MenuPage::kHome; }

bool Editing() { return Active() && g.menu && g.menu->Editing(); }

void ClickWhileEditing(PPCContext& ctx, uint8_t* base, int32_t x, int32_t y) {
  if (!Editing() || !Live(g.screen)) return;
  LobbyMenu& m = Menu();
  const bool home = m.Page() == MenuPage::kHome;
  const uint32_t first_row = home ? 1 : 0;
  const MenuView v = m.View();
  for (uint32_t slot = first_row; slot < kRows; ++slot) {
    const uint32_t c = Load32(base, g.screen + kRowButtons + 4 * slot);
    const int32_t cx = int32_t(Load32(base, c + 16)), cy = int32_t(Load32(base, c + 20));
    const int32_t cw = int32_t(Load32(base, c + 24)), ch = int32_t(Load32(base, c + 28));
    if (x < cx || x >= cx + cw || y < cy || y >= cy + ch) continue;
    const size_t index = v.top + (slot - first_row);
    if (index >= v.rows.size()) break;
    const MenuEffect e = m.Select(index);  // ends the edit (kept), then that row as a click
    Sync();
    Draw(ctx, base, g.screen, false);
    Apply(ctx, base, g.screen, e);
    return;
  }
  m.Text({{true, kuf::input::TextKey::kEnter, {}}});  // a click outside the rows: the edit is done
  Draw(ctx, base, g.screen, false);
}

void TraceFlash(const uint8_t* base);

void OnTick(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(kuf_lobby_menus)) return;
  g.base = base;
  if (!OnScreen()) {
    if (g.on_screen) {  // the screen closed: an edit ends with it (nothing typed later goes anywhere)
      g.on_screen = false;
      g.fonts.clear();  // its controls are gone
      g.update_logged = false;
      if (g.menu && g.menu->Editing()) g.menu->Text({{true, kuf::input::TextKey::kEscape, {}}});
      TakeText();
    }
    return;
  }
  bool first = !g.on_screen;
  if (first || !Live(g.screen)) {
    // The screen's menu object: the input handler records it; when the screen opens, an event the handler ignores
    // is given to the focused control (as the pointer's steps are), which brings it there.
    g.screen = 0;
    kuf::menu_pointer::DispatchToFocused(ctx, base, kProbeCode);
    if (!Live(g.screen)) {
      if (!g.probe_failed) REXLOG_WARN("[kuf-lobby] the game type screen's menu object was not found (probe)");
      g.probe_failed = true;
      return;
    }
    REXLOG_INFO("[kuf-lobby] the game type screen: menu object {:08X}", g.screen);
    first = true;
  }
  g.on_screen = true;
  const uint32_t menu = g.screen;
  LobbyMenu& m = Menu();
  if (first) {
    m.Open();
    g.difficulty = false;
    g.difficulty_name.clear();
    g.difficulty_logged = false;
  }
  if (g.difficulty_name.empty() && m.Page() == MenuPage::kHome) {
    // The lobby is created with the game's own difficulty (it follows the character): its text, once the game wrote it.
    for (uint32_t f : {kRowButtons, kRowTwins}) {
      if (g.difficulty_name.empty()) g.difficulty_name = Plain(TextOf(base, Load32(base, menu + f)));
    }
    if (g.difficulty_name.empty() && !g.difficulty_logged) {
      g.difficulty_logged = true;  // once per screen: what row 0 holds (the difficulty's text was not found in it)
      REXLOG_INFO("[kuf-lobby] row 0 texts: '{}' / '{}' (difficulty index {})",
                  TextOf(base, Load32(base, menu + kRowButtons)), TextOf(base, Load32(base, menu + kRowTwins)),
                  Load32(base, menu + kDifficulty));
    }
    if (!g.difficulty_name.empty()) {
      m.SetGameDifficulty(uint8_t(Load32(base, menu + kDifficulty)), g.difficulty_name);
      REXLOG_INFO("[kuf-lobby] the game's difficulty: '{}' ({})", g.difficulty_name, Load32(base, menu + kDifficulty));
    }
  }
  Sync();
  TakeText();
  MenuEffect e;
  if (g.pending) {  // the model ticks in Tick(); its game steps are made here, on the screen
    e = *g.pending;
    g.pending.reset();
  }
  // The game's own update may change the rows' states (its co-op poll): drawn again twice a second regardless (an
  // edited field's blinking caret changes its text as well).
  Draw(ctx, base, menu, first || (++g.ticks % 15) == 0);
  Apply(ctx, base, menu, e);
  TraceFlash(base);
}

// --kuf_trace_selection_flash: the selected control's draw fields over time (colour +352..+364 floats, the per-state
// objects +288..+296, the per-state values +312..+328, +372), the selected row's label text beside it.
void TraceFlash(const uint8_t* base) {
  if (!REXCVAR_GET(kuf_trace_selection_flash)) return;
  static uint32_t last_focus = 0, ticks = 0;
  using kuf::menu_pointer::GuestReadable;
  constexpr uint32_t kFocused = 0x82ABBB90;
  if (!GuestReadable(kFocused, 4)) return;
  const uint32_t focus = Load32(base, kFocused);
  if (focus != last_focus) {
    last_focus = focus;
    ticks = 60;  // 2 s
  }
  if (ticks == 0 || (ticks-- % 2) != 0 || !GuestReadable(focus, 400)) return;
  auto f = [&](uint32_t c, uint32_t at) {
    uint32_t v = Load32(base, c + at);
    float x;
    std::memcpy(&x, &v, 4);
    return x;
  };
  auto words = [&](uint32_t c, uint32_t from, uint32_t n) {
    std::string out;
    for (uint32_t i = 0; i < n; ++i) out += fmt::format(" {:08X}", Load32(base, c + from + 4 * i));
    return out;
  };
  std::string label;
  if (g.menu && g.screen) {
    const MenuView v = g.menu->View();
    const uint32_t slot = uint32_t(v.selected - v.top) + (v.page == MenuPage::kHome ? 1 : 0);
    const uint32_t lc = kuf::menu_pointer::FindInFocusedScene(kuf::ui::kLobbyLabelText + slot);
    if (lc && GuestReadable(lc, 400)) {
      label = fmt::format("; label {:08X} rgba {:.3f} {:.3f} {:.3f} {:.3f} st {:X} +288{} +312{}", Load32(base, lc + 8),
                          f(lc, 352), f(lc, 356), f(lc, 360), f(lc, 364), Load32(base, lc + 56), words(lc, 288, 3),
                          words(lc, 312, 6));
    }
  }
  REXLOG_INFO("[kuf-flash] focus {:08X} rgba {:.3f} {:.3f} {:.3f} {:.3f} st {:X} b372 {} +288{} +312{}{}",
              Load32(base, focus + 8), f(focus, 352), f(focus, 356), f(focus, 360), f(focus, 364),
              Load32(base, focus + 56), base[focus + 372], words(focus, 288, 3), words(focus, 312, 6), label);
}

void Tick() {
  // Every tick, on the screen or not: a host's start completes (or fails) after the game left the screen; a game
  // step the model asks for waits until the screen is shown again.
  if (!REXCVAR_GET(kuf_lobby_menus) || !g.menu) return;
  const MenuEffect e = g.menu->Tick();
  if (e.action != Action::kNone) g.pending = e;
}

}  // namespace kuf::lobby_screen

// The screen's input: all of it to the model while the menus are on.

REX_HOOK_RAW(sub_8266A3F8) {
  using namespace kuf::lobby_screen;
  const uint32_t menu = ctx.r3.u32, event = ctx.r4.u32;
  g.base = base;
  if (!REXCVAR_GET(kuf_lobby_menus) || !Live(menu) || !kuf::menu_pointer::GuestReadable(event, 16)) {
    __imp__sub_8266A3F8(ctx, base);
    return;
  }
  g.screen = menu;
  LobbyMenu& m = Menu();
  Sync();
  const uint32_t code = Load32(base, event + kEventCode);
  const bool home = m.Page() == MenuPage::kHome;
  // Home: row 0 is the game's own Difficulty (Left / Right go to the game).
  if (home && g.difficulty) {
    if (code == kCodeDown) {
      g.difficulty = false;
    } else if (code == kCodeLeft || code == kCodeRight) {
      __imp__sub_8266A3F8(ctx, base);
      g.drawn.clear();
    } else if (code == kCodeBack) {
      Apply(ctx, base, menu, m.Input(MenuInput::kBack));
      return;
    }
    Draw(ctx, base, menu, false);
    ctx.r3.u64 = 0;
    return;
  }
  if (home && code == kCodeUp && m.View().selected == 0) {
    g.difficulty = true;
    Draw(ctx, base, menu, false);
    ctx.r3.u64 = 0;
    return;
  }
  MenuInput in;
  switch (code) {
    case kCodeSelect: in = MenuInput::kSelect; break;
    case kCodeBack: in = MenuInput::kBack; break;
    case kCodeUp: in = MenuInput::kUp; break;
    case kCodeDown: in = MenuInput::kDown; break;
    case kCodeLeft: in = MenuInput::kLeft; break;
    case kCodeRight: in = MenuInput::kRight; break;
    default:
      ctx.r3.u64 = 0;
      return;  // other codes (shoulders, ..., the tick's probe) do nothing here
  }
  const MenuEffect e = m.Input(in);
  Sync();
  TakeText();  // an edit begun here takes the keyboard at once (not at the next tick)
  Draw(ctx, base, menu, false);
  Apply(ctx, base, menu, e);
  ctx.r3.u64 = 0;
}

// The screen's own per-tick update: only observed (the first two runs showed that the menus' per-tick work never ran
// from here; its object is logged once per screen to find out why). The per-tick work is OnTick.
REX_HOOK_RAW(sub_82669010) {
  using namespace kuf::lobby_screen;
  const uint32_t menu = ctx.r3.u32;
  __imp__sub_82669010(ctx, base);
  if (!REXCVAR_GET(kuf_lobby_menus) || !OnScreen() || g.update_logged) return;
  g.update_logged = true;
  REXLOG_INFO("[kuf-lobby] the screen's update runs with {:08X} (the input handler's {:08X}; rows readable: {})", menu,
              g.screen, Live(menu));
}

#else

namespace kuf::lobby_screen {
bool Active() { return false; }
bool IsLayout(const std::string&) { return false; }
bool ExtendLayout(std::vector<uint8_t>*, std::string*) { return false; }
bool RowZeroIsPageRow() { return false; }
bool Editing() { return false; }
void ClickWhileEditing(PPCContext&, uint8_t*, int32_t, int32_t) {}
void OnTick(PPCContext&, uint8_t*) {}
void Tick() {}
}  // namespace kuf::lobby_screen

#endif
