// Reading and extending the original menu layouts ("BlueSide UI Export File v0.001", d:\data\Menu\Ui*.uitb) in
// memory. Format (docs/MENU_ARCHITECTURE.md section 2, verified on all 24 layout files): a 44-byte header (scene
// count at +40), then per scene a 100-byte record (type 0, ID at +4, child count at +88) followed by its
// (control ID, x, y) placements, then exactly those controls' records in the same order. Control records start
// with their type and ID: button (1) 136 bytes, list box (2) 180 or 308 bytes, text (4) 92, image (5) 380. All
// values are big-endian. The game files are never changed: these functions work on a copy.

#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <span>
#include <string>
#include <vector>

namespace kuf::ui {

struct Placement {
  uint32_t id;
  int32_t x, y;
};

struct ControlRecord {
  uint32_t type, id;
  std::vector<uint8_t> bytes;  // the whole record, starting with type and ID
};

struct Scene {
  std::vector<uint8_t> header;  // 100 bytes
  std::vector<Placement> placements;
  std::vector<ControlRecord> controls;  // same order as placements
  uint32_t id() const;
};

struct Layout {
  std::vector<uint8_t> header;  // 44 bytes
  std::vector<Scene> scenes;
};

// Parses a layout; nullopt (with the reason) if anything does not match the format above.
std::optional<Layout> Parse(std::span<const uint8_t> data, std::string* error);
std::vector<uint8_t> Serialize(const Layout& layout);

uint32_t Get32(std::span<const uint8_t> bytes, size_t offset);
void Put32(std::span<uint8_t> bytes, size_t offset, uint32_t value);

// The in-game Settings scene (SN_PROFILE_OPTION_CONFIG) with one more row below Vibration: an action row (label
// button and frame cloned from the Vibration row with new IDs, without its On/Off halves), the panel grown and
// the button help below it moved down by one row. The label gets text number `label_text` (0: keep the
// Vibration label). Used for "PC Settings" (src/runtime/pc_settings_menu.cpp).
constexpr uint32_t kPcRowButton = 0x2701080;
constexpr int32_t kRowSpacing = 46;  // between the Settings rows (all 8 rows of the original scene)
std::optional<std::vector<uint8_t>> AddSettingsActionRow(std::span<const uint8_t> data, uint32_t label_text,
                                                         std::string* error);

// The title SETTINGS scene (SN_TITLE_OPTION, d:\data\Menu\UiTitle.uitb) with one more row below Change Storage
// Device: its frame and label cloned with new IDs, one row lower (the panel already extends below it, the help
// text stays). Label text number as above.
constexpr uint32_t kTitlePcRowButton = 0x4101080;
std::optional<std::vector<uint8_t>> AddTitleSettingsActionRow(std::span<const uint8_t> data, uint32_t label_text,
                                                              std::string* error);

// The title list (scene 0x039: NEW GAME ... CREDITS, d:\data\Menu\UiTitle.uitb) with `labels.size()` more rows
// below CREDITS (M7 feature 4): each the CREDITS row's label button and frame image cloned with new IDs (buttons
// kTitleMainRowButton + k, images kTitleMainRowImage + 3 k), label text number labels[k]; the whole list (rows,
// frames, the highlight) moved up by one row per added row so that it still ends where it did.
constexpr uint32_t kTitleMainRowButton = 0x390100A;
constexpr uint32_t kTitleMainRowImage = 0x3905020;
constexpr int32_t kTitleRowSpacing = 28;  // between the title list's rows
std::optional<std::vector<uint8_t>> AddTitleMainRows(std::span<const uint8_t> data,
                                                     const std::vector<uint32_t>& labels, std::string* error);

// A copy of a scene's text control with a new ID (M7 UX fix 4: the PC Settings descriptions), placed at (x, y) with
// size w x h, its text set by code (text number -1), font preset `font` (0: the source's); added as the scene's last
// control (drawn over the others).
std::optional<std::vector<uint8_t>> AddTextClone(std::span<const uint8_t> data, uint32_t scene_id, uint32_t source,
                                                 uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                                                 uint32_t font, std::string* error);

// A copy of a scene's image control with a new ID (M7 menu overhaul: the level-up dialog's backdrop and its panel
// made more opaque), inserted just before control `before` (drawn under it; 0: added last, drawn over the others).
// `place` false keeps the source's place and size, else (x, y) and w x h; `alpha` 0-255 (-1: the source's).
struct ImageClone {
  uint32_t source, id;
  bool place = false;
  int32_t x = 0, y = 0, w = 0, h = 0;
  int32_t alpha = -1;
};
std::optional<std::vector<uint8_t>> AddImageClones(std::span<const uint8_t> data, uint32_t scene_id,
                                                   const std::vector<ImageClone>& clones, uint32_t before,
                                                   std::string* error);

// The scene's placements of `ids` moved by (dx, dy) (M7 UX fix 5: the title's Change Storage Device row moved off
// screen and PC Settings up into its place). False (unchanged) if one of them is missing.
std::optional<std::vector<uint8_t>> MoveControls(std::span<const uint8_t> data, uint32_t scene_id,
                                                 const std::vector<uint32_t>& ids, int32_t dx, int32_t dy,
                                                 std::string* error);

// Copies of a scene's button control (type 1) with new IDs, each placed at (x, y), its text set by code (the source's
// text number kept until then): the update box's three buttons (src/runtime/update_box.cpp) from the message box's
// own. Images the source button owns (its state images, +100..+132, whose owner field +376 is the source) are copied
// too, with IDs `id + 0x4000 + k`, at the same offset from their button. Added after the source.
struct ButtonClone {
  uint32_t id;
  int32_t x, y;
};
std::optional<std::vector<uint8_t>> AddButtonClones(std::span<const uint8_t> data, uint32_t scene_id, uint32_t source,
                                                    const std::vector<ButtonClone>& clones, std::string* error);
// The game's message box (scene 0x025: the message text, the answer line button 0x02501001, a second text below).
constexpr uint32_t kMessageBoxScene = 0x02500000;
constexpr uint32_t kMessageBoxButton = 0x02501001;
constexpr uint32_t kUpdateBoxButtons[3] = {0x02501010, 0x02501011, 0x02501012};  // Install, GitHub, Cancel

// The multiplayer menus' columns on the game type screen (scene 0x02F, UiStartGame.uitb; src/runtime/lobby_screen.cpp):
// for each of its six rows a label text (kLobbyLabelText + row) and a value text (kLobbyValueText + row), copies of
// the screen's description line (left-aligned, the rows' text size), at fixed places: the label column from the rows'
// left edge, the value column at a fixed offset, both on the row's own line. Fixed: nothing a row shows moves them.
constexpr uint32_t kLobbyScene = 0x02F00000;
constexpr uint32_t kLobbyRowButtons[6] = {0x02F01001, 0x02F01002, 0x02F01003, 0x02F01004, 0x02F01006, 0x02F01007};
constexpr uint32_t kLobbyTextSource = 0x02F04001;  // the description line
constexpr uint32_t kLobbyLabelText = 0x02F04010, kLobbyValueText = 0x02F04020;
constexpr int32_t kLobbyLabelInset = 20;     // from the rows' left edge
constexpr int32_t kLobbyLabelWidth = 230;
constexpr int32_t kLobbyValueOffset = 270;   // the value column, from the rows' left edge
constexpr int32_t kLobbyValueWidth = 340;
constexpr int32_t kLobbyRowHeight = 30;
std::optional<std::vector<uint8_t>> AddLobbyColumns(std::span<const uint8_t> data, std::string* error);

}  // namespace kuf::ui
