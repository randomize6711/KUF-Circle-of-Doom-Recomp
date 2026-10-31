// The menu pointer's plan (M7 feature 2, src/runtime/menu_pointer_math.cpp) on button layouts as the game's menus
// have them (traced with --kuf_trace_menu_pointer: the title list, a highlight over a hidden selected row, a line of
// tabs, a grid).
#include <catch2/catch_test_macros.hpp>

#include "runtime/menu_pointer_math.h"

using kuf::menu_pointer::Plan;
using kuf::menu_pointer::UiButton;

namespace {
// The title list: seven rows 500 x 32 at x 390, y 482 + 28 k; the third (y 538) unavailable; the highlight button
// over the selected row. Focus is the selected row itself here.
std::vector<UiButton> TitleList(int selected) {
  std::vector<UiButton> b;
  for (int k = 0; k < 7; ++k) b.push_back({0x03901001u + k, 390, 482 + 28 * k, 500, 32, k == 2 ? 0x93u : 0x83u});
  b.push_back({0x03901007, 390, 482 + 28 * selected, 500, 32, 0x3});
  return b;
}
}  // namespace

TEST_CASE("menu pointer: over the selected row, nothing to move", "[ui]") {
  const auto b = TitleList(0);
  const auto s = Plan(b, b[0], 400, 490);
  CHECK(s.over_row);
  CHECK(s.on_selected);
  CHECK(s.dx == 0);
  CHECK(s.dy == 0);
}

TEST_CASE("menu pointer: a row below steps down, unavailable rows not counted", "[ui]") {
  const auto b = TitleList(0);
  const auto s = Plan(b, b[0], 600, 594 + 10);  // the fifth row (index 4)
  CHECK(s.over_row);
  CHECK_FALSE(s.on_selected);
  CHECK(s.available);
  CHECK(s.dy == 1);
  CHECK(s.dx == 0);
  CHECK(s.distance == 3);  // rows 1, 3, 4 (row 2 unavailable)
  const auto up = Plan(TitleList(4), TitleList(4)[4], 400, 485);
  CHECK(up.dy == -1);
  CHECK(up.distance == 3);
}

TEST_CASE("menu pointer: an unavailable row is not a target; outside the rows nothing", "[ui]") {
  const auto b = TitleList(0);
  const auto s = Plan(b, b[0], 400, 540);  // the unavailable row
  CHECK(s.over_row);
  CHECK_FALSE(s.available);
  CHECK(s.dy == 0);
  const auto out = Plan(b, b[0], 100, 100);
  CHECK_FALSE(out.over_row);
  CHECK(out.dy == 0);
  const auto right = Plan(b, b[0], 890, 490);  // the rectangle's right edge is exclusive
  CHECK_FALSE(right.over_row);
}

TEST_CASE("menu pointer: a highlight over a hidden selected row (character slots)", "[ui]") {
  // Four slots 648 x 130; the selected one (y 60) hidden under the highlight, which has the focus.
  std::vector<UiButton> b = {{0x02E01001, 506, 60, 648, 130, 0x2},
                             {0x02E01002, 506, 200, 648, 130, 0x3},
                             {0x02E01003, 506, 342, 648, 130, 0x3},
                             {0x02E01004, 506, 483, 648, 130, 0x3},
                             {0x02E01005, 506, 60, 648, 130, 0x3}};
  const UiButton highlight = b[4];
  CHECK(Plan(b, highlight, 600, 100).on_selected);
  const auto s = Plan(b, highlight, 600, 500);
  CHECK(s.dy == 1);
  CHECK(s.distance == 3);
}

TEST_CASE("menu pointer: a line of tabs steps left and right", "[ui]") {
  std::vector<UiButton> b = {{1, 540, 208, 50, 42, 0x2}, {2, 592, 208, 50, 42, 0x3}, {3, 645, 208, 50, 42, 0x3},
                             {4, 540, 208, 50, 42, 0x3}};  // 4: the highlight on the first tab
  const auto s = Plan(b, b[3], 650, 220);
  CHECK(s.dx == 1);
  CHECK(s.dy == 0);
  CHECK(s.distance == 2);
  const auto back = Plan(b, {9, 645, 208, 50, 42, 0x3}, 545, 220);
  CHECK(back.dx == -1);
}

TEST_CASE("menu pointer: a grid moves along the line first, then the column", "[ui]") {
  std::vector<UiButton> b;
  for (int col = 0; col < 2; ++col) {
    for (int row = 0; row < 3; ++row) b.push_back({uint32_t(col * 10 + row), 521 + 45 * col, 265 + 43 * row, 40, 34, 3});
  }
  const auto s = Plan(b, b[0], 570, 360);  // column 1, row 2
  CHECK(s.dx == 1);
  CHECK(s.dy == 0);
  const auto then = Plan(b, b[3], 570, 360);  // after the step right
  CHECK(then.dx == 0);
  CHECK(then.dy == 1);
}

TEST_CASE("menu pointer: other buttons of another size are not rows", "[ui]") {
  auto b = TitleList(0);
  b.push_back({0x0390100A, 390, 700, 100, 20, 0x3});  // a small button elsewhere in the scene
  CHECK_FALSE(Plan(b, b[0], 400, 705).over_row);
}

TEST_CASE("menu pointer: list boxes (the Language list: 8 rows of 27 px, all visible)", "[ui]") {
  using kuf::menu_pointer::ListBox;
  using kuf::menu_pointer::PlanList;
  ListBox l{525, 460, 230, 220, 8, 2, 0, 8, 27};
  CHECK(PlanList(l, 600, 460 + 27 * 2 + 5).on_selected);
  const auto down = PlanList(l, 600, 460 + 27 * 6 + 1);
  CHECK(down.dy == 1);
  CHECK(down.distance == 4);
  CHECK(PlanList(l, 600, 461).dy == -1);
  CHECK_FALSE(PlanList(l, 500, 500).over_row);  // left of the list
  CHECK_FALSE(PlanList(l, 600, 460 + 27 * 8 + 1).over_row);  // below the last visible row
  ListBox scrolled{0, 0, 100, 100, 20, 12, 10, 4, 25};  // rows 10..13 visible
  const auto s = PlanList(scrolled, 50, 80);  // the fourth visible row: item 13
  CHECK(s.dy == 1);
  CHECK(s.distance == 1);
  ListBox short_list{0, 0, 100, 100, 2, 0, 0, 4, 25};
  CHECK_FALSE(PlanList(short_list, 50, 60).over_row);  // an empty row slot
}

TEST_CASE("menu pointer: a wider action row in the same column belongs to the list (title SETTINGS)", "[ui]") {
  // Value rows 285 x 32 at x 176 from y 135 every 46; Change Storage Device (548) and PC Settings (594) 905 wide;
  // the highlight (the focus) on the first row.
  std::vector<UiButton> b;
  for (int k = 0; k < 8; ++k) b.push_back({0x04101001u + k, 176, 135 + 46 * k, 285, 32, 3});
  b.push_back({0x0410100D, 176, 548, 905, 32, 0x13});  // Change Storage Device, unavailable (no storage device)
  b.push_back({0x04101080, 176, 594, 905, 32, 3});     // PC Settings
  const UiButton focus{0x04101008, 176, 135, 285, 32, 3};
  const auto s = Plan(b, focus, 700, 600);  // on PC Settings, right of the value rows' width
  CHECK(s.over_row);
  CHECK(s.dy == 1);
  CHECK(s.distance == 8);  // seven value rows below, the storage row skipped, PC Settings
  CHECK_FALSE(Plan(b, focus, 700, 300).over_row);  // right of a value row: not on it
}

// The game type screen (scene 0x02F, after choosing a character; traced in the M7 UI polish): each row twice (a shown
// button and a hidden twin), the co-op rows greyed, Difficulty set in from the list's column.
std::vector<UiButton> GameType() {
  return {
      {0x02F01001, 548, 236, 580, 30, 0x3},  {0x02F01002, 528, 278, 620, 30, 0x2},  // Difficulty; Continue (hidden)
      {0x02F01003, 528, 320, 620, 30, 0x13}, {0x02F01004, 528, 362, 620, 30, 0x13}, {0x02F01006, 528, 403, 620, 30, 0x13},
      {0x02F01007, 528, 445, 620, 30, 0x13}, {0x02F01005, 528, 236, 600, 30, 0x2},  {0x02F01008, 528, 278, 620, 30, 0x3},
      {0x02F01009, 528, 320, 620, 30, 0x2},  {0x02F0100A, 528, 362, 620, 30, 0x2},  {0x02F0100B, 528, 403, 620, 30, 0x2},
      {0x02F0100C, 528, 445, 620, 30, 0x2},
  };
}

TEST_CASE("menu pointer: greyed rows with hidden twins stay unavailable (game type screen)", "[ui]") {
  const auto buttons = GameType();
  const UiButton focus = buttons[7];  // Continue Solo Game
  const auto step = Plan(buttons, focus, 800, 330);  // Host Private Co-op Game: greyed
  CHECK(step.over_row);
  CHECK_FALSE(step.available);
  CHECK(step.dy == 0);
}

TEST_CASE("menu pointer: a row set in from the column is in it (Difficulty above Continue)", "[ui]") {
  const auto buttons = GameType();
  const UiButton focus = buttons[7];
  auto step = Plan(buttons, focus, 1000, 245);  // Difficulty
  CHECK(step.over_row);
  CHECK(step.available);
  CHECK(step.dy == -1);
  CHECK(step.dx == 0);
  CHECK(step.distance == 1);
  step = Plan(buttons, focus, 535, 245);  // left of Difficulty's own button but in the list's column: still the row
  CHECK(step.over_row);
}

TEST_CASE("menu pointer: a wide slot beside the focused column extends its row (inventory equipment)", "[ui]") {
  // The inventory at rest (traced during its slide-in, 640 further right): the button icons 40 x 34 at x 521, the
  // equipment slots 540 x 34 at x 620, one pixel off the icons' tops.
  const int icon_y[] = {265, 307, 350, 394}, slot_y[] = {264, 308, 350, 394};
  std::vector<UiButton> b;
  for (int k = 0; k < 4; ++k) {
    b.push_back({0x0030100Bu + k, 521, icon_y[k], 40, 34, 0x3});
    b.push_back({0x00301007u + k, 620, slot_y[k], 540, 34, 0x3});
  }
  const auto s = Plan(b, b[0], 900, 360);  // over the third slot
  CHECK(s.over_row);
  CHECK(s.available);
  CHECK(s.dx == 0);
  CHECK(s.dy == 1);
  CHECK(s.distance == 2);
  const auto on = Plan(b, b[0], 1100, 270);  // over the selected row's slot
  CHECK(on.on_selected);
  // Not a companion: a button of the row's height far to the right, or on another line.
  std::vector<UiButton> far = {{1, 521, 265, 40, 34, 0x3}, {2, 521, 307, 40, 34, 0x3}, {3, 900, 307, 200, 34, 0x3}};
  CHECK_FALSE(Plan(far, far[0], 950, 315).over_row);
  std::vector<UiButton> off = {{1, 521, 265, 40, 34, 0x3}, {2, 521, 307, 40, 34, 0x3}, {3, 570, 320, 200, 34, 0x3}};
  CHECK_FALSE(Plan(off, off[0], 700, 340).over_row);
}
