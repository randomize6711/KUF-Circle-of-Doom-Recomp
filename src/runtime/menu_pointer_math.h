// The menu pointer's decision (M7 feature 2, src/runtime/menu_pointer.cpp): from the focused scene's buttons, the
// focused control and the pointer, the next navigation step that brings the game's own selection under the pointer.
// Pure (no guest memory): unit tests in tests/ui.
#pragma once

#include <cstdint>
#include <vector>

namespace kuf::menu_pointer {

struct UiButton {
  uint32_t id = 0;
  int32_t x = 0, y = 0, w = 0, h = 0;  // 1280 x 720 layout space
  uint32_t state = 0;                   // +56: 1 shown, 0x10 unavailable
};

constexpr uint32_t kStateShown = 0x1;
constexpr uint32_t kStateUnavailable = 0x10;

struct Step {
  bool over_row = false;     // the pointer is over a row of the focused control's group
  bool on_selected = false;  // ... over the selected one
  bool available = false;    // the row under the pointer can be selected
  int dx = 0, dy = 0;        // the next press: -1 left / up, +1 right / down, 0 none (at most one of them)
  int distance = 0;          // presses still needed (rows between, counting only available ones)
};

// The focused control's group: the scene's buttons with the focused control's size, or in its column with its height
// (an action row wider than the value rows, as the Settings menus' last rows). Its rows are their distinct
// positions (a highlight drawn over the selected row, or the row hidden under it, is the same row); the selected row is
// the focused control's position. A column (one x) steps with Up/Down, a line (one y) with Left/Right, a grid with
// both (columns first). A position is a row if a button there is shown, or it is the selected one; it is available if
// a button there is not unavailable. Rows are counted between the selected one and the target over available rows
// only (the game's Up/Down skip or stop at unavailable ones; the caller re-plans after each press).
Step Plan(const std::vector<UiButton>& scene_buttons, const UiButton& focus, int32_t px, int32_t py);

// A list box (its rows drawn inside the one control): +408 the selected index, +404 the first visible row, +384 the
// visible rows, +380 a row's height, the item vector +392..+396 (sub_82626EE8 keeps the selection visible).
struct ListBox {
  int32_t x = 0, y = 0, w = 0, h = 0;
  int32_t count = 0, index = 0, top = 0, visible = 0, row_height = 0;
};

// The row under the pointer among the visible ones; Up/Down towards it (the list scrolls only by the wheel).
Step PlanList(const ListBox& list, int32_t px, int32_t py);

}  // namespace kuf::menu_pointer
