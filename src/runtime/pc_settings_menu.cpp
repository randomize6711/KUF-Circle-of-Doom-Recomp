// PC Settings in the game's own Settings menus (M6B; the M6A proof of concept made real), both the in-game
// Settings (options menu) and the title screen's SETTINGS. docs/SETTINGS.md shows the pages;
// docs/MENU_ARCHITECTURE.md the menu internals.
//
// Layout: the game reads each menu layout file with sub_82190C60(path, &buffer, &size) and parses it right after.
// For d:\data\Menu\UiProfileOption.uitb and UiTitle.uitb the buffer is replaced by an extended copy with one more
// row, "PC Settings" (src/runtime/ui_layout.cpp), allocated and freed with the game's own allocator. The game
// files are never changed; the game creates, draws and destroys the row's controls itself.
//
// The two Settings menus are separate code with the same structure (Spec below): eight value rows (four
// sliders: SFX, Music, Voice, Brightness; the skin selector; three toggles: Camera, Blood, Vibration) in the
// menu object, the selected row index, the highlight button, and the functions that show the selected row,
// draw a slider and draw the selector; the title adds Change Storage Device as a ninth row. The selected toggle
// half's +72 points at the highlight.
//
//   Settings page:  Down on the last row selects PC Settings (the original highlight code with that row's
//                   button in a row slot, the original cursor sound); Select opens the PC pages; Back leaves
//                   Settings through the original code.
//   PC pages (M7):  a category page (Controls, Camera, Display, Frame Rate; the PC Settings row becomes Restore
//                   Defaults) and one page per category, drawn in the game's own eight rows (sliders in slots 0-3;
//                   other values shown in the row labels with the widgets hidden). Select opens a category, Back
//                   goes back to the categories and from there to the Settings page, which shows its own labels and
//                   values again and writes pc_settings.toml if something changed. Values change at once
//                   (pc_settings.h); all input on these pages is handled here; the original handler (and with it
//                   the game's own options and their save) is not involved. In the title, Change Storage Device is
//                   hidden meanwhile.
//
//   Frame rate:     FPS (30 (Original) ... Unlimited), Interpolation, World Objects, Animation, Effects (M6D D5);
//                   each change takes effect within a tick, without a restart (frame_pacing.cpp).
//
// Text: the row labels are the port's own (English; markup as the original labels). With
// --kuf_pc_settings_menu=false nothing here runs.

#include <algorithm>
#include <functional>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>

#include "input/kuf_input_driver.h"
#include "input/pad_remap_driver.h"
#include "runtime/controls_screen.h"
#include "runtime/display.h"
#include "runtime/frame_pacing.h"
#include "runtime/lobby_screen.h"
#include "runtime/menu_mouse.h"
#include "runtime/menu_pointer.h"
#include "runtime/pc_settings.h"
#include "runtime/pc_settings_menu.h"
#include "runtime/prompt_text.h"
#include "runtime/title_menu.h"
#include "runtime/ui_layout.h"
#include "runtime/version.h"

REXCVAR_DECLARE(bool, kuf_trace_menu_pointer);
REXCVAR_DEFINE_BOOL(kuf_storage_row, false, "KUF/Settings",
                    "M7 UX fix 5: show the console's Change Storage Device row in the title SETTINGS (the PC port saves "
                    "in its own folder; false: the row is moved off screen and PC Settings takes its place)");
REXCVAR_DEFINE_BOOL(kuf_descriptions, true, "KUF/Settings",
                    "M7: the description of the selected row on the PC Settings pages");
REXCVAR_DEFINE_BOOL(kuf_settings_compact_rows, true, "KUF/Settings",
                    "M7 menu overhaul: a Settings page's rows one under the other (the game's own rows moved up "
                    "from their slot)");
REXCVAR_DEFINE_INT32(kuf_description_font, 35, "KUF/Debug",
                     "M7 UI polish: the font preset of the PC Settings descriptions (35: the small left-aligned one)");
REXCVAR_DEFINE_BOOL(kuf_pc_settings_menu, true, "KUF/Settings",
                    "PC Settings in the game's Settings menus (off: the original menus exactly)");

REX_EXTERN(__imp__sub_82190C60);
REX_EXTERN(sub_82624570);  // control lookup (node, id)
REX_EXTERN(sub_82624CB8);  // set a control's text (control, string, 0): the control keeps its own copy
REX_EXTERN(sub_82623850);  // control state flags (control, flag, on): 1 shown; 16 unavailable
REX_EXTERN(sub_8261B488);  // menu sound cue (menu, cue)
REX_EXTERN(sub_8218F410);  // the game's allocation (used by sub_82190C60 for the file buffer)
REX_EXTERN(sub_8218F7F8);  // the matching release (the layout loader frees the buffer with it)
REX_EXTERN(sub_82661CD0);  // in-game Settings: show the selected row (highlight, states)
REX_EXTERN(sub_82661DA0);  // in-game Settings: draw the selected row's slider (menu, f1 = value 0..1)
REX_EXTERN(sub_82661E80);  // in-game Settings: draw the skin selector
REX_EXTERN(__imp__sub_82663FA0);  // in-game options menu: its event handler (menu, scene, event, r6)
REX_EXTERN(__imp__sub_82678280);  // title SETTINGS: its input handler (menu, event)
REX_EXTERN(sub_82676C38);  // title SETTINGS: show the selected row (and its button help)
REX_EXTERN(sub_82676AB8);  // title SETTINGS: draw the selected row's slider (menu, f1 = value 0..1)
REX_EXTERN(sub_82676B98);  // title SETTINGS: draw the skin selector

namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);

// One Settings menu: offsets in its menu object and its functions.
struct Spec {
  const char* name;
  std::string_view layout;   // layout file name (lower case, with the leading backslash)
  uint32_t scene;            // menu offset of the Settings scene instance
  uint32_t rows;             // the row buttons (eight value rows, then the title's Change Storage Device)
  uint32_t selected;         // the selected row index
  uint32_t highlight;        // the highlight button
  uint32_t toggles;          // the six toggle halves (rows 5, 6, 7: first / second)
  uint32_t selector_text, arrow_left, arrow_right;
  uint32_t slider_values[4];  // rows 0-3 (floats 0..1)
  uint32_t slider_fill, slider_knob;  // rows 0-3: the slider's two controls (+4 per row; the draw function's)
  GuestFn show_row, draw_slider, draw_selector;
  uint32_t pc_button_id;     // the PC Settings row's button (the layout clone)
  uint32_t extra_slot;       // the row slot the PC Settings row is shown through (index = row number)
  bool has_storage_row;      // title: row 8 is Change Storage Device (selectable only with a storage device)
  uint32_t help_text_id;     // in-game: the button help control (0: the menu's own code sets the help)
  // Layout IDs (not in the menu object): each row's frame (six images from this ID: the frame's left / middle /
  // right and the label button's), rows 0-7, then the title's Change Storage Device (0: none) and the PC
  // Settings row's clone (src/runtime/ui_layout.cpp); the four sliders' tracks.
  uint32_t row_frames[10];
  uint32_t slider_tracks[4];
  uint32_t description_id;   // M7 UX fix 4: the description text (a clone of the help line, kDescriptionText)
  uint32_t description_source;  // the help line cloned for it
  int32_t help_y;            // the button help line's top in the layout (the description ends above it)
};

const Spec kInGame = {"in-game Settings", "\\uiprofileoption.uitb", 44, 64, 164, 60, 128, 152, 156, 160,
                      {176, 172, 180, 184}, 96, 112, sub_82661CD0, sub_82661DA0, sub_82661E80, kuf::ui::kPcRowButton,
                      7, false, 0x2704001,
                      {0x270500A, 0x2705010, 0x2705016, 0x270502E, 0x270501C, 0x2705037, 0x270503D, 0x2705045, 0,
                       0x2705080},
                      {0x2705022, 0x2705025, 0x2705028, 0x2705034}, 0x2704080, 0x2704001, 563};
const Spec kTitle = {"title SETTINGS", "\\uititle.uitb", 44, 160, 268, 156, 228, 252, 256, 260,
                     {280, 276, 284, 288}, 196, 212, sub_82676C38, sub_82676AB8, sub_82676B98, kuf::ui::kTitlePcRowButton,
                     8, true, 0,
                     {0x410500A, 0x4105010, 0x4105016, 0x410502B, 0x410501C, 0x4105034, 0x410503A, 0x410504B, 0x4105043,
                      0x4105080},
                     {0x4105022, 0x4105025, 0x4105028, 0x4105031}, 0x4104080, 0x4104002, 634};

// The title SETTINGS' version line (M7): a copy of its button help line (right-aligned, the same font, smaller), just
// above it in the panel's bottom-right corner; the descriptions end above it.
constexpr uint32_t kVersionText = 0x4104082;
constexpr int32_t kVersionY = 610, kVersionHeight = 24;
// The scrollbar's track and thumb (layout copies; +1 the thumb).
constexpr uint32_t ScrollTrackId(const Spec& spec) { return (spec.slider_tracks[0] & 0xFFFFF000) | 0x090; }
constexpr uint32_t kLabelText = 0xFF00;  // not in the game's text tables (they end at 1910)
constexpr int32_t kDescriptionGap = 8;      // between the list (above) and the button help (below) and the description
constexpr int32_t kDescriptionIndent = 12;  // the description's left margin from the rows' labels
constexpr uint32_t kSetRect = 0x82624018;  // the toolkit's base SetRect (control, x, y, width, height)
constexpr uint32_t kDescriptionFont = 35;  // the small left-aligned font of the Controls page's second lines
constexpr uint32_t kTitleSceneId = 0x04100000;  // SN_TITLE_OPTION
constexpr int32_t kExtraSelected = 100;  // ShowRowsFor: the Restore Defaults row (the PC Settings row) is selected
constexpr int32_t kOffScreen = 4000;            // a placement this far down is never drawn or under the pointer
constexpr char kLabel[] = "@(offsety=-2)PC Settings";
constexpr uint32_t kEventMessage = 4, kEventCode = 12, kMessageInput = 1100;
constexpr uint32_t kCodeSelect = 0, kCodeBack = 1, kCodeUp = 6, kCodeDown = 7, kCodeLeft = 8, kCodeRight = 9;
constexpr uint32_t kRows = 8, kLastRow = 7, kStorageRow = 8, kToggleSelected = 72, kButtonText = 84;
constexpr uint32_t kControlState = 56, kStateUnavailable = 0x10;
constexpr uint32_t kCueMove = 4, kCueSelect = 0xB, kCueBack = 0xF;

constexpr char kDefaultsLabel[] = "@(offsety=-2)Restore Defaults";
// --- The PC pages (M7 feature 3: a category page and one page per category) ---------------------------------------
//
// PC Settings opens the category page: Controls, Camera, Display, Frame Rate (label rows) and Restore Defaults (the
// PC Settings row). Select opens a category's page with its first row selected; Back goes from a category page to the
// category page (that category selected) and from there to the game's Settings page (the file written if changed).
// Each page uses some of the game's eight row slots: a slot's widget is fixed by the game's layout (sliders in
// slots 0-3, the selector in slot 4, toggle halves in slots 5-7), so a page puts each setting in a slot with its
// widget, or shows the value in the label with the widget hidden. Up/Down move over the page's rows only.

using kuf::pc::CameraSettings;

// M7 menu overhaul: Settings opens on the categories (no separate PC Settings): Display, Graphics, Camera, Audio,
// Mouse & Keyboard, Controller, Gameplay, then Restore Defaults. The game's own eight options live in them as the
// game's own rows (Kind::kGame: the game's label, widget and input handling in their own slot): SFX, Music, Voice
// (Audio), Brightness (Graphics), Camera Normal / Inverted and Vibration (Controller), Skin and Blood (Gameplay); the
// game saves them as before when Settings closes.
enum class Page : int { kCategories, kDisplay, kCamera, kAudio, kMouseKeys, kController, kGameplay, kKeys,
                        kButtons };
// A slider (slots 0-3), the value in the label (the widget hidden), the game's own row, or a row opening a page.
enum class Kind { kSlider, kLabel, kGame, kLink };
// What a row shows (the PC settings; kGameRow: the game's own option of its slot).
enum class Item { kNone, kMouseH, kMouseV, kMouseY, kStickSens, kDeadzone, kPrompts, kDistance, kFov, kVRange,
                  kRecenter, kModern, kLinkKeys, kLinkButtons, kGameRow };
struct RowDef {
  uint32_t slot;
  Kind kind;
  Item item = Item::kNone;
};

constexpr RowDef kCategoryRows[] = {{0, Kind::kLabel}, {1, Kind::kLabel}, {2, Kind::kLabel},
                                    {3, Kind::kLabel}, {4, Kind::kLabel}, {5, Kind::kLabel}};
constexpr const char* kCategoryLabels[] = {"@(offsety=-2)Display",          "@(offsety=-2)Camera",
                                           "@(offsety=-2)Audio",            "@(offsety=-2)Mouse & Keyboard",
                                           "@(offsety=-2)Controller",       "@(offsety=-2)Gameplay"};
constexpr Page kCategoryPages[] = {Page::kDisplay,   Page::kCamera,     Page::kAudio,
                                   Page::kMouseKeys, Page::kController, Page::kGameplay};
// Camera: distance and field of view (sliders, the value also in the label), the vertical range, recentering and the
// modern camera (values in the labels).
constexpr RowDef kCameraRows[] = {{0, Kind::kSlider, Item::kDistance}, {1, Kind::kSlider, Item::kFov},
                                  {2, Kind::kLabel, Item::kVRange},    {3, Kind::kLabel, Item::kRecenter},
                                  {4, Kind::kLabel, Item::kModern}};
// Audio: the game's SFX, Music and Voice volumes.
constexpr RowDef kAudioRows[] = {{0, Kind::kGame, Item::kGameRow}, {1, Kind::kGame, Item::kGameRow},
                                 {2, Kind::kGame, Item::kGameRow}};
// Mouse & Keyboard: the mouse's sensitivities (sliders), its Y axis, the Key Bindings page.
constexpr RowDef kMouseKeysRows[] = {{0, Kind::kSlider, Item::kMouseH}, {1, Kind::kSlider, Item::kMouseV},
                                     {2, Kind::kLabel, Item::kMouseY}, {3, Kind::kLink, Item::kLinkKeys}};
// Controller: the stick's sensitivity and dead zone (sliders), the Controller Buttons page, the game's Camera
// (vertical axis, slot 5) and Vibration (slot 7).
constexpr RowDef kControllerRows[] = {{0, Kind::kSlider, Item::kStickSens}, {1, Kind::kSlider, Item::kDeadzone},
                                      {2, Kind::kLink, Item::kLinkButtons}, {5, Kind::kGame, Item::kGameRow},
                                      {7, Kind::kGame, Item::kGameRow}};
// Gameplay: the button prompts, the game's Skin (slot 4) and Blood (slot 6).
constexpr RowDef kGameplayRows[] = {{0, Kind::kLabel, Item::kPrompts}, {4, Kind::kGame, Item::kGameRow},
                                    {6, Kind::kGame, Item::kGameRow}};
// Display (M6C; M7 UX fix 3 adds VSync and the frame rate; the UI polish moves Apply last; Render to Graphics in the
// menu overhaul): a list of 11 items, eight
// rows at a time (scrolling as Key Bindings), values in the labels: Mode, Size, Monitor (these three take effect with
// Apply and its confirmation), Widescreen and VSync (at once), the frame rate (M6D D5: FPS, Interpolation and the
// advanced World Objects, Animation, Effects; each at once, within a tick, frame_pacing.cpp), Apply (greyed while
// Mode, Size and Monitor are as in effect). Left / Right step; Select steps forward and
// wraps (the mouse: hover and click), on Apply applies / keeps.
constexpr uint32_t kRowMode = 0, kRowSize = 1, kRowMonitor = 2, kRowWidescreen = 3, kRowVsync = 4, kItemFps = 5,
                   kItemEffects = 9, kRowApply = 10;
constexpr uint32_t kRowRender = 100;      // Render (the render resolution)
constexpr uint32_t kRowBrightness = 101;  // the game's own Brightness (its slider slot 3; changed by the game's code)
// M7 UI follow-up: Display holds every display and graphics option (no Graphics page): the display setup (Mode,
// Resolution, Monitor, Widescreen), then rendering and the frame rate, Apply last; no heading rows (the player: they
// read as empty rows). The list's entries by position (the items above keep their numbers: descriptions, labels and
// changes are by item).
constexpr uint32_t kDisplayList[] = {kRowMode,     kRowSize,     kRowMonitor,  kRowWidescreen, kRowVsync,
                                     kItemFps + 0, kItemFps + 1, kItemFps + 2, kItemFps + 3,   kItemFps + 4,
                                     kRowRender,   kRowBrightness, kRowApply};
constexpr uint32_t kDisplayItems = std::size(kDisplayList);
constexpr uint32_t kBrightnessSlot = 3;  // the game's Brightness row (its slider)
constexpr int kConfirmSeconds = 15;
// The frame-rate rows within their own group (FrameRateLabel, ChangeFrameRateValue): item - kItemFps.
constexpr uint32_t kRowFps = 0, kRowInterpolation = 1, kRowObjects = 2, kRowAnimation = 3, kRowEffects = 4;
// Key Bindings and Controller Buttons (M7 feature 5): one row per gameplay action, eight at a time (Up / Down past
// the first or last visible row scroll the list); Select waits for the key or button (capture, ended by OnTick).
constexpr RowDef kListRows[] = {{0, Kind::kLabel}, {1, Kind::kLabel}, {2, Kind::kLabel}, {3, Kind::kLabel},
                                {4, Kind::kLabel}, {5, Kind::kLabel}, {6, Kind::kLabel}, {7, Kind::kLabel}};
using kuf::input::Control;
using kuf::input::PadButton;
struct KeyAction {
  Control control;
  const char* name;
};
constexpr KeyAction kKeyActions[] = {
    {Control::kLeftStickUp, "Forward"},       {Control::kLeftStickDown, "Backward"},
    {Control::kLeftStickLeft, "Left"},        {Control::kLeftStickRight, "Right"},
    {Control::kA, "Weapon A"},                {Control::kX, "Weapon X"},
    {Control::kB, "Dash"},                    {Control::kY, "Pick Up"},
    {Control::kLeftShoulder, "Item L"},       {Control::kRightShoulder, "Item R"},
    {Control::kLeftTrigger, "Aim"},           {Control::kRightTrigger, "Ability"},
    {Control::kStart, "Status"},              {Control::kBack, "Options"},
    {Control::kRightThumb, "Centre View"},    {Control::kLeftThumb, "L Stick"},
    {Control::kRightStickUp, "Look Up"},      {Control::kRightStickDown, "Look Down"},
    {Control::kRightStickLeft, "Look Left"},  {Control::kRightStickRight, "Look Right"},
    {Control::kDpadUp, "D-Pad Up"},           {Control::kDpadDown, "D-Pad Down"},
    {Control::kDpadLeft, "D-Pad Left"},       {Control::kDpadRight, "D-Pad Right"},
};
struct ButtonAction {
  PadButton button;
  const char* name;
};
constexpr ButtonAction kButtonActions[] = {
    {PadButton::kA, "Weapon A"},           {PadButton::kX, "Weapon X"},          {PadButton::kB, "Dash"},
    {PadButton::kY, "Pick Up"},            {PadButton::kLeftShoulder, "Item L"}, {PadButton::kRightShoulder, "Item R"},
    {PadButton::kLeftTrigger, "Aim"},      {PadButton::kRightTrigger, "Ability"}, {PadButton::kStart, "Status"},
    {PadButton::kBack, "Options"},         {PadButton::kRightThumb, "Centre View"},
    {PadButton::kLeftThumb, "L Stick"},
};
constexpr int kCaptureSeconds = 10;
constexpr size_t kLabelChars = 20;  // longer labels wrap
constexpr int kOriginalFovDegrees = 58;  // shown for the game's own field of view (57.6 rounded)
constexpr char kActionHelp[] = "@(Dpad_Small)Move  @(A)Select  @(B)Back";

std::span<const RowDef> RowsOf(Page page) {
  switch (page) {
    case Page::kCategories: return kCategoryRows;
    case Page::kCamera: return kCameraRows;
    case Page::kAudio: return kAudioRows;
    case Page::kMouseKeys: return kMouseKeysRows;
    case Page::kController: return kControllerRows;
    case Page::kGameplay: return kGameplayRows;
    default: return kListRows;  // Display, Key Bindings, Controller Buttons
  }
}

// The number of actions a list page scrolls over (0: not a list page).
uint32_t ListCount(Page page) {
  return page == Page::kKeys      ? uint32_t(std::size(kKeyActions))
         : page == Page::kButtons ? uint32_t(std::size(kButtonActions))
         : page == Page::kDisplay ? kDisplayItems
                                  : 0;
}

const char* PageName(Page page) {
  switch (page) {
    case Page::kCategories: return "categories";
    case Page::kDisplay: return "display";
    case Page::kCamera: return "camera";
    case Page::kAudio: return "audio";
    case Page::kMouseKeys: return "mouse and keyboard";
    case Page::kController: return "controller";
    case Page::kGameplay: return "gameplay";
    case Page::kKeys: return "key bindings";
    default: return "controller buttons";
  }
}

// The page a page's Back returns to, and the row selected there.
Page ParentOf(Page page) {
  return page == Page::kKeys ? Page::kMouseKeys : page == Page::kButtons ? Page::kController : Page::kCategories;
}
uint32_t RowIn(Page page, Page child) {
  if (page == Page::kCategories) {
    for (uint32_t i = 0; i < std::size(kCategoryPages); ++i) {
      if (kCategoryPages[i] == child) return i;
    }
    return 0;
  }
  const auto rows = RowsOf(page);
  for (uint32_t r = 0; r < rows.size(); ++r) {
    if ((rows[r].item == Item::kLinkKeys && child == Page::kKeys) ||
        (rows[r].item == Item::kLinkButtons && child == Page::kButtons)) {
      return r;
    }
  }
  return 0;
}

struct MenuState {
  uint32_t scene = 0;  // the Settings instance this state belongs to
  bool pc_page = false;
  bool extra = false;  // the PC Settings / Restore Defaults row has the cursor
  uint32_t return_row = kLastRow;         // the row the cursor goes back to from it
  std::array<std::string, kRows> labels;  // the original labels while the PC pages show
  std::array<bool, kRows> unavailable{};  // the original rows' unavailable state (the PC pages grey Apply)
  std::array<uint32_t, 6> toggles{};      // the original toggle halves' selection
  std::string help;                       // the original button help (in-game)
  bool help_changed = false;
  bool storage_available = false;         // the title's Change Storage Device row before the PC pages opened
  bool closing = false;  // Back on the categories: the game closes Settings (no re-entry)
  float brightness = 0;  // the game's Brightness (its slider value 0-1), read when the display page is drawn
  Page page = Page::kCategories;
  uint32_t row = 0;                       // the selected row of the page (an index into RowsOf(page))
  uint32_t top = 0;                       // a list page: the action in its first row
  // A list page waiting for a key or button (M7): the action, since when, and what OnTick needs to redraw.
  bool capturing = false;
  uint32_t capture_action = 0;
  std::chrono::steady_clock::time_point capture_start;
  uint32_t menu = 0, pc_button = 0;
  // The display page.
  kuf::pc::DisplaySettings pending;       // what its rows show (Mode, Size, Monitor take effect with Apply)
  std::vector<kuf::display::Monitor> monitors;
  bool confirming = false;                // applied, waiting for Keep
  kuf::pc::DisplaySettings loaded;        // the display in effect, as the rows show it
  kuf::pc::DisplaySettings previous;      // the display before Apply (restored unless kept)
  uint64_t confirm = 0;                   // its confirmation (g_confirm)
};

// The display confirmation: Apply starts one (a number); a timer thread restores the previous display if it is
// still the current one after kConfirmSeconds. Keep or Back end it.
std::atomic<uint64_t> g_confirm{0};
std::atomic<uint64_t> g_confirm_timed_out{0};

uint32_t g_label = 0;    // guest copy of kLabel
uint32_t g_scratch = 0;  // guest buffer for strings handed to the game's set-text (it copies them)
constexpr uint32_t kScratchSize = 256;
MenuState g_state[2];  // in-game, title
uint32_t g_ingame_menu = 0;  // the in-game options menu object seen last (BeforeSettingsEvent)

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

float LoadF(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

bool EndsWith(const uint8_t* base, uint32_t path, std::string_view suffix) {
  const char* s = reinterpret_cast<const char*>(base + path);
  const std::string_view name(s, strnlen(s, 260));
  if (name.size() < suffix.size()) {
    return false;
  }
  for (size_t i = 0; i < suffix.size(); ++i) {
    char c = name[name.size() - suffix.size() + i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != suffix[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

REX_HOOK_RAW(sub_82190C60) {  // read a whole file: (path, &buffer, &size) -> 0 on success
  const uint32_t path = ctx.r3.u32, buffer_ptr = ctx.r4.u32, size_ptr = ctx.r5.u32;
  __imp__sub_82190C60(ctx, base);
  if (!REXCVAR_GET(kuf_pc_settings_menu) || ctx.r3.u32 != 0 || size_ptr == 0) {
    return;
  }
  const bool in_game = EndsWith(base, path, kInGame.layout), title = EndsWith(base, path, kTitle.layout);
  const uint32_t buffer = Load32(base, buffer_ptr), size = Load32(base, size_ptr);
  if (!in_game && !title) {
    // M7: the level-up dialog, the equipment screen and the item list (src/runtime/menu_mouse.cpp).
    std::string name(reinterpret_cast<const char*>(base + path), strnlen(reinterpret_cast<const char*>(base + path), 260));
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    const bool lobby = kuf::lobby_screen::IsLayout(name);  // the multiplayer menus' columns (lobby_screen.cpp)
    if (!lobby && !kuf::menu_mouse::IsExtendedLayout(name)) return;
    std::vector<uint8_t> data(base + buffer, base + buffer + size);
    std::string why;
    if (!(lobby ? kuf::lobby_screen::ExtendLayout(&data, &why) : kuf::menu_mouse::ExtendLayout(name, &data, &why))) {
      if (!why.empty()) REXLOG_WARN("[kuf-mouse] {} left unchanged: {}", name, why);
      return;
    }
    ctx.r3.u64 = data.size();
    sub_8218F410(ctx, base);
    const uint32_t copy = ctx.r3.u32;
    if (copy == 0) {
      REXLOG_WARN("[kuf-mouse] {} left unchanged: no memory for {} bytes", name, data.size());
      ctx.r3.u64 = 0;
      return;
    }
    std::memcpy(base + copy, data.data(), data.size());
    Store32(base, buffer_ptr, copy);
    Store32(base, size_ptr, uint32_t(data.size()));
    ctx.r3.u64 = buffer;
    sub_8218F7F8(ctx, base);
    ctx.r3.u64 = 0;
    REXLOG_INFO("[kuf-mouse] {} extended: {} -> {} bytes ({})", name, size, data.size(),
                lobby ? "the multiplayer menus' label and value columns"
                      : "clickable footer entries; level-up: backdrop, panel");
    return;
  }
  std::string error;
  auto extended = in_game ? kuf::ui::AddSettingsActionRow(std::span(base + buffer, size), kLabelText, &error)
                          : kuf::ui::AddTitleSettingsActionRow(std::span(base + buffer, size), kLabelText, &error);
  const char* menu = in_game ? kInGame.name : kTitle.name;
  if (!extended) {
    REXLOG_WARN("[kuf-pc-menu] {} layout left unchanged: {}", menu, error);
    return;
  }
  if (title && !REXCVAR_GET(kuf_storage_row)) {  // M7 UX fix 5: Change Storage Device off screen, PC Settings up
    constexpr uint32_t kStorageButton = 0x410100D, kStorageFrame = 0x4105043, kPcFrame = 0x4105080;
    std::vector<uint32_t> storage{kStorageButton}, pc{kuf::ui::kTitlePcRowButton};
    for (uint32_t i = 0; i < 6; ++i) {
      storage.push_back(kStorageFrame + i);
      pc.push_back(kPcFrame + i);
    }
    std::string why;
    auto moved = kuf::ui::MoveControls(*extended, kTitleSceneId, storage, 0, kOffScreen, &why);
    if (moved) moved = kuf::ui::MoveControls(*moved, kTitleSceneId, pc, 0, -kuf::ui::kRowSpacing, &why);
    if (moved) {
      extended = std::move(moved);
      REXLOG_INFO("[kuf-pc-menu] title SETTINGS: Change Storage Device removed (PC Settings in its place)");
    } else {
      REXLOG_WARN("[kuf-pc-menu] title SETTINGS: Change Storage Device kept: {}", why);
    }
  }
  {  // M7: the list pages' scrollbar (track and thumb: copies of the slider's track and bar, placed by UpdateScrollbar)
    const Spec& spec = in_game ? kInGame : kTitle;
    const uint32_t track = spec.slider_tracks[0], scene_id = spec.description_id & 0xFFF00000;
    std::string why;
    auto with = kuf::ui::AddImageClones(*extended, scene_id,
                                        {{track, ScrollTrackId(spec), true, 0, kOffScreen, 14, 100},
                                         {track + 1, ScrollTrackId(spec) + 1, true, 0, kOffScreen, 10, 20}},
                                        0, &why);
    if (with) {
      extended = std::move(with);
    } else {
      REXLOG_WARN("[kuf-pc-menu] {}: no scrollbar: {}", menu, why);
    }
  }
  if (REXCVAR_GET(kuf_descriptions)) {  // M7 UX fix 4: the description text (placed per page, UpdateDescription)
    const Spec& spec = in_game ? kInGame : kTitle;
    std::string why;
    auto with = kuf::ui::AddTextClone(*extended, spec.description_id & 0xFFF00000, spec.description_source,
                                      spec.description_id, 480, 150, 600, 300,
                                      uint32_t(REXCVAR_GET(kuf_description_font)), &why);
    if (with) {
      extended = std::move(with);
    } else {
      REXLOG_WARN("[kuf-pc-menu] {}: no description text: {}", menu, why);
    }
  }
  if (title) {  // M7: the version line (EnterPcPage sets its text)
    std::string why;
    auto with = kuf::ui::AddTextClone(*extended, kTitleSceneId, kTitle.description_source, kVersionText, 92, kVersionY,
                                      1094, kVersionHeight, 0, &why);
    if (with) {
      extended = std::move(with);
    } else {
      REXLOG_WARN("[kuf-pc-menu] title SETTINGS: no version line: {}", why);
    }
  }
  if (title) {  // M7: the title list's own rows (the same file)
    std::string why;
    if (kuf::title_menu::ExtendLayout(ctx, base, &*extended, &why)) {
      REXLOG_INFO("[kuf-title] title list extended: Check for Updates, Quit Game");
    } else if (!why.empty()) {
      REXLOG_WARN("[kuf-title] title list left unchanged: {}", why);
    }
  }
  {  // M7: footers of other scenes in the same file (menu_mouse.cpp), if it has them
    std::string name(reinterpret_cast<const char*>(base + path), strnlen(reinterpret_cast<const char*>(base + path), 260));
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string why;
    if (kuf::menu_mouse::ExtendLayout(name, &*extended, &why)) {
      REXLOG_INFO("[kuf-mouse] {}: clickable footer entries added", name);
    } else if (!why.empty()) {
      REXLOG_WARN("[kuf-mouse] {}: no clickable footer entries: {}", name, why);
    }
  }
  if (g_label == 0) {  // once; kept for the whole run
    ctx.r3.u64 = sizeof(kLabel);
    sub_8218F410(ctx, base);
    if (ctx.r3.u32 != 0) {
      std::memcpy(base + ctx.r3.u32, kLabel, sizeof(kLabel));
      g_label = ctx.r3.u32;
    }
    ctx.r3.u64 = kScratchSize;
    sub_8218F410(ctx, base);
    g_scratch = ctx.r3.u32;
  }
  ctx.r3.u64 = extended->size();
  sub_8218F410(ctx, base);
  const uint32_t copy = ctx.r3.u32;
  if (copy == 0) {
    REXLOG_WARN("[kuf-pc-menu] {} layout left unchanged: no memory for {} bytes", menu, extended->size());
    ctx.r3.u64 = 0;
    return;
  }
  std::memcpy(base + copy, extended->data(), extended->size());
  Store32(base, buffer_ptr, copy);
  Store32(base, size_ptr, uint32_t(extended->size()));
  ctx.r3.u64 = buffer;
  sub_8218F7F8(ctx, base);
  ctx.r3.u64 = 0;
  REXLOG_INFO("[kuf-pc-menu] {} layout extended: {} -> {} bytes (PC Settings row)", menu, size, extended->size());
}

namespace {

uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFn fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  fn(ctx, base);
  return ctx.r3.u32;
}

uint32_t Find(PPCContext& ctx, uint8_t* base, uint32_t scene, uint32_t id) {
  return Call(ctx, base, sub_82624570, scene, id);
}

std::string TextOf(const uint8_t* base, uint32_t control) {
  const uint32_t text = control ? Load32(base, control + kButtonText) : 0;
  if (text == 0) {
    return {};
  }
  const char* s = reinterpret_cast<const char*>(base + text);
  return std::string(s, strnlen(s, kScratchSize - 1));
}

void SetText(PPCContext& ctx, uint8_t* base, uint32_t control, std::string_view text) {
  if (!control || !g_scratch) {
    return;
  }
  const size_t n = std::min<size_t>(text.size(), kScratchSize - 1);
  std::memcpy(base + g_scratch, text.data(), n);
  base[g_scratch + n] = 0;
  Call(ctx, base, sub_82624CB8, control, g_scratch, 0);
}

void Cue(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t cue) { Call(ctx, base, sub_8261B488, menu, cue); }

void Show(PPCContext& ctx, uint8_t* base, uint32_t control, bool shown) {
  if (control) {
    Call(ctx, base, sub_82623850, control, 1, shown ? 1 : 0);
  }
}

uint32_t Row(const uint8_t* base, const Spec& spec, uint32_t menu, uint32_t row) {
  return Load32(base, menu + spec.rows + 4 * row);
}

// The title's Change Storage Device row can be chosen (a storage device exists).
bool StorageRowAvailable(const uint8_t* base, const Spec& spec, uint32_t menu) {
  if (!spec.has_storage_row) {
    return false;
  }
  const uint32_t row = Row(base, spec, menu, kStorageRow);
  return row && (Load32(base, row + kControlState) & kStateUnavailable) == 0;
}

// The last of the game's own rows the cursor can reach.
uint32_t LastOriginalRow(const uint8_t* base, const Spec& spec, uint32_t menu) {
  if (spec.has_storage_row && !REXCVAR_GET(kuf_storage_row)) return kLastRow;  // the row is off screen (UX fix 5)
  return StorageRowAvailable(base, spec, menu) ? kStorageRow : kLastRow;
}

// --- Descriptions (M7 UX fix 4) ---------------------------------------------------------------------------------
//
// On the PC pages a text control cloned from the help line (ui_layout.h AddTextClone, kDescriptionText) shows what
// the selected row does, in the right-hand column: beside the label rows, below any sliders (never over a widget).
// The selection follows the pointer (menu_pointer.cpp), so hovering a row shows its description; the keyboard and a
// controller select the same way.

constexpr const char* kCategoryText[] = {
    "Window mode, resolution, monitor and aspect; VSync, the frame rate and its interpolation, the render "
    "resolution and the game's brightness.",
    "The modern camera: distance, field of view, vertical range and recentering, or the original console camera.",
    "Sound effects, music and voice volumes.",
    "Mouse sensitivity and vertical axis, and the keyboard and mouse keys for each gameplay action.",
    "Stick sensitivity and dead zone, the controller's buttons, the camera's vertical axis and vibration.",
    "Button prompts, the character's skin and blood effects.",
};
constexpr char kDefaultsText[] =
    "Sets the PC settings back to their defaults: camera, mouse and stick sensitivity, the frame rate, key and "
    "button bindings, prompts. The display and the game's own options (volumes, brightness, skin, blood, camera axis, "
    "vibration) stay as they are.";
// The game's own options by slot (the original Settings rows).
constexpr const char* kGameRowText[] = {
    "The volume of sound effects.",
    "The volume of the music.",
    "The volume of the characters' voices.",
    "The game's brightness.",
    "The character's skin (appearance) for this game.",
    "The camera's vertical axis with a controller's right stick: Normal or Inverted. The mouse has its own setting "
    "under Mouse & Keyboard.",
    "Blood effects on or off.",
    "Controller vibration on or off.",
};

const char* ItemText(Item item) {
  switch (item) {
    case Item::kMouseH:
      return "How far the camera turns per mouse movement, sideways. Raw mouse input: no acceleration or smoothing.";
    case Item::kMouseV: return "How far the camera turns per mouse movement, up and down.";
    case Item::kMouseY: return "Inverts the mouse's vertical axis (a controller follows Camera under Controller).";
    case Item::kStickSens:
      return "How fast the right stick turns the camera at full deflection (the vertical speed follows at 65 %).";
    case Item::kDeadzone:
      return "How far a stick must move before it counts. Raise it if the camera drifts with the stick released.";
    case Item::kPrompts:
      return "Which button icons the game's texts show: Auto follows the device you used last; Keyboard shows the "
             "bound keys; Gamepad shows the controller's icons.";
    case Item::kDistance: return "How far the camera stays from the character (100 % is the game's own distance).";
    case Item::kFov:
      return "The horizontal field of view at 16:9; wider displays see more at the sides. 58 is the game's own view.";
    case Item::kVRange: return "How far the camera can look up and down from level, in degrees.";
    case Item::kRecenter: return "Eases the view back to its usual height after a pause in camera input.";
    case Item::kModern:
      return "On: the PC camera (free mouse look and the settings above). Off: the original console camera, exactly "
             "as on the Xbox 360.";
    case Item::kLinkKeys:
      return "The keyboard and mouse keys for each gameplay action. Menus keep Enter, Escape and the arrow keys.";
    case Item::kLinkButtons: return "Which controller button does each gameplay action. Menus keep A, B and the D-pad.";
    default: return "";
  }
}
// The display page's descriptions by item (the constants above, so a new order cannot shift them).
const char* DisplayText(uint32_t item) {
  switch (item) {
    case kRowMode:
      return "Windowed, Borderless (the desktop's resolution) or Fullscreen (exclusive, with its own resolution and "
             "refresh rate). Takes effect with Apply.";
    case kRowSize: return "The window's size, or the fullscreen resolution and refresh rate. Takes effect with Apply.";
    case kRowMonitor: return "The monitor the game is shown on. Takes effect with Apply.";
    case kRowWidescreen:
      return "On: drawn for your display's aspect (21:9, 32:9, 16:10...). Off: 16:9 as the original, with black bars.";
    case kRowVsync:
      return "Synchronizes frame presentation with the display to avoid tearing. Can add latency and holds the frames "
             "shown to the display's refresh rate. The game's speed is unchanged.";
    case kItemFps + kRowFps:
      return "Sets the maximum number of frames rendered per second. Higher values can look smoother but use more GPU. "
             "The game itself always runs at its original 30 updates per second.";
    case kItemFps + kRowInterpolation:
      return "Draws the camera and the player between the game's 30 updates for smooth motion. Off: each frame repeats "
             "its update. Gameplay is unchanged.";
    case kItemFps + kRowObjects: return "Also interpolates the world's other characters and objects (with Interpolation on).";
    case kItemFps + kRowAnimation:
      return "Also interpolates the characters' skeletal animation and cloth (with Interpolation on).";
    case kItemFps + kRowEffects: return "Also runs the effect shaders' clock between updates (with Interpolation on).";
    case kRowRender:
      return "The resolution the game renders at: Native matches your display; 1x-4x are multiples of 1280x720. Higher "
             "costs more GPU. Takes effect at the next start (marked *).";
    case kRowBrightness: return "The game's own brightness (the same setting as on the Xbox 360). Changes at once.";
    case kRowApply:
      return "Applies Mode, Resolution and Monitor (greyed while they are as in effect). Keep the new display within 15 "
             "seconds or the previous one returns.";
    default: return "";
  }
}

std::string Describe(const MenuState& g) {
  if (!g.pc_page) return {};
  const uint32_t item = g.top + g.row;
  switch (g.page) {
    case Page::kCategories:
      return g.extra ? kDefaultsText : item < std::size(kCategoryText) ? kCategoryText[item] : "";
    case Page::kDisplay: return item < kDisplayItems ? DisplayText(kDisplayList[item]) : "";
    case Page::kKeys:
      return item < std::size(kKeyActions)
                 ? std::string(kKeyActions[item].name) +
                       ": the keys for this action in gameplay. Select, then press a key or mouse button (Escape "
                       "cancels). A key another action had moves here; that action gets this one's old keys."
                 : "";
    case Page::kButtons:
      return item < std::size(kButtonActions)
                 ? std::string(kButtonActions[item].name) +
                       ": the controller button for this action in gameplay. Select, then press the button; the two "
                       "buttons swap actions. Menus are not changed."
                 : "";
    default: {
      const auto rows = RowsOf(g.page);
      if (g.row >= rows.size()) return {};
      const RowDef& r = rows[g.row];
      return r.kind == Kind::kGame ? (r.slot < std::size(kGameRowText) ? kGameRowText[r.slot] : "") : ItemText(r.item);
    }
  }
}

// The rows of a page one under the other (M7 menu overhaul): a page's slots are not always consecutive (the game's
// own rows keep their slot, with its widget), so each shown slot moves up into the page's next row position, all its
// controls (label, widget, slider track, frame) by the same amount, through each control's own SetRect. The game
// takes the highlight's place from the selected row's button and draws the sliders from their own controls, so both
// follow. Zero moves them back (the game's Settings page, Settings closed).
struct SlotPlacement {
  uint32_t scene = 0;  // the Settings instance (a new one has the layout's places)
  bool known = false;
  std::array<int32_t, kRows> y{};   // each slot's label top in the layout
  std::array<int32_t, kRows> dy{};  // how far each slot is moved now
  int32_t extra_y = 0;              // the Restore Defaults row's top in the layout
  int32_t extra_dy = 0;             // and how far it is moved now
};
SlotPlacement g_placement[2];  // in-game, title

// The description control: its text, and its rectangle in the right-hand column from the first row without a widget
// on this page to the last row (hidden off the PC pages).
void UpdateDescription(PPCContext& ctx, uint8_t* base, const Spec& spec, const MenuState& g, uint32_t scene) {
  const uint32_t text = spec.description_id ? Find(ctx, base, scene, spec.description_id) : 0;
  if (!text || !g.menu) return;
  const std::string description = Describe(g);
  if (description.empty()) {
    Show(ctx, base, text, false);
    return;
  }
  // UI polish: a region of its own below the list (the eighth row, or the Restore Defaults row on the category page)
  // and above the button help, as wide as the rows: clear of every row, slider and the panel's background logo.
  const uint32_t last_row = Row(base, spec, g.menu, kLastRow), label = Row(base, spec, g.menu, 0);
  const uint32_t track = Find(ctx, base, scene, spec.slider_tracks[0]);
  const uint32_t pc_button = g.pc_button;
  if (!last_row || !label || !track) return;
  // Below the last row's place in the layout (fixed: PlaceRows may have moved that slot up).
  int32_t y = int32_t(Load32(base, last_row + 20) + Load32(base, last_row + 28)) -
              g_placement[&spec == &kTitle ? 1 : 0].dy[kLastRow];
  if (g.page == Page::kCategories && pc_button) {
    y = std::max(y, int32_t(Load32(base, pc_button + 20) + Load32(base, pc_button + 28)));
  }
  y += kDescriptionGap;
  const int32_t x = int32_t(Load32(base, label + 16)) + kDescriptionIndent;
  const int32_t right = int32_t(Load32(base, track + 16) + Load32(base, track + 24));
  // The button help line's top (layout); in the title, the version line's.
  const int32_t bottom = (&spec == &kTitle ? kVersionY : spec.help_y) - kDescriptionGap / 2;
  if (right - x < 200 || bottom - y < 16) return;
  // The control's own SetRect (vtable slot 5: control, x, y, width, height), only when it is the toolkit's base one
  // (sub_82624018, which the buttons' sub_82625AC8 calls first); otherwise the layout's rectangle stays.
  const uint32_t set_rect = Load32(base, Load32(base, text) + 20);
  static bool logged = false;
  if (set_rect != kSetRect && !logged) {
    logged = true;
    REXLOG_WARN("[kuf-pc-menu] the description's SetRect is {:08X}, not the shared one: not moved", set_rect);
  }
  if (PPCFunc* fn = set_rect == kSetRect ? rex::runtime::ResolveIndirectFunction(set_rect) : nullptr) {
    ctx.r3.u64 = text;
    ctx.r4.s64 = x;
    ctx.r5.s64 = y;
    ctx.r6.s64 = right - x;
    ctx.r7.s64 = bottom - y;
    fn(ctx, base);
  }
  SetText(ctx, base, text, "@(offsety=-2)" + description);
  Show(ctx, base, text, true);
  if (REXCVAR_GET(kuf_trace_menu_pointer)) {
    REXLOG_INFO("[kuf-pc-menu] description ({},{} {}x{}): {}", x, y, right - x, bottom - y, description);
  }
}

// The button help (in-game: set here; the title's own highlight code sets it by row).
void ShowHelp(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t scene, bool action) {
  UpdateDescription(ctx, base, spec, g, scene);  // M7 UX fix 4 (every selection change passes here)
  if (!spec.help_text_id) {
    return;
  }
  const uint32_t help = Find(ctx, base, scene, spec.help_text_id);
  if (!help) {
    return;
  }
  if (!g.help_changed && action) {
    g.help = TextOf(base, help);
  }
  if (action != g.help_changed && (!action || !g.help.empty())) {
    SetText(ctx, base, help, action ? kuf::prompts::ForCurrentDevice(kActionHelp) : g.help);  // M7: keyboard prompts
    g.help_changed = action;
  }
}

// While the PC page shows, the title's Change Storage Device row is shown unavailable (the game's own state flag
// for rows that cannot be chosen, as it uses for Vibration without vibration support); afterwards it is made
// available again if it was.
void SetStorageRowUnavailable(PPCContext& ctx, uint8_t* base, const Spec& spec, uint32_t menu, bool unavailable) {
  if (spec.has_storage_row) {
    if (const uint32_t row = Row(base, spec, menu, kStorageRow)) {
      Call(ctx, base, sub_82623850, row, kStateUnavailable, unavailable ? 1 : 0);
    }
  }
}

// The original highlight code on one of the game's rows.
void HighlightRow(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, uint32_t row,
                  uint32_t pc_button) {
  if (pc_button) {
    Show(ctx, base, pc_button, true);
  }
  Store32(base, menu + spec.selected, row);
  Call(ctx, base, spec.show_row, menu);
  if (g.pc_page && g.storage_available) {
    SetStorageRowUnavailable(ctx, base, spec, menu, true);
  }
}

// The highlight on the PC Settings row: the original highlight code with that row's button in a row slot.
void HighlightPcRow(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu,
                    uint32_t pc_button) {
  const uint32_t slot = menu + spec.rows + 4 * spec.extra_slot;
  const uint32_t original = Load32(base, slot);
  if (original) {
    Show(ctx, base, original, true);
  }
  Store32(base, menu + spec.selected, spec.extra_slot);
  Store32(base, slot, pc_button);
  Call(ctx, base, spec.show_row, menu);
  Store32(base, slot, original);
  if (g.pc_page && g.storage_available) {
    SetStorageRowUnavailable(ctx, base, spec, menu, true);
  }
}

int RangeIndex(int degrees) {
  for (size_t i = 0; i < std::size(kuf::pc::kVerticalRanges); ++i) {
    if (kuf::pc::kVerticalRanges[i] == degrees) return int(i);
  }
  return int(std::size(kuf::pc::kVerticalRanges)) - 1;
}

int FovIndex(int fov) { return fov == kuf::pc::kFovOriginal ? 0 : (fov - kuf::pc::kFovMin) / kuf::pc::kFovStep + 1; }
constexpr int kFovIndexMax = (kuf::pc::kFovMax - kuf::pc::kFovMin) / kuf::pc::kFovStep + 1;

void DrawSlider(PPCContext& ctx, uint8_t* base, const Spec& spec, uint32_t menu, uint32_t slot, double value) {
  const uint32_t selected = Load32(base, menu + spec.selected);
  Store32(base, menu + spec.selected, slot);  // the slider functions draw the selected row's slider
  ctx.f1.f64 = value;
  Call(ctx, base, spec.draw_slider, menu);
  Store32(base, menu + spec.selected, selected);
}

// The controls of one slot: its label button and its widget (slider, selector, or the two toggle halves).
std::array<uint32_t, 4> SlotControls(const uint8_t* base, const Spec& spec, uint32_t menu, uint32_t slot) {
  std::array<uint32_t, 4> c{Row(base, spec, menu, slot), 0, 0, 0};
  if (slot < 4) {
    c[1] = Load32(base, menu + spec.slider_fill + 4 * slot);
    c[2] = Load32(base, menu + spec.slider_knob + 4 * slot);
  } else if (slot == 4) {
    c[1] = Load32(base, menu + spec.selector_text);
    c[2] = Load32(base, menu + spec.arrow_left);
    c[3] = Load32(base, menu + spec.arrow_right);
  } else if (slot < kRows) {
    c[1] = Load32(base, menu + spec.toggles + 8 * (slot - 5));
    c[2] = Load32(base, menu + spec.toggles + 8 * (slot - 5) + 4);
  }
  return c;
}

struct Rect {
  int32_t x = 0, y = 0, w = 0, h = 0;
  bool Contains(int32_t px, int32_t py, int32_t pad_y = 0) const {
    return w > 0 && px >= x && px < x + w && py >= y - pad_y && py < y + h + pad_y;
  }
};
Rect RectOf(const uint8_t* base, uint32_t control) {
  if (!control) return {};
  return {int32_t(Load32(base, control + 16)), int32_t(Load32(base, control + 20)), int32_t(Load32(base, control + 24)),
          int32_t(Load32(base, control + 28))};
}

// A control's rectangle through its own SetRect (vtable slot 5).
void MoveTo(PPCContext& ctx, uint8_t* base, uint32_t control, const Rect& r) {
  if (control < 0x10000) return;
  PPCFunc* fn = rex::runtime::ResolveIndirectFunction(Load32(base, Load32(base, control) + 20));
  if (!fn) return;
  ctx.r3.u64 = control;
  ctx.r4.s64 = r.x;
  ctx.r5.s64 = r.y;
  ctx.r6.s64 = r.w;
  ctx.r7.s64 = r.h;
  fn(ctx, base);
}

void MoveControlY(PPCContext& ctx, uint8_t* base, uint32_t control, int32_t dy) {
  if (control < 0x10000 || !dy) return;
  PPCFunc* fn = rex::runtime::ResolveIndirectFunction(Load32(base, Load32(base, control) + 20));
  if (!fn) return;
  ctx.r3.u64 = control;
  ctx.r4.s64 = int32_t(Load32(base, control + 16));
  ctx.r5.s64 = int32_t(Load32(base, control + 20)) + dy;
  ctx.r6.s64 = int32_t(Load32(base, control + 24));
  ctx.r7.s64 = int32_t(Load32(base, control + 28));
  fn(ctx, base);
}

void PlaceSlot(PPCContext& ctx, uint8_t* base, const Spec& spec, uint32_t menu, uint32_t scene, uint32_t slot,
               int32_t dy) {
  SlotPlacement& p = g_placement[&spec == &kTitle ? 1 : 0];
  const int32_t delta = dy - p.dy[slot];
  if (!delta) return;
  for (const uint32_t c : SlotControls(base, spec, menu, slot)) MoveControlY(ctx, base, c, delta);
  if (slot < std::size(spec.slider_tracks)) MoveControlY(ctx, base, Find(ctx, base, scene, spec.slider_tracks[slot]), delta);
  for (uint32_t i = 0; spec.row_frames[slot] && i < 6; ++i) {
    MoveControlY(ctx, base, Find(ctx, base, scene, spec.row_frames[slot] + i), delta);
  }
  p.dy[slot] = dy;
}

void PlaceRows(PPCContext& ctx, uint8_t* base, const Spec& spec, uint32_t menu, uint32_t scene, uint32_t pc_button,
               Page page, bool original) {
  SlotPlacement& p = g_placement[&spec == &kTitle ? 1 : 0];
  if (p.scene != scene) p = SlotPlacement{scene};
  if (!p.known) {  // the layout's places (nothing moved yet)
    for (uint32_t slot = 0; slot < kRows; ++slot) {
      const uint32_t row = Row(base, spec, menu, slot);
      if (!row) return;
      p.y[slot] = int32_t(Load32(base, row + 20));
    }
    p.extra_y = pc_button ? int32_t(Load32(base, pc_button + 20)) : 0;
    p.known = true;
  }
  const bool compact = REXCVAR_GET(kuf_settings_compact_rows) && !original;
  const auto rows = RowsOf(page);
  // Row i's place: from the first row down, one row spacing apart (the first two rows' distance). The layouts' own
  // places are not evenly spaced (the title's fourth row is a row lower: the player's screenshot of Display showed
  // an empty row there), so positions come from the shown rows' order, not from their slots.
  const int32_t pitch = p.y[1] - p.y[0];
  std::array<int32_t, kRows> dy{};
  for (uint32_t i = 0; compact && i < rows.size(); ++i) dy[rows[i].slot] = p.y[0] + int32_t(i) * pitch - p.y[rows[i].slot];
  for (uint32_t slot = 0; slot < kRows; ++slot) PlaceSlot(ctx, base, spec, menu, scene, slot, dy[slot]);
  // Restore Defaults (the categories' last row, the PC row button and its frame): right below the categories.
  const int32_t extra_dy = compact && page == Page::kCategories && pc_button && p.extra_y
                               ? p.y[0] + int32_t(rows.size()) * pitch - p.extra_y
                               : 0;
  if (const int32_t delta = extra_dy - p.extra_dy; delta && pc_button) {
    MoveControlY(ctx, base, pc_button, delta);
    for (uint32_t i = 0; spec.row_frames[9] && i < 6; ++i) {
      MoveControlY(ctx, base, Find(ctx, base, scene, spec.row_frames[9] + i), delta);
    }
    p.extra_dy = extra_dy;
  }
}

// Shows what a page uses of the slots (labels, widgets, frames, slider tracks) with the game's own shown flag and hides
// the rest; the Restore Defaults row only on the category page; the title's Change Storage Device never. With
// `original` everything shows again (the game's Settings page).
void ShowRowsFor(PPCContext& ctx, uint8_t* base, const Spec& spec, uint32_t menu, uint32_t scene, uint32_t pc_button,
                 Page page, bool original = false, int32_t selected_slot = -1) {
  const auto rows = RowsOf(page);
  PlaceRows(ctx, base, spec, menu, scene, pc_button, page, original);
  for (uint32_t slot = 0; slot < kRows; ++slot) {
    const RowDef* def = nullptr;
    for (const auto& r : rows) {
      if (r.slot == slot) def = &r;
    }
    const bool row_shown = original || def;
    // Only a slider row or the game's own row shows its slot's widget (a link row is a plain entry: Key Bindings showed
    // slot 3's slider, the player's screenshot).
    const bool widget_shown = original || (def && (def->kind == Kind::kSlider || def->kind == Kind::kGame));
    const auto controls = SlotControls(base, spec, menu, slot);
    // The selected row's own label stays hidden: the highlight drawn over it shows the label (as the game's own code
    // leaves it; shown, the row's plain label covered the highlight's and the selection looked greyed).
    Show(ctx, base, controls[0], row_shown && int32_t(slot) != selected_slot);
    for (size_t i = 1; i < controls.size(); ++i) {
      Show(ctx, base, controls[i], widget_shown);
    }
    if (slot < std::size(spec.slider_tracks)) {
      Show(ctx, base, Find(ctx, base, scene, spec.slider_tracks[slot]), widget_shown);
    }
    for (uint32_t i = 0; spec.row_frames[slot] && i < 6; ++i) {
      // The frame (images 0-2) shows with the row; the label button's own images (3-5) only with the button: the
      // selected row's are hidden with it (in slots 3 and 7 they are drawn after the highlight and covered it).
      const bool label_image = i >= 3;
      Show(ctx, base, Find(ctx, base, scene, spec.row_frames[slot] + i),
           row_shown && !(label_image && int32_t(slot) == selected_slot));
    }
  }
  const bool extra_shown = original || page == Page::kCategories;
  if (spec.has_storage_row) {
    Show(ctx, base, Row(base, spec, menu, kStorageRow), original);
    for (uint32_t i = 0; spec.row_frames[kStorageRow] && i < 6; ++i) {
      Show(ctx, base, Find(ctx, base, scene, spec.row_frames[kStorageRow] + i), original);
    }
  }
  for (uint32_t i = 0; spec.row_frames[9] && i < 6; ++i) {
    Show(ctx, base, Find(ctx, base, scene, spec.row_frames[9] + i), extra_shown);
  }
  Show(ctx, base, pc_button, extra_shown && selected_slot != kExtraSelected);
}

// --- The display page -----------------------------------------------------------------------------------

using kuf::pc::DisplayMode;
using kuf::pc::DisplaySettings;

bool SameDisplay(const DisplaySettings& a, const DisplaySettings& b) {
  return a.mode == b.mode && a.width == b.width && a.height == b.height && a.refresh_mhz == b.refresh_mhz &&
         a.monitor == b.monitor;
}

const kuf::display::Monitor* PendingMonitor(const MenuState& g, size_t* index = nullptr) {
  if (g.monitors.empty()) {
    return nullptr;
  }
  bool fallback = false;
  const size_t i = kuf::display::FindMonitor(g.monitors, g.pending.monitor, &fallback);
  if (index) *index = i;
  return &g.monitors[i];
}

// The sizes the Size row offers for the pending mode and monitor.
std::vector<kuf::display::Mode> SizeChoices(const MenuState& g) {
  const auto* m = PendingMonitor(g);
  if (!m) {
    return {};
  }
  switch (g.pending.mode) {
    case DisplayMode::kWindowed: return kuf::display::WindowSizes(m->modes, m->usable_width, m->usable_height);
    case DisplayMode::kFullscreen: return m->modes;
    default: return {m->desktop};
  }
}

// After the mode or monitor changed: a size the new choices have.
void FitPendingSize(MenuState& g) {
  const auto choices = SizeChoices(g);
  const auto* m = PendingMonitor(g);
  if (g.pending.mode == DisplayMode::kBorderless || choices.empty() || !m) {
    g.pending.width = g.pending.height = g.pending.refresh_mhz = 0;
    return;
  }
  if (kuf::display::FindMode(choices, g.pending.width, g.pending.height, g.pending.refresh_mhz) >= 0) {
    if (g.pending.mode == DisplayMode::kWindowed) g.pending.refresh_mhz = 0;
    return;
  }
  // Fullscreen: the desktop's mode; windowed: the largest size smaller than the desktop.
  int pick = 0;
  if (g.pending.mode == DisplayMode::kFullscreen) {
    pick = std::max(0, kuf::display::FindMode(choices, m->desktop.width, m->desktop.height, m->desktop.refresh_mhz));
  } else {
    for (size_t i = 0; i < choices.size(); ++i) {
      if (choices[i].width < m->desktop.width && choices[i].height < m->desktop.height) {
        pick = int(i);
        break;
      }
    }
  }
  g.pending.width = int(choices[pick].width);
  g.pending.height = int(choices[pick].height);
  g.pending.refresh_mhz = g.pending.mode == DisplayMode::kFullscreen ? int(choices[pick].refresh_mhz) : 0;
}

std::string Clip(std::string text) {
  if (text.size() > kLabelChars) text.resize(kLabelChars);
  return "@(offsety=-2)" + text;
}

// --- The frame rate (D5; on the display page since M7) ----------------------------------------------------------------------------

using kuf::pc::FrameRateSettings;

std::string FpsName(int fps) {
  if (fps == kuf::pc::kFpsOriginal) return "30 (Original)";
  if (fps == kuf::pc::kFpsUnlimited) return "Unlimited";
  return std::to_string(fps);
}

std::string OnOff(const char* name, bool on) { return Clip(std::string(name) + (on ? "On" : "Off")); }

std::string FrameRateLabel(const FrameRateSettings& f, uint32_t row) {
  switch (row) {
    case kRowFps: return Clip("FPS: " + FpsName(f.fps_limit));
    case kRowInterpolation: return OnOff("Interpolation: ", f.interpolation);
    case kRowObjects: return OnOff("World Objects: ", f.objects);
    case kRowAnimation: return OnOff("Animation: ", f.animation);
    default: return OnOff("Effects: ", f.effects);
  }
}

// The menu's frame rates in order (30 ... 240, Unlimited last); a custom limit from the file steps to its neighbours.
int StepFps(int fps, int step, bool wrap) {
  constexpr int n = int(std::size(kuf::pc::kFpsChoices));
  auto rank = [](int v) { return v == kuf::pc::kFpsUnlimited ? 100000 : v; };
  int index = -1;
  for (int i = 0; i < n; ++i) {
    if (kuf::pc::kFpsChoices[i] == fps) index = i;
  }
  if (index < 0) {  // custom: the nearest choice in the direction of the step
    for (int i = 0; i < n; ++i) {
      const int r = rank(kuf::pc::kFpsChoices[step > 0 ? i : n - 1 - i]);
      if (step > 0 ? r > rank(fps) : r < rank(fps)) return kuf::pc::kFpsChoices[step > 0 ? i : n - 1 - i];
    }
    return fps;
  }
  int next = index + step;
  if (wrap) next = (next + n) % n;
  return kuf::pc::kFpsChoices[std::clamp(next, 0, n - 1)];
}

// Left (-1) / Right (+1) / Select (+1, wrapping) on a frame-rate row. Returns false if nothing changed.
bool ChangeFrameRateValue(uint32_t row, int step, bool wrap) {
  auto settings = kuf::pc::Current();
  auto& f = settings.frame_rate;
  auto toggle = [&](bool& v) { v = wrap ? !v : step < 0; };  // Left: On, Right: Off (as the toggles); Select: flip
  switch (row) {
    case kRowFps: f.fps_limit = StepFps(f.fps_limit, step, wrap); break;
    case kRowInterpolation: toggle(f.interpolation); break;
    case kRowObjects: toggle(f.objects); break;
    case kRowAnimation: toggle(f.animation); break;
    default: toggle(f.effects); break;
  }
  if (f == kuf::pc::Current().frame_rate) return false;
  kuf::pc::Update(settings);
  kuf::pacing::RequestFrameRate(f);
  REXLOG_INFO("[kuf-pc-menu] frame rate {} interpolation {} objects {} animation {} effects {}", FpsName(f.fps_limit),
              f.interpolation, f.objects, f.animation, f.effects);
  return true;
}

std::string DisplayLabel(const MenuState& g, uint32_t row) {
  const auto& d = g.pending;
  switch (row) {
    case kRowMode:
      return Clip(std::string("Mode: ") +
                  (d.mode == DisplayMode::kWindowed ? "Windowed" : d.mode == DisplayMode::kFullscreen ? "Fullscreen"
                                                                                                    : "Borderless"));
    case kRowSize: {
      const auto* m = PendingMonitor(g);
      kuf::display::Mode size{uint32_t(d.width), uint32_t(d.height), uint32_t(d.refresh_mhz)};
      if (d.mode == DisplayMode::kBorderless || !d.width) {
        size = m ? m->desktop : kuf::display::Mode{};
        size.refresh_mhz = 0;
      }
      // ("@" starts the game's text markup: not used in values.)
      const std::string wxh = std::to_string(size.width) + "x" + std::to_string(size.height);
      std::string text = d.mode == DisplayMode::kWindowed     ? "Window: " + wxh
                         : d.mode == DisplayMode::kBorderless ? "Desktop: " + wxh
                                                              : wxh;
      if (d.mode == DisplayMode::kFullscreen && size.refresh_mhz) {
        text += " " + std::to_string((size.refresh_mhz + 500) / 1000) + "Hz";
      }
      return Clip(text);
    }
    case kRowMonitor: {
      size_t index = 0;
      const auto* m = PendingMonitor(g, &index);
      return Clip("Monitor: " + std::to_string(index + 1) + " " + (m ? m->name : std::string("?")));
    }
    case kRowWidescreen:
      return Clip(std::string("Widescreen: ") + (d.widescreen ? "On" : "Off"));
    case kRowRender: {
      const auto out = kuf::display::CurrentOutput();
      const bool restart = !(kuf::display::RenderScaleForSetting(d.render_scale, out.width, out.height) ==
                             kuf::display::StartRenderScale());
      return Clip("Render: " + kuf::display::RenderSettingName(d.render_scale) + (restart ? " *" : ""));
    }
    case kRowVsync:
      return Clip(std::string("VSync: ") + (kuf::pc::Current().display.vsync ? "On" : "Off"));
    case kRowBrightness: return Clip("Brightness: " + std::to_string(int(std::lround(g.brightness * 100))) + "%");
    case kRowApply:
      if (g.confirming) return "@(offsety=-2)Keep? @(A)Yes @(B)No";
      return Clip(SameDisplay(d, g.loaded) ? "Apply" : "Apply *");
    default:
      return FrameRateLabel(kuf::pc::Current().frame_rate, row - kItemFps);
  }
}

// Opening the display page: its rows show the display in effect.
void LoadDisplayPage(MenuState& g) {
  g.monitors = kuf::display::Monitors();
  g.pending = kuf::pc::Current().display;
  g.confirming = false;
  FitPendingSize(g);  // a saved size of 0 (the desktop's) shows as that size
  g.loaded = g.pending;
}

// A timed-out confirmation (the timer thread restored the previous display) is shown on the next input.
void TakeTimeout(MenuState& g) {
  if (g.confirming && g_confirm_timed_out.load() == g.confirm) {
    g.confirming = false;
    g.pending = g.loaded;
  }
}

void RevertDisplay(MenuState& g, const char* why) {
  ++g_confirm;  // no timer acts on it any more
  std::string result;
  kuf::display::Apply(g.previous, &result);
  g.confirming = false;
  g.pending = g.loaded;
  REXLOG_INFO("[kuf-pc-menu] display {}: back to the previous one", why);
}

void ApplyDisplay(MenuState& g) {
  const DisplaySettings current = kuf::pc::Current().display;
  if (SameDisplay(g.pending, g.loaded)) {
    return;
  }
  g.previous = current;
  std::string result;
  if (!kuf::display::Apply(g.pending, &result)) {
    REXLOG_WARN("[kuf-pc-menu] display not applied: {}", result);
    std::string ignored;
    kuf::display::Apply(current, &ignored);
    g.pending = g.loaded;
    return;
  }
  g.confirming = true;
  g.confirm = ++g_confirm;
  REXLOG_INFO("[kuf-pc-menu] display applied ({}): keep within {} s or it reverts", result, kConfirmSeconds);
  const uint64_t confirm = g.confirm;
  const DisplaySettings previous = g.previous;
  std::thread([confirm, previous] {
    std::this_thread::sleep_for(std::chrono::seconds(kConfirmSeconds));
    if (g_confirm.load() != confirm) {
      return;  // kept, reverted or replaced meanwhile
    }
    std::string result;
    kuf::display::Apply(previous, &result);
    g_confirm_timed_out = confirm;
    REXLOG_INFO("[kuf-pc-menu] display not kept within {} s: back to the previous one", kConfirmSeconds);
  }).detach();
}

void KeepDisplay(MenuState& g) {
  ++g_confirm;
  g.confirming = false;
  auto settings = kuf::pc::Current();
  const bool widescreen = settings.display.widescreen;  // these two are saved by their own rows
  const int render_scale = settings.display.render_scale;
  settings.display = g.pending;
  settings.display.widescreen = widescreen;
  settings.display.render_scale = render_scale;
  kuf::pc::Update(settings);
  g.loaded = g.pending;
  REXLOG_INFO("[kuf-pc-menu] display kept: {} {}x{} refresh {} mHz monitor '{}'",
              kuf::pc::DisplayModeName(settings.display.mode), settings.display.width, settings.display.height,
              settings.display.refresh_mhz, settings.display.monitor);
}

// Left (-1) / Right (+1) on a display row. Returns false if nothing changed.
bool ChangeDisplayValue(MenuState& g, uint32_t row, int step) {
  auto& d = g.pending;
  switch (row) {
    case kRowMode: {
      const int mode = std::clamp(int(d.mode) + step, int(DisplayMode::kWindowed), int(DisplayMode::kFullscreen));
      if (mode == int(d.mode)) return false;
      d.mode = DisplayMode(mode);
      FitPendingSize(g);
      return true;
    }
    case kRowSize: {
      const auto choices = SizeChoices(g);
      if (d.mode == DisplayMode::kBorderless || choices.empty()) return false;
      int index = kuf::display::FindMode(choices, d.width, d.height, d.refresh_mhz);
      // The choices go from the largest; Right is larger.
      const int next = std::clamp(index < 0 ? 0 : index - step, 0, int(choices.size()) - 1);
      if (next == index) return false;
      d.width = int(choices[next].width);
      d.height = int(choices[next].height);
      d.refresh_mhz = d.mode == DisplayMode::kFullscreen ? int(choices[next].refresh_mhz) : 0;
      return true;
    }
    case kRowMonitor: {
      g.monitors = kuf::display::Monitors();
      size_t index = 0;
      if (!PendingMonitor(g, &index)) return false;
      const int next = std::clamp(int(index) + step, 0, int(g.monitors.size()) - 1);
      if (next == int(index) && d.monitor == g.monitors[index].name) return false;
      d.monitor = g.monitors[next].name;
      FitPendingSize(g);
      return true;
    }
    case kRowWidescreen: {
      const bool on = step < 0;  // Left: On, Right: Off (as the toggles)
      if (on == d.widescreen) return false;
      d.widescreen = on;
      auto settings = kuf::pc::Current();
      settings.display.widescreen = on;  // at once (no confirmation needed)
      kuf::pc::Update(settings);
      kuf::display::Refresh();
      REXLOG_INFO("[kuf-pc-menu] widescreen {}", on ? "on" : "off");
      return true;
    }
    case kRowVsync: {
      const bool on = step < 0;  // Left: On, Right: Off (as the toggles)
      auto settings = kuf::pc::Current();
      if (on == settings.display.vsync) return false;
      settings.display.vsync = d.vsync = on;  // at once (presentation only; no confirmation needed)
      kuf::pc::Update(settings);
      kuf::display::SetVsync(on);
      REXLOG_INFO("[kuf-pc-menu] vsync {}", on ? "on" : "off");
      return true;
    }
    case kRowRender: {
      const int scale = std::clamp(d.render_scale + step, kuf::pc::kRenderScaleNative, kuf::pc::kRenderScaleMax);
      if (scale == d.render_scale) return false;
      d.render_scale = scale;
      auto settings = kuf::pc::Current();
      settings.display.render_scale = scale;  // saved now, in effect from the next start
      kuf::pc::Update(settings);
      const auto out = kuf::display::CurrentOutput();
      const auto need = kuf::display::RenderScaleForSetting(scale, out.width, out.height);
      const auto running = kuf::display::StartRenderScale();
      REXLOG_INFO("[kuf-pc-menu] render {}: {}x{}{}", kuf::display::RenderSettingName(scale),
                  kuf::display::kGuestWidth * need.x, kuf::display::kGuestHeight * need.y,
                  need == running ? " (in effect)"
                                  : fmt::format(" from the next start (now {}x{})",
                                                kuf::display::kGuestWidth * running.x,
                                                kuf::display::kGuestHeight * running.y));
      return true;
    }
    default:
      return false;
  }
}

// --- Labels, values and changes per page ------------------------------------------------------------------------

std::string ItemLabel(const MenuState& g, const kuf::pc::Settings& s, Item item) {
  const auto& c = s.camera;
  constexpr const char* kPrompts[] = {"Auto", "Keyboard", "Gamepad"};  // short: the highlight wraps at ~18
  switch (item) {
    case Item::kMouseH: return "@(offsety=-2)Mouse Horizontal";
    case Item::kMouseV: return "@(offsety=-2)Mouse Vertical";
    case Item::kMouseY: return Clip(std::string("Mouse Y: ") + (c.invert_mouse_y ? "Inverted" : "Normal"));
    case Item::kStickSens: return "@(offsety=-2)Stick Sensitivity";
    case Item::kDeadzone: return "@(offsety=-2)Stick Dead Zone";
    case Item::kPrompts: return Clip(std::string("Prompts: ") + kPrompts[std::clamp(s.ui.button_prompts, 0, 2)]);
    case Item::kDistance: return "@(offsety=-2)Distance " + std::to_string(c.distance) + "%";
    case Item::kFov:  // the game's own field of view is 57.6 degrees horizontally at 16:9 (camera_math.h)
      return "@(offsety=-2)Field of View " +
             std::to_string(c.field_of_view == kuf::pc::kFovOriginal ? kOriginalFovDegrees : c.field_of_view);
    case Item::kVRange: return Clip("Vertical Range: " + std::to_string(c.vertical_range));
    case Item::kRecenter: return OnOff("Recentering: ", c.auto_recenter);
    case Item::kModern: return OnOff("Modern Camera: ", c.modern);
    case Item::kLinkKeys: return "@(offsety=-2)Key Bindings >";  // opens a page
    case Item::kLinkButtons: return "@(offsety=-2)Controller Buttons >";
    default: return {};
  }
}

// --- Key Bindings and Controller Buttons (M7 feature 5) ---------------------------------------------------------

std::string KeysLabel(const MenuState& g, uint32_t action) {
  const KeyAction& a = kKeyActions[action];
  if (g.capturing && g.capture_action == action) return Clip("Press a Key (Esc)");
  // As many keys as fit on the row without wrapping (the first always; "+" when some are left out).
  constexpr size_t kKeysRowChars = 17;
  std::string label = std::string(a.name) + ": ";
  const size_t start = label.size();
  if (auto* driver = kuf::input::KufInputDriver::Instance()) {
    const auto keys = driver->GameplayKeys(a.control);
    for (size_t i = 0; i < keys.size(); ++i) {
      const std::string next = (i ? ", " : "") + kuf::input::HostKeyLabel(keys[i]);
      if (i && label.size() + next.size() > kKeysRowChars) {
        label += " +";
        break;
      }
      label += next;
    }
  }
  if (label.size() == start) label += "None";
  return Clip(label);
}

std::string ButtonsLabel(const MenuState& g, uint32_t action) {
  const ButtonAction& a = kButtonActions[action];
  if (g.capturing && g.capture_action == action) return Clip("Press a Button");
  const auto remap = kuf::input::RemapFromSettings(kuf::pc::Current().bindings);
  // The icon markup is not counted against the label's width.
  return Clip(std::string(a.name) + ": ") + kuf::input::PadButtonIcon(remap.source(a.button));
}

bool IsReservedKey(kuf::input::HostKey key) {  // the runtime's overlays (docs/INPUT.md)
  for (const char* name : {"F3", "F4", "F7", "Backtick"}) {
    if (key == kuf::input::ParseHostKey(name)) return true;
  }
  return false;
}

// `key` becomes the only key of the action's control in gameplay. Another control that had it loses it; one left
// with no key takes the action's previous keys (a swap), so nothing is left unbound.
void AssignKey(uint32_t action, kuf::input::HostKey key) {
  using kuf::input::HostKey;
  auto* driver = kuf::input::KufInputDriver::Instance();
  if (!driver) return;
  const auto defaults = kuf::input::Bindings::Defaults();
  std::array<std::vector<HostKey>, kuf::input::kControlCount> keys;
  for (size_t c = 0; c < keys.size(); ++c) keys[c] = driver->GameplayKeys(static_cast<Control>(c));
  const size_t target = static_cast<size_t>(kKeyActions[action].control);
  const auto previous = keys[target];
  for (size_t c = 0; c < keys.size(); ++c) {
    if (c == target || std::find(keys[c].begin(), keys[c].end(), key) == keys[c].end()) continue;
    std::erase(keys[c], key);
    if (keys[c].empty()) {
      keys[c] = previous;
      std::erase(keys[c], key);
    }
    REXLOG_INFO("[kuf-pc-menu] {} no longer on {}", kuf::input::ControlName(static_cast<Control>(c)),
                kuf::input::HostKeyName(key));
  }
  keys[target] = {key};
  auto settings = kuf::pc::Current();
  settings.bindings.keys.clear();
  for (size_t c = 0; c < keys.size(); ++c) {
    const auto control = static_cast<Control>(c);
    if (keys[c] == defaults.keys(kuf::input::Context::kGameplay, control) || keys[c].empty()) continue;
    std::string names;
    for (const auto k : keys[c]) names += (names.empty() ? "" : ",") + kuf::input::HostKeyName(k);
    settings.bindings.keys[kuf::input::ControlName(control)] = names;
  }
  kuf::pc::Update(settings);
  kuf::input::ApplyBindingSettings();
  REXLOG_INFO("[kuf-pc-menu] gameplay key: {} = {}", kuf::input::ControlName(kKeyActions[action].control),
              kuf::input::HostKeyName(key));
}

// `physical` acts as the action's button in gameplay (the button it acted as before gets the action's old one).
void AssignButton(uint32_t action, PadButton physical) {
  auto settings = kuf::pc::Current();
  auto remap = kuf::input::RemapFromSettings(settings.bindings);
  remap.Assign(kButtonActions[action].button, physical);
  settings.bindings.buttons.clear();
  for (size_t i = 0; i < kuf::input::kPadButtonCount; ++i) {
    const auto b = static_cast<PadButton>(i);
    if (remap.source(b) != b) settings.bindings.buttons[kuf::input::PadButtonName(b)] =
        kuf::input::PadButtonName(remap.source(b));
  }
  kuf::pc::Update(settings);
  kuf::input::ApplyBindingSettings();
  REXLOG_INFO("[kuf-pc-menu] controller button: {} acts as {}", kuf::input::PadButtonName(physical),
              kuf::input::PadButtonName(kButtonActions[action].button));
}

void BeginCapture(MenuState& g, uint32_t action) {
  g.capturing = true;
  g.capture_action = action;
  g.capture_start = std::chrono::steady_clock::now();
  // The keyboard always (on the Controller Buttons page any key cancels); controllers on that page only.
  if (auto* keys = kuf::input::KufInputDriver::Instance()) keys->BeginKeyCapture();
  if (g.page == Page::kButtons) {
    if (auto* pads = kuf::input::PadRemapDriver::Instance()) pads->BeginCapture();
  }
  REXLOG_INFO("[kuf-pc-menu] waiting for a {} for {}", g.page == Page::kKeys ? "key" : "button",
              g.page == Page::kKeys ? kKeyActions[action].name : kButtonActions[action].name);
}

void EndCapture(MenuState& g) {
  g.capturing = false;
  if (auto* keys = kuf::input::KufInputDriver::Instance()) keys->EndKeyCapture();
  if (auto* pads = kuf::input::PadRemapDriver::Instance()) pads->EndCapture();
}

std::string LabelOf(const MenuState& g, Page page, uint32_t row) {
  const auto s = kuf::pc::Current();
  switch (page) {
    case Page::kKeys: return KeysLabel(g, g.top + row);
    case Page::kButtons: return ButtonsLabel(g, g.top + row);
    case Page::kCategories: return kCategoryLabels[row];
    case Page::kDisplay: return g.top + row < kDisplayItems ? DisplayLabel(g, kDisplayList[g.top + row]) : "";
    default: {
      const RowDef& r = RowsOf(page)[row];
      return r.kind == Kind::kGame ? g.labels[r.slot] : ItemLabel(g, s, r.item);
    }
  }
}

// Draws a widget row's value (the label rows show theirs in the label; the game's own rows: the game's value).
void DrawValue(PPCContext& ctx, uint8_t* base, const Spec& spec, uint32_t menu, Page page, uint32_t row) {
  const auto c = kuf::pc::Current().camera;
  const RowDef& r = RowsOf(page)[row];
  const uint32_t slot = r.slot;
  if (r.kind == Kind::kGame) {
    if (slot < 4) DrawSlider(ctx, base, spec, menu, slot, LoadF(base, menu + spec.slider_values[slot]));
    if (slot == 4) Call(ctx, base, spec.draw_selector, menu);
    return;
  }
  switch (r.item) {
    case Item::kMouseH: DrawSlider(ctx, base, spec, menu, slot, c.mouse_horizontal / double(kuf::pc::kLevelMax)); break;
    case Item::kMouseV: DrawSlider(ctx, base, spec, menu, slot, c.mouse_vertical / double(kuf::pc::kLevelMax)); break;
    case Item::kStickSens:
      DrawSlider(ctx, base, spec, menu, slot, c.stick_sensitivity / double(kuf::pc::kLevelMax));
      break;
    case Item::kDeadzone:
      DrawSlider(ctx, base, spec, menu, slot, c.stick_deadzone / double(kuf::pc::kDeadzoneMax));
      break;
    case Item::kDistance:
      DrawSlider(ctx, base, spec, menu, slot,
                 double(c.distance - kuf::pc::kDistanceMin) / (kuf::pc::kDistanceMax - kuf::pc::kDistanceMin));
      break;
    case Item::kFov: DrawSlider(ctx, base, spec, menu, slot, double(FovIndex(c.field_of_view)) / kFovIndexMax); break;
    default: break;
  }
}

// Writes a page's labels and values.
// --- The list pages' scrollbar (M7) ----------------------------------------------------------------------------------
//
// Display, Key Bindings and Controller Buttons have more entries than the eight rows: beside the rows a track with a
// thumb (the game's slider track and bar images, stretched upright) shows which part is on screen; the pointer
// clicks the track (that part shown) or drags the thumb (ScrollPointer). Pages that fit show none.

struct ScrollGeometry {
  bool shown = false;
  Rect track, thumb;
};

ScrollGeometry ScrollbarOf(PPCContext& ctx, uint8_t* base, const Spec& spec, const MenuState& g, uint32_t menu) {
  ScrollGeometry geo;
  const uint32_t list = g.pc_page ? ListCount(g.page) : 0;
  const uint32_t rows = uint32_t(RowsOf(g.page).size());
  if (list <= rows) return geo;
  const uint32_t last = Row(base, spec, menu, rows - 1);
  if (!Row(base, spec, menu, 0) || !last) return geo;
  // Right of the rows' frames (the frame's right piece: row_frames + 2), as tall as the rows.
  const Rect frame = RectOf(base, Load32(base, menu + spec.rows));  // the first row's label (for its top)
  int32_t right = 0;
  if (const uint32_t piece = spec.row_frames[0] && g.scene ? Find(ctx, base, g.scene, spec.row_frames[0] + 2) : 0) {
    const Rect r = RectOf(base, piece);
    right = r.x + r.w;
  }
  if (right <= 0) right = frame.x + 905;
  const Rect bottom = RectOf(base, last);
  geo.track = {right + 8, frame.y, 14, bottom.y + bottom.h - frame.y};
  const int32_t h = std::max<int32_t>(20, int32_t(int64_t(geo.track.h) * rows / list));
  const int32_t y = geo.track.y + int32_t(int64_t(geo.track.h - h) * g.top / (list - rows));
  geo.thumb = {geo.track.x + 2, y, 10, h};
  geo.shown = true;
  return geo;
}

void UpdateScrollbar(PPCContext& ctx, uint8_t* base, const Spec& spec, const MenuState& g, uint32_t menu) {
  if (!g.scene) return;
  const uint32_t track = Find(ctx, base, g.scene, ScrollTrackId(spec));
  const uint32_t thumb = Find(ctx, base, g.scene, ScrollTrackId(spec) + 1);
  if (!track || !thumb) return;  // the layout was not extended
  const ScrollGeometry geo = ScrollbarOf(ctx, base, spec, g, menu);
  if (geo.shown) {
    MoveTo(ctx, base, track, geo.track);
    MoveTo(ctx, base, thumb, geo.thumb);
  }
  Show(ctx, base, track, geo.shown);
  Show(ctx, base, thumb, geo.shown);
}

void DrawPage(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, Page page) {
  const auto rows = RowsOf(page);
  g.brightness = LoadF(base, menu + spec.slider_values[kBrightnessSlot]);
  UpdateScrollbar(ctx, base, spec, g, menu);
  for (uint32_t row = 0; row < rows.size(); ++row) {
    const uint32_t button = Row(base, spec, menu, rows[row].slot);
    SetText(ctx, base, button, LabelOf(g, page, row));
    if (rows[row].kind == Kind::kSlider || rows[row].kind == Kind::kGame) DrawValue(ctx, base, spec, menu, page, row);
    // The display page's Apply is greyed (the game's unavailable state) while there is nothing to apply.
    const uint32_t item = page == Page::kDisplay && g.top + row < kDisplayItems ? kDisplayList[g.top + row] : 0;
    const bool greyed = page == Page::kDisplay &&
                        item == kRowApply && !g.confirming && SameDisplay(g.pending, g.loaded);
    // Only a change (setting the flag redraws the row: the highlight copies its look).
    if (button && bool(Load32(base, button + kControlState) & kStateUnavailable) != greyed) {
      Call(ctx, base, sub_82623850, button, kStateUnavailable, greyed ? 1 : 0);
    }
  }
}

// Left (-1) / Right (+1) on a PC setting's row; Select (wrap) steps a label row forward, wrapping. Returns false if
// nothing changed.
bool ChangeItem(MenuState& g, Item item, int step, bool wrap = false) {
  auto settings = kuf::pc::Current();
  if (item == Item::kPrompts) {  // Button Prompts: Auto, Keyboard, Gamepad (M7 UX fix 4)
    int& p = settings.ui.button_prompts;
    const int next = wrap ? (p + 1) % 3 : std::clamp(p + step, kuf::pc::kPromptsAuto, kuf::pc::kPromptsController);
    if (next == p) return false;
    p = next;
    kuf::pc::Update(settings);
    REXLOG_INFO("[kuf-pc-menu] button prompts {}", p == 0 ? "auto" : p == 1 ? "keyboard" : "controller");
    return true;
  }
  auto& c = settings.camera;
  auto level = [step](int v) { return std::clamp(v + step, kuf::pc::kLevelMin, kuf::pc::kLevelMax); };
  switch (item) {
    case Item::kMouseH: c.mouse_horizontal = level(c.mouse_horizontal); break;
    case Item::kMouseV: c.mouse_vertical = level(c.mouse_vertical); break;
    case Item::kStickSens: c.stick_sensitivity = level(c.stick_sensitivity); break;
    case Item::kDeadzone:
      c.stick_deadzone =
          std::clamp(c.stick_deadzone + step * kuf::pc::kDeadzoneStep, kuf::pc::kDeadzoneMin, kuf::pc::kDeadzoneMax);
      break;
    case Item::kMouseY: c.invert_mouse_y = wrap ? !c.invert_mouse_y : step > 0; break;  // Left: Normal, Right: Inverted
    case Item::kDistance:
      c.distance = std::clamp(c.distance + step * kuf::pc::kDistanceStep, kuf::pc::kDistanceMin, kuf::pc::kDistanceMax);
      break;
    case Item::kFov: {
      const int index = std::clamp(FovIndex(c.field_of_view) + step, 0, kFovIndexMax);
      c.field_of_view = index == 0 ? kuf::pc::kFovOriginal : kuf::pc::kFovMin + (index - 1) * kuf::pc::kFovStep;
      break;
    }
    case Item::kVRange: {
      const int n = int(std::size(kuf::pc::kVerticalRanges));
      const int next = RangeIndex(c.vertical_range) + step;
      c.vertical_range = kuf::pc::kVerticalRanges[wrap ? (next + n) % n : std::clamp(next, 0, n - 1)];
      break;
    }
    case Item::kRecenter: c.auto_recenter = wrap ? !c.auto_recenter : step < 0; break;  // Left: On, Right: Off
    case Item::kModern: c.modern = wrap ? !c.modern : step < 0; break;                // Left: On, Right: Off
    default: return false;
  }
  if (settings.camera == kuf::pc::Current().camera) return false;
  kuf::pc::Update(settings);
  return true;
}

// --- Navigation -------------------------------------------------------------------------------------------------

// The original highlight code on a row of the page (it shows every row again: hide what the page does not use). The
// label rows' highlight shows the label (with the value).
void SelectRow(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, uint32_t scene,
               uint32_t pc_button, uint32_t row) {
  g.row = row;
  g.extra = false;
  // The rows in this page's places first: the game puts the highlight where the selected row is.
  PlaceRows(ctx, base, spec, menu, scene, pc_button, g.page, false);
  HighlightRow(ctx, base, spec, g, menu, RowsOf(g.page)[row].slot, 0);
  ShowRowsFor(ctx, base, spec, menu, scene, pc_button, g.page, false, int32_t(RowsOf(g.page)[row].slot));
  // The highlight takes the greyed look from a greyed row (Apply with nothing to apply) but the game never clears it
  // again (its rows do not change state): the highlight's own unavailable state follows the row's.
  const uint32_t highlight = Load32(base, menu + spec.highlight);
  const uint32_t selected = Row(base, spec, menu, RowsOf(g.page)[row].slot);
  if (highlight && selected) {
    const bool greyed = Load32(base, selected + kControlState) & kStateUnavailable;
    if (bool(Load32(base, highlight + kControlState) & kStateUnavailable) != greyed) {
      Call(ctx, base, sub_82623850, highlight, kStateUnavailable, greyed ? 1 : 0);
    }
  }
  ShowHelp(ctx, base, spec, g, scene, g.page == Page::kCategories);
}

// The Restore Defaults row (the PC Settings row) on the category page.
void SelectDefaultsRow(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, uint32_t scene,
                       uint32_t pc_button) {
  g.extra = true;
  PlaceRows(ctx, base, spec, menu, scene, pc_button, g.page, false);  // the highlight goes where the row is
  HighlightPcRow(ctx, base, spec, g, menu, pc_button);
  ShowRowsFor(ctx, base, spec, menu, scene, pc_button, g.page, false, kExtraSelected);
  ShowHelp(ctx, base, spec, g, scene, true);
}

void OpenPage(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, uint32_t scene,
              uint32_t pc_button, Page page, uint32_t row) {
  if (g.confirming) RevertDisplay(g, "not kept (page left)");
  if (g.capturing) EndCapture(g);
  g.page = page;
  g.top = 0;
  if (page == Page::kDisplay) LoadDisplayPage(g);
  DrawPage(ctx, base, spec, g, menu, page);
  SelectRow(ctx, base, spec, g, menu, scene, pc_button, row);
  REXLOG_INFO("[kuf-pc-menu] {} page", PageName(page));
}

void EnterPcPage(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, uint32_t scene,
                 uint32_t pc_button) {
  for (uint32_t row = 0; row < kRows; ++row) {
    g.labels[row] = TextOf(base, Row(base, spec, menu, row));
    const uint32_t button = Row(base, spec, menu, row);
    g.unavailable[row] = button && (Load32(base, button + kControlState) & kStateUnavailable);
  }
  for (uint32_t i = 0; i < g.toggles.size(); ++i) {
    const uint32_t half = Load32(base, menu + spec.toggles + 4 * i);
    g.toggles[i] = half ? Load32(base, half + kToggleSelected) : 0;
  }
  SetText(ctx, base, pc_button, kDefaultsLabel);
  if (&spec == &kTitle) {  // the installed build (the same version the updater compares)
    if (const uint32_t version = Find(ctx, base, scene, kVersionText)) {
      SetText(ctx, base, version,
              "@(scale=0.7,0.7)Version " + std::string(kuf::version::String()) + "  |  Built " +
                  std::string(kuf::version::BuildTimeUtc()) + " UTC");
      Show(ctx, base, version, true);
    }
  }
  g.storage_available = StorageRowAvailable(base, spec, menu);
  g.pc_page = true;
  REXLOG_INFO("[kuf-pc-menu] PC Settings opened ({})", spec.name);
  OpenPage(ctx, base, spec, g, menu, scene, pc_button, Page::kCategories, 0);
}

void LeavePcPage(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, uint32_t scene,
                 uint32_t pc_button) {
  if (g.capturing) EndCapture(g);
  if (g.confirming) {
    RevertDisplay(g, "not kept (Settings left)");
  }
  ShowRowsFor(ctx, base, spec, menu, scene, pc_button, g.page, true);
  for (uint32_t row = 0; row < kRows; ++row) {
    const uint32_t button = Row(base, spec, menu, row);
    SetText(ctx, base, button, g.labels[row]);
    if (button && bool(Load32(base, button + kControlState) & kStateUnavailable) != g.unavailable[row]) {
      Call(ctx, base, sub_82623850, button, kStateUnavailable, g.unavailable[row] ? 1 : 0);
    }
  }
  for (uint32_t row = 0; row < 4; ++row) {
    DrawSlider(ctx, base, spec, menu, row, LoadF(base, menu + spec.slider_values[row]));
  }
  Call(ctx, base, spec.draw_selector, menu);
  for (uint32_t i = 0; i < g.toggles.size(); ++i) {
    if (const uint32_t half = Load32(base, menu + spec.toggles + 4 * i)) {
      Store32(base, half + kToggleSelected, g.toggles[i]);
    }
  }
  SetText(ctx, base, pc_button, kLabel);
  if (g.storage_available) {
    SetStorageRowUnavailable(ctx, base, spec, menu, false);
  }
  if (const uint32_t highlight = Load32(base, menu + spec.highlight);  // not greyed from a greyed Apply
      highlight && (Load32(base, highlight + kControlState) & kStateUnavailable)) {
    Call(ctx, base, sub_82623850, highlight, kStateUnavailable, 0);
  }
  g.pc_page = false;
  UpdateScrollbar(ctx, base, spec, g, menu);  // hidden
  g.page = Page::kCategories;
  g.extra = true;
  g.return_row = LastOriginalRow(base, spec, menu);
  HighlightRow(ctx, base, spec, g, menu, g.return_row, 0);
  HighlightPcRow(ctx, base, spec, g, menu, pc_button);
  ShowHelp(ctx, base, spec, g, scene, true);
  const bool saved = kuf::pc::SaveIfChanged();
  REXLOG_INFO("[kuf-pc-menu] PC Settings closed{}", saved ? "" : " (settings could not be saved)");
}

// Restore Defaults (the category page): the controls, the camera and the frame rate (the display stays: it changes
// the window and has its own Apply and confirmation).
void RestoreDefaults() {
  auto settings = kuf::pc::Current();
  settings.camera = CameraSettings{};
  const bool frame_changed = !(settings.frame_rate == kuf::pc::FrameRateSettings{});
  settings.frame_rate = kuf::pc::FrameRateSettings{};
  settings.bindings = kuf::pc::BindingSettings{};
  settings.ui = kuf::pc::InterfaceSettings{};
  kuf::pc::Update(settings);
  if (frame_changed) kuf::pacing::RequestFrameRate(settings.frame_rate);
  kuf::input::ApplyBindingSettings();
  REXLOG_INFO("[kuf-pc-menu] PC settings restored to the defaults (controls, camera, frame rate, bindings, prompts)");
}

// The game's own handler for the event being handled (Settings input: the game's code for its own rows, and its Back
// that closes Settings and saves its options).
// The argument: the input code to give it (the event's own code, or Left / Right for a Select on the display page's
// Brightness).
using Forward = std::function<void(uint32_t code)>;

// After the game handled an input on one of its own rows: the page's rows as the page shows them again (the game's code
// may show every row), the toggles' selection as the game left it (kept when the pages close).
void AfterGameInput(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, uint32_t scene,
                    uint32_t pc_button) {
  for (uint32_t i = 0; i < g.toggles.size(); ++i) {
    const uint32_t half = Load32(base, menu + spec.toggles + 4 * i);
    g.toggles[i] = half ? Load32(base, half + kToggleSelected) : 0;
  }
  const auto rows = RowsOf(g.page);
  ShowRowsFor(ctx, base, spec, menu, scene, pc_button, g.page, false,
              g.row < rows.size() ? int32_t(rows[g.row].slot) : -1);
}

// Input on the Settings pages (every input event is handled here; the game's own rows by the game's code).
void PcPageInput(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, uint32_t scene,
                 uint32_t code, uint32_t pc_button, const Forward& forward) {
  if (g.capturing) return;  // OnTick ends it (no input reaches the game meanwhile)
  if (g.page == Page::kDisplay) TakeTimeout(g);
  const auto rows = RowsOf(g.page);
  const uint32_t row = std::min<uint32_t>(g.row, uint32_t(rows.size()) - 1);
  const uint32_t list = ListCount(g.page);
  const RowDef& def = rows[row];
  auto redraw = [&] {  // after a value changed: the labels (values) and the highlight, which shows the label too
    DrawPage(ctx, base, spec, g, menu, g.page);
    SelectRow(ctx, base, spec, g, menu, scene, pc_button, row);
  };
  const bool game_row = !g.extra && g.page != Page::kCategories && !list && def.kind == Kind::kGame;
  const uint32_t display_item = g.page == Page::kDisplay && g.top + row < kDisplayItems ? kDisplayList[g.top + row]
                                                                                        : 0xFFFFFFFFu;
  // The game's Brightness on the display page: the game's own code with its row (slot 3) as the selected one, then
  // the page's selection back (its handler may show the highlight on its own row).
  auto forward_brightness = [&](uint32_t with) {
    Store32(base, menu + spec.selected, kBrightnessSlot);
    forward(with);
    Store32(base, menu + spec.selected, rows[row].slot);
    AfterGameInput(ctx, base, spec, g, menu, scene, pc_button);
    redraw();
  };
  auto brightness = [&] { return LoadF(base, menu + spec.slider_values[kBrightnessSlot]); };
  // One row up or down, scrolling a list page. False at the end.
  auto move = [&](int dir) {
    uint32_t r = row, top = g.top;
    if (dir < 0 && r > 0) {
      --r;
    } else if (dir < 0 && list && top > 0) {
      --top;
    } else if (dir > 0 && r + 1 < rows.size()) {
      ++r;
    } else if (dir > 0 && list && top + rows.size() < list) {
      ++top;
    } else {
      return false;
    }
    if (top != g.top) {
      g.top = top;
      DrawPage(ctx, base, spec, g, menu, g.page);
    }
    SelectRow(ctx, base, spec, g, menu, scene, pc_button, r);
    return true;
  };
  switch (code) {
    case kCodeUp:
      if (g.extra) {
        SelectRow(ctx, base, spec, g, menu, scene, pc_button, uint32_t(rows.size()) - 1);
      } else if (!move(-1)) {
        break;
      }
      Cue(ctx, base, menu, kCueMove);
      break;
    case kCodeDown:
      if (g.extra) break;
      if (move(1)) {
      } else if (g.page == Page::kCategories) {
        SelectDefaultsRow(ctx, base, spec, g, menu, scene, pc_button);
      } else {
        break;
      }
      Cue(ctx, base, menu, kCueMove);
      break;
    case kCodeLeft:
    case kCodeRight: {
      const int step = code == kCodeLeft ? -1 : 1;
      if (game_row) {  // the game's own option: the game's code changes and draws it
        forward(code);
        AfterGameInput(ctx, base, spec, g, menu, scene, pc_button);
      } else if (g.page == Page::kDisplay && display_item == kRowBrightness) {
        forward_brightness(code);
      } else if (g.page == Page::kDisplay) {
        const uint32_t item = display_item;
        if (item >= kItemFps && item <= kItemEffects) {
          if (ChangeFrameRateValue(item - kItemFps, step, false)) redraw();
        } else if (!g.confirming && ChangeDisplayValue(g, item, step)) {
          redraw();
        }
      } else if (!list && g.page != Page::kCategories && def.kind != Kind::kLink) {
        if (ChangeItem(g, def.item, step)) {
          if (def.kind == Kind::kSlider) {
            DrawValue(ctx, base, spec, menu, g.page, row);
            if (def.item == Item::kDistance || def.item == Item::kFov) redraw();  // the label shows the value
          } else {
            redraw();
          }
        }
      }
      break;
    }
    case kCodeSelect:
      if (g.page == Page::kCategories) {
        if (g.extra) {
          RestoreDefaults();
        } else {
          OpenPage(ctx, base, spec, g, menu, scene, pc_button, kCategoryPages[row], 0);
        }
        Cue(ctx, base, menu, kCueSelect);
      } else if (game_row) {
        forward(code);
        AfterGameInput(ctx, base, spec, g, menu, scene, pc_button);
      } else if (!list && def.kind == Kind::kLink) {
        OpenPage(ctx, base, spec, g, menu, scene, pc_button, def.item == Item::kLinkKeys ? Page::kKeys : Page::kButtons,
                 0);
        Cue(ctx, base, menu, kCueSelect);
      } else if (g.page == Page::kDisplay && display_item == kRowBrightness) {
        // Select steps forward; from the highest back to the lowest (the game's Right / Left)
        const float before = brightness();
        forward_brightness(kCodeRight);
        for (int i = 0; i < 16 && brightness() == before; ++i) {
          const float now = brightness();
          forward_brightness(kCodeLeft);
          if (brightness() == now) break;
        }
        Cue(ctx, base, menu, kCueSelect);
      } else if (g.page == Page::kDisplay && display_item == kRowApply) {
        if (g.confirming) {
          KeepDisplay(g);
        } else {
          ApplyDisplay(g);
        }
        redraw();
        Cue(ctx, base, menu, kCueSelect);
      } else if (g.page == Page::kDisplay) {  // Select steps forward and wraps (the mouse alone can set every row)
        const uint32_t item = display_item;
        bool changed = false;
        if (item >= kItemFps && item <= kItemEffects) {
          changed = ChangeFrameRateValue(item - kItemFps, 1, true);
        } else if (!g.confirming) {
          if (item == kRowWidescreen || item == kRowVsync) {  // a toggle: Left is On
            const bool on = item == kRowVsync ? kuf::pc::Current().display.vsync : g.pending.widescreen;
            changed = ChangeDisplayValue(g, item, on ? 1 : -1);
          } else if (!(changed = ChangeDisplayValue(g, item, 1))) {  // at the end: back to the first value
            while (ChangeDisplayValue(g, item, -1)) changed = true;
          }
        }
        if (changed) {
          redraw();
          Cue(ctx, base, menu, kCueSelect);
        }
      } else if (g.page == Page::kKeys || g.page == Page::kButtons) {  // wait for the key or button (OnTick)
        g.menu = menu;
        g.pc_button = pc_button;
        BeginCapture(g, g.top + row);
        redraw();
        Cue(ctx, base, menu, kCueSelect);
      } else if (def.kind == Kind::kLabel) {
        if (ChangeItem(g, def.item, 1, true)) {  // the mouse: a click steps the value
          redraw();
          Cue(ctx, base, menu, kCueSelect);
        }
      }
      break;
    case kCodeBack:
      if (g.page == Page::kDisplay && g.confirming) {
        RevertDisplay(g, "not kept (Back)");
        redraw();
        Cue(ctx, base, menu, kCueBack);
      } else if (g.page != Page::kCategories) {  // back to the parent page, the row it was opened from selected
        const Page parent = ParentOf(g.page);
        OpenPage(ctx, base, spec, g, menu, scene, pc_button, parent, RowIn(parent, g.page));
        Cue(ctx, base, menu, kCueBack);
      } else {  // the categories: Settings closes (the game's Back: its options saved if they changed)
        LeavePcPage(ctx, base, spec, g, menu, scene, pc_button);
        g.closing = true;
        forward(code);
      }
      break;
    default:
      break;
  }
}

// An input event for a Settings menu. Returns true if handled here (the original must not run).
bool SettingsInput(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, uint32_t menu, uint32_t scene,
                   uint32_t code, const Forward& forward) {
  const uint32_t pc_button = Find(ctx, base, scene, spec.pc_button_id);
  if (!pc_button) {
    return false;  // the layout was not extended
  }
  g.menu = menu;  // for the pointer's sliders (PointerSlider)
  g.pc_button = pc_button;
  if (g.pc_page) {
    PcPageInput(ctx, base, spec, g, menu, scene, code, pc_button, forward);
    return true;
  }
  return false;  // the game's own page (shown until the next tick opens the categories, or when closing)
}

MenuState& StateFor(MenuState& g, uint32_t scene) {
  if (scene != g.scene) {
    g = MenuState{};
    g.scene = scene;
  }
  return g;
}

}  // namespace

namespace kuf::pc_menu {

uint32_t TextOverride(uint32_t number) {
  if (const uint32_t title = kuf::title_menu::TextOverride(number)) return title;
  return REXCVAR_GET(kuf_pc_settings_menu) && number == kLabelText ? g_label : 0;
}

bool BeforeSettingsEvent(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(kuf_pc_settings_menu)) {
    return false;
  }
  const uint32_t menu = ctx.r3.u32, scene = ctx.r4.u32, event = ctx.r5.u32, r6 = ctx.r6.u32;
  if (menu >= 0x10000) {
    g_ingame_menu = menu;  // the options menu (its Settings scene at +44), for PointerSlider
    kuf::controls_screen::NoteOptionsMenu(menu);  // and its Controls page (UI polish)
  }
  if (menu < 0x10000 || event < 0x10000 || scene == 0 || scene != Load32(base, menu + kInGame.scene)) {
    return false;
  }
  MenuState& g = StateFor(g_state[0], scene);
  g.menu = menu;
  if (Load32(base, event + kEventMessage) != kMessageInput) {
    if (g.pc_page) {
      kuf::pc::SaveIfChanged();  // the scene is going away with the PC page open: keep what was changed
    }
    return false;
  }
  const uint32_t event_code = Load32(base, event + kEventCode);
  const Forward forward = [&](uint32_t code) {  // the game's handler for this event (the arguments as they came)
    Store32(base, event + kEventCode, code);
    ctx.r3.u64 = menu;
    ctx.r4.u64 = scene;
    ctx.r5.u64 = event;
    ctx.r6.u64 = r6;
    __imp__sub_82663FA0(ctx, base);
    Store32(base, event + kEventCode, event_code);
  };
  if (SettingsInput(ctx, base, kInGame, g, menu, scene, Load32(base, event + kEventCode), forward)) {
    ctx.r3.u64 = 0;
    return true;
  }
  ctx.r3.u64 = menu;  // the arguments as they came (calls made here change the registers)
  ctx.r4.u64 = scene;
  ctx.r5.u64 = event;
  ctx.r6.u64 = r6;
  return false;
}

namespace {

constexpr uint32_t kFocusedControl = 0x82ABBB90;  // the toolkit's focused control (menu_pointer.cpp)
constexpr int32_t kSliderReach = 8;               // pixels above and below a track that still take a click


// The scrollbar's thumb being dragged (independent of a slider's drag: each has its own press).
struct ScrollDrag {
  bool active = false;
  size_t state = 0;
  int32_t grab = 0;  // the pointer's distance from the thumb's top
};
ScrollDrag g_scroll;

// Shows the list from entry `top` (clamped): the selected entry stays selected while on screen, else the nearest row.
void ScrollTo(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, int32_t top) {
  const int32_t rows = int32_t(RowsOf(g.page).size()), list = int32_t(ListCount(g.page));
  top = std::clamp(top, 0, std::max(0, list - rows));
  if (uint32_t(top) == g.top) return;
  const int32_t item = int32_t(g.top + g.row);
  g.top = uint32_t(top);
  DrawPage(ctx, base, spec, g, g.menu, g.page);
  SelectRow(ctx, base, spec, g, g.menu, g.scene, g.pc_button, uint32_t(std::clamp(item - top, 0, rows - 1)));
}

// The scrollbar with the pointer: a press on the thumb drags it, on the track shows that part (the thumb centred
// there, then dragged while held). True while it has the press.
bool ScrollPointer(PPCContext& ctx, uint8_t* base, const Spec& spec, MenuState& g, size_t index, int32_t px,
                   int32_t py, bool click, bool held) {
  if (g_scroll.active && (!held || g_scroll.state != index || !g.pc_page)) {
    g_scroll.active = false;
    return false;
  }
  if (!g_scroll.active && !click) return false;
  const ScrollGeometry geo = ScrollbarOf(ctx, base, spec, g, g.menu);
  if (!geo.shown) {
    g_scroll.active = false;
    return false;
  }
  if (!g_scroll.active) {
    const Rect hit{geo.track.x - 6, geo.track.y, geo.track.w + 12, geo.track.h};
    if (!hit.Contains(px, py)) return false;
    g_scroll = {true, index, geo.thumb.Contains(px, py) ? py - geo.thumb.y : geo.thumb.h / 2};
    REXLOG_INFO("[kuf-pc-menu] scrollbar ({}): {}", PageName(g.page), geo.thumb.Contains(px, py) ? "thumb" : "track");
  }
  const int32_t rows = int32_t(RowsOf(g.page).size()), list = int32_t(ListCount(g.page));
  const int32_t room = std::max(1, geo.track.h - geo.thumb.h);
  const int32_t y = py - g_scroll.grab - geo.track.y;
  ScrollTo(ctx, base, spec, g, int32_t((int64_t(y) * (list - rows) + room / 2) / room));
  return true;
}

struct Drag {
  bool active = false;
  size_t state = 0;  // g_state index
  uint32_t slot = 0;
};
Drag g_drag;

// The Settings instance on screen (its scene has the focus: the focused control's scene record, +0 the scene ID), or
// null. The menu object: the options menu / title menu seen last (their Settings scene instance at +44).
constexpr uint32_t kSettingsSceneIds[] = {0x02700000, 0x04100000};  // in-game, title (ui_layout.h IDs)
MenuState* FocusedSettings(const uint8_t* base, size_t* index) {
  const uint32_t focus = Load32(base, kFocusedControl);
  if (focus < 0x10000) return nullptr;
  const uint32_t record = Load32(base, focus + 48);
  if (record < 0x10000) return nullptr;
  const uint32_t id = Load32(base, record);
  const uint32_t candidates[] = {g_ingame_menu, kuf::title_menu::Menu()};
  for (size_t i = 0; i < std::size(g_state); ++i) {
    const uint32_t menu = candidates[i];
    if (id != kSettingsSceneIds[i] || menu < 0x10000) continue;
    const uint32_t scene = Load32(base, menu + (i == 0 ? kInGame : kTitle).scene);
    if (scene < 0x10000) return nullptr;
    MenuState& g = StateFor(g_state[i], scene);
    g.menu = menu;
    *index = i;
    return &g;
  }
  return nullptr;
}

// The slider slots of the page on screen: slot -> the row the game's selection or the PC page uses for it.
bool SliderRow(const MenuState& g, uint32_t slot, uint32_t* row) {
  if (!g.pc_page) {  // the game's own Settings page: rows 0-3 are its sliders
    *row = slot;
    return slot < 4;
  }
  const auto rows = RowsOf(g.page);
  for (uint32_t r = 0; r < rows.size(); ++r) {
    // The PC sliders, and the game's own sliders (slots 0-3: drawn and changed by the game's code).
    if (rows[r].slot == slot && (rows[r].kind == Kind::kSlider || (rows[r].kind == Kind::kGame && slot < 4))) {
      *row = r;
      return true;
    }
  }
  return false;
}

uint32_t SelectedRowOf(const uint8_t* base, const Spec& spec, const MenuState& g) {
  return g.pc_page ? (g.extra ? 0xFFFFFFFFu : g.row) : (g.extra ? 0xFFFFFFFFu : Load32(base, g.menu + spec.selected));
}

// Steps the slider of `slot` (its row selected) towards the pointer's x: Left / Right through the menu's own input,
// so each setting keeps its minimum, maximum, step and what a change does; stops at the step whose knob is nearest.
void SlideTo(PPCContext& ctx, uint8_t* base, const Spec& spec, const MenuState& g, uint32_t slot, int32_t px) {
  const uint32_t knob_at = g.menu + spec.slider_knob + 4 * slot;
  auto knob_x = [&] {
    const Rect k = RectOf(base, Load32(base, knob_at));
    return k.x + k.w / 2;
  };
  int32_t x = knob_x();
  for (int i = 0; i < 64 && x != px; ++i) {
    const int dir = px > x ? 1 : -1;
    if (!kuf::menu_pointer::Dispatch(ctx, base, dir > 0 ? kCodeRight : kCodeLeft)) return;
    const int32_t next = knob_x();
    if (next == x) return;  // at the end of its range
    if ((px - next) * dir < 0) {  // passed the pointer: keep the nearer step
      if (std::abs(px - next) > std::abs(px - x)) kuf::menu_pointer::Dispatch(ctx, base, dir > 0 ? kCodeLeft : kCodeRight);
      return;
    }
    x = next;
  }
}

// The row of a slot on the page on screen (the game's own Settings page: the slot itself), false if not shown there.
bool RowOfSlot(const MenuState& g, uint32_t slot, uint32_t* row) {
  if (!g.pc_page) {
    *row = slot;
    return slot < kRows;
  }
  const auto rows = RowsOf(g.page);
  for (uint32_t r = 0; r < rows.size() && !ListCount(g.page); ++r) {
    if (rows[r].slot == slot && rows[r].kind == Kind::kGame) {
      *row = r;
      return true;
    }
  }
  return false;
}

// Selects `row` with the menu's own Up / Down (as the slider's click); true when it is selected.
bool SelectByPointer(PPCContext& ctx, uint8_t* base, const Spec& spec, const MenuState& g, uint32_t row) {
  for (int i = 0; i < 16 && SelectedRowOf(base, spec, g) != row; ++i) {
    const uint32_t at = SelectedRowOf(base, spec, g);
    if (!kuf::menu_pointer::Dispatch(ctx, base, at == 0xFFFFFFFFu || at > row ? kCodeUp : kCodeDown)) break;
    if (SelectedRowOf(base, spec, g) == at) break;
  }
  return SelectedRowOf(base, spec, g) == row;
}

// The game's choice widgets with the mouse (M7 UI follow-up; the player's screenshot of Gameplay: Skin's arrows and
// Blood's On / Off did not take clicks): the selector's arrows (slot 4) and the toggles' two halves (slots 5-7). A
// click selects the widget's row, then gives that row the game's own Left (the left arrow, the first half: On /
// Normal) or Right (the right arrow, the second half) through the menu's input handler, as the D-pad does: the
// game's code keeps its values, wrapping and saving. Hover over a widget selects its row. Rectangles are the
// controls' own (the layout's 1280 x 720 space the pointer is mapped into, at any window size).
bool PointerWidget(PPCContext& ctx, uint8_t* base, int32_t px, int32_t py, bool click) {
  size_t index = 0;
  MenuState* g = FocusedSettings(base, &index);
  if (!g || g->capturing) return false;
  const Spec& spec = index == 0 ? kInGame : kTitle;
  for (uint32_t slot = 4; slot < kRows; ++slot) {
    uint32_t row = 0;
    if (!RowOfSlot(*g, slot, &row)) continue;
    const auto c = SlotControls(base, spec, g->menu, slot);
    const uint32_t first = slot == 4 ? c[2] : c[1], second = slot == 4 ? c[3] : c[2];
    for (const uint32_t part : {first, second}) {
      if (!part || !(Load32(base, part + kControlState) & 1) || !RectOf(base, part).Contains(px, py)) continue;
      const uint32_t other = part == first ? second : first;
      const bool left = !other || RectOf(base, part).x <= RectOf(base, other).x;
      if (!SelectByPointer(ctx, base, spec, *g, row)) return click;  // taken: no stray Select on another row
      if (!click) return true;
      kuf::menu_pointer::Dispatch(ctx, base, left ? kCodeLeft : kCodeRight);
      REXLOG_INFO("[kuf-pc-menu] slot {} ({}): {} clicked", slot, g->pc_page ? PageName(g->page) : "Settings",
                  slot == 4 ? (left ? "left arrow" : "right arrow") : (left ? "first half" : "second half"));
      return true;
    }
  }
  return false;
}

}  // namespace

bool PointerSlider(PPCContext& ctx, uint8_t* base, int32_t px, int32_t py, bool click, bool held, bool moved) {
  if (!REXCVAR_GET(kuf_pc_settings_menu)) return false;
  if (!held && !click) {
    g_scroll.active = false;
    if (moved && !g_drag.active) return PointerWidget(ctx, base, px, py, false);
    if (g_drag.active) REXLOG_INFO("[kuf-pc-menu] slider {}: released", g_drag.slot);
    g_drag.active = false;
    return false;
  }
  size_t index = 0;
  MenuState* g = FocusedSettings(base, &index);
  if (!g || g->capturing) {
    g_drag.active = false;
    return false;
  }
  const Spec& spec = index == 0 ? kInGame : kTitle;
  if (!g_drag.active && ScrollPointer(ctx, base, spec, *g, index, px, py, click, held)) return true;
  if (g_drag.active) {
    if (g_drag.state != index) {
      g_drag.active = false;
      return false;
    }
    SlideTo(ctx, base, spec, *g, g_drag.slot, px);
    return true;
  }
  if (!click) return false;
  if (REXCVAR_GET(kuf_trace_menu_pointer)) {  // the slider areas the click was compared with
    std::string areas;
    for (uint32_t slot = 0; slot < 4; ++slot) {
      uint32_t row = 0;
      const uint32_t track = Find(ctx, base, g->scene, spec.slider_tracks[slot]);
      const Rect t = RectOf(base, track), k = RectOf(base, Load32(base, g->menu + spec.slider_knob + 4 * slot));
      areas += fmt::format(" [{}: {} track ({},{} {}x{}) knob ({},{} {}x{})]", slot,
                           SliderRow(*g, slot, &row) && track && (Load32(base, track + kControlState) & 1) ? "slider"
                                                                                                         : "off",
                           t.x, t.y, t.w, t.h, k.x, k.y, k.w, k.h);
    }
    REXLOG_INFO("[kuf-pointer] click at ({},{}): sliders{}", px, py, areas);
  }
  // A click on a shown slider's track or knob: its row selected (as Up / Down would), then the value.
  for (uint32_t slot = 0; slot < 4; ++slot) {
    uint32_t row = 0;
    if (!SliderRow(*g, slot, &row)) continue;
    const uint32_t track = Find(ctx, base, g->scene, spec.slider_tracks[slot]);
    const uint32_t knob = Load32(base, g->menu + spec.slider_knob + 4 * slot);
    if (!track || !(Load32(base, track + kControlState) & 1)) continue;  // hidden on this page
    if (!RectOf(base, track).Contains(px, py, kSliderReach) && !RectOf(base, knob).Contains(px, py, kSliderReach)) {
      continue;
    }
    for (int i = 0; i < 16 && SelectedRowOf(base, spec, *g) != row; ++i) {
      const uint32_t at = SelectedRowOf(base, spec, *g);
      if (!kuf::menu_pointer::Dispatch(ctx, base, at == 0xFFFFFFFFu || at > row ? kCodeUp : kCodeDown)) break;
      if (SelectedRowOf(base, spec, *g) == at) break;
    }
    if (SelectedRowOf(base, spec, *g) != row) return false;
    g_drag = {true, index, slot};
    SlideTo(ctx, base, spec, *g, slot, px);
    REXLOG_INFO("[kuf-pc-menu] slider {} ({}): set from the pointer", slot, g->pc_page ? PageName(g->page) : "Settings");
    return true;
  }
  return PointerWidget(ctx, base, px, py, true);
}

void OnTick(PPCContext& ctx, uint8_t* base) {
  // Settings opens on its categories (the game's own list is never shown: its rows are on the category pages). Back
  // on the categories lets the game close Settings; no re-entry until the Settings scene is gone.
  size_t focused = 0;
  if (!REXCVAR_GET(kuf_pc_settings_menu)) {
    // the game's own Settings menus
  } else if (MenuState* s = FocusedSettings(base, &focused)) {
    const Spec& spec = focused == 0 ? kInGame : kTitle;
    if (!s->pc_page && !s->closing) {
      if (const uint32_t pc_button = Find(ctx, base, s->scene, spec.pc_button_id)) {
        s->pc_button = pc_button;
        EnterPcPage(ctx, base, spec, *s, s->menu, s->scene, pc_button);
      }
    }
  } else {
    for (MenuState& s : g_state) s.closing = false;
  }
  for (size_t i = 0; i < std::size(g_state); ++i) {
    MenuState& g = g_state[i];
    if (!g.capturing) continue;
    const Spec& spec = i == 0 ? kInGame : kTitle;
    auto* keys = kuf::input::KufInputDriver::Instance();
    auto* pads = kuf::input::PadRemapDriver::Instance();
    const kuf::input::HostKey key = keys ? keys->TakeKeyCapture() : 0;
    const auto button = pads && g.page == Page::kButtons ? pads->TakeCapture() : std::nullopt;
    const bool timed_out = std::chrono::steady_clock::now() - g.capture_start > std::chrono::seconds(kCaptureSeconds);
    if (!key && !button && !timed_out) continue;
    const uint32_t action = g.capture_action;
    EndCapture(g);
    if (g.page == Page::kKeys && key && key != kuf::input::ParseHostKey("Escape") && !IsReservedKey(key)) {
      AssignKey(action, key);
    } else if (g.page == Page::kButtons && button) {
      AssignButton(action, *button);
    } else {
      REXLOG_INFO("[kuf-pc-menu] {} (nothing changed)", timed_out ? "no input in time"
                                                        : key && IsReservedKey(key) ? "that key is reserved"
                                                                                     : "cancelled");
    }
    // The page as it is now: labels, then the highlight (which shows the selected label).
    DrawPage(ctx, base, spec, g, g.menu, g.page);
    SelectRow(ctx, base, spec, g, g.menu, g.scene, g.pc_button, g.row);
  }
}

bool BeforeTitleSettingsEvent(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(kuf_pc_settings_menu)) {
    return false;
  }
  const uint32_t menu = ctx.r3.u32, event = ctx.r4.u32;
  if (menu < 0x10000 || event < 0x10000) {
    return false;
  }
  const uint32_t scene = Load32(base, menu + kTitle.scene);
  if (scene == 0) {
    return false;
  }
  MenuState& g = StateFor(g_state[1], scene);
  const uint32_t event_code = Load32(base, event + kEventCode);
  const Forward forward = [&](uint32_t code) {
    Store32(base, event + kEventCode, code);
    ctx.r3.u64 = menu;
    ctx.r4.u64 = event;
    __imp__sub_82678280(ctx, base);
    Store32(base, event + kEventCode, event_code);
  };
  if (SettingsInput(ctx, base, kTitle, g, menu, scene, Load32(base, event + kEventCode), forward)) {
    return true;
  }
  ctx.r3.u64 = menu;
  ctx.r4.u64 = event;
  return false;
}

}  // namespace kuf::pc_menu
