#include "runtime/menu_pointer_math.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <utility>

namespace kuf::menu_pointer {
namespace {

struct Row {
  int32_t x, y, w, h;
  bool shown = false, available = false;
};

int Sign(int v) { return (v > 0) - (v < 0); }

constexpr int32_t kLineSlack = 2;      // a companion's top and its row's (the inventory's differ by one pixel)
constexpr int32_t kCompanionGap = 120;  // at most this far right of the focused column

// Available rows strictly after `from` up to and including `to` along one sorted coordinate list.
int CountAvailable(const std::vector<std::pair<int32_t, bool>>& line, size_t from, size_t to) {
  int n = 0;
  const int dir = to > from ? 1 : -1;
  for (size_t i = from; i != to;) {
    i += dir;
    n += line[i].second ? 1 : 0;
  }
  return n;
}

}  // namespace

Step Plan(const std::vector<UiButton>& scene_buttons, const UiButton& focus, int32_t px, int32_t py) {
  Step step;
  if (focus.w <= 0 || focus.h <= 0) return step;
  std::map<std::pair<int32_t, int32_t>, Row> rows;  // (x, y) -> row
  for (const UiButton& b : scene_buttons) {
    // The group: the focused control's size, or its column and height (an action row wider than the value rows), or
    // its height and at least half of its width (a row set in from the list, as the game type screen's Difficulty):
    // those count as the focused control's column.
    const bool same_size = b.w == focus.w && b.h == focus.h;
    const bool same_column = b.x == focus.x && b.h == focus.h;
    const int32_t overlap = std::min(b.x + b.w, focus.x + focus.w) - std::max(b.x, focus.x);
    const bool overlapping = b.h == focus.h && overlap * 2 >= std::min(b.w, focus.w);
    if (!same_size && !same_column && !overlapping) continue;
    const int32_t x = same_size || same_column ? b.x : focus.x;
    Row& r = rows.try_emplace({x, b.y}, Row{x, b.y, b.w, b.h}).first->second;
    if (x == b.x) {
      r.w = std::max(r.w, b.w);
    } else {  // the row's extent covers the button too
      const int32_t right = std::max(r.x + r.w, b.x + b.w);
      r.w = right - r.x;
    }
    const bool shown = (b.state & kStateShown) != 0;
    r.shown |= shown;
    // Available: a shown button there that is not unavailable (a hidden twin of a greyed row does not make it
    // available: the game type screen keeps a hidden copy of each row).
    r.available |= shown && (b.state & kStateUnavailable) == 0;
  }
  // A row's wide companion (M7 menu overhaul: the inventory's equipment slots, 540 wide, beside the focused 40-wide
  // column of their button icons): a shown button outside the group on the same line (its height, its top within
  // kLineSlack) starting just right of the focused column extends that column's row over it.
  for (const UiButton& b : scene_buttons) {
    if (!(b.state & kStateShown) || b.h != focus.h || (b.w == focus.w && b.h == focus.h) || b.x == focus.x) continue;
    const int32_t gap = b.x - (focus.x + focus.w);
    if (gap < 0 || gap > kCompanionGap) continue;
    for (auto& [pos, r] : rows) {
      if (r.x != focus.x || std::abs(r.y - b.y) > kLineSlack) continue;
      r.w = std::max(r.w, b.x + b.w - r.x);
      break;
    }
  }
  Row& selected = rows.try_emplace({focus.x, focus.y}, Row{focus.x, focus.y, focus.w, focus.h}).first->second;
  selected.shown = true;
  selected.available = true;  // the selection is where it is
  for (auto it = rows.begin(); it != rows.end();) {  // hidden positions that are not the selected row are not rows
    it = it->second.shown ? std::next(it) : rows.erase(it);
  }
  const Row* target = nullptr;
  for (const auto& [pos, r] : rows) {
    if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) target = &r;
  }
  if (!target) return step;
  step.over_row = true;
  step.available = target->available;
  if (target->x == focus.x && target->y == focus.y) {
    step.on_selected = true;
    return step;
  }
  if (!target->available) return step;
  // Columns: the distinct x positions; lines: the distinct y positions.
  std::map<int32_t, bool> xs, ys;
  for (const auto& [pos, r] : rows) {
    xs[r.x] |= r.available;
    ys[r.y] |= r.available;
  }
  auto index_of = [](const std::map<int32_t, bool>& m, int32_t v) {
    return size_t(std::distance(m.begin(), m.find(v)));
  };
  auto line_of = [&](bool vertical, int32_t fixed) {  // the available flags along one column or line
    std::vector<std::pair<int32_t, bool>> line;
    for (const auto& [pos, r] : rows) {
      if ((vertical ? r.x : r.y) == fixed) line.push_back({vertical ? r.y : r.x, r.available});
    }
    std::sort(line.begin(), line.end());
    return line;
  };
  if (target->x != focus.x) {  // a different column first (Left/Right), along the selected row's line
    auto line = line_of(false, focus.y);
    auto from = std::find_if(line.begin(), line.end(), [&](auto& e) { return e.first == focus.x; });
    auto to = std::find_if(line.begin(), line.end(), [&](auto& e) { return e.first == target->x; });
    step.dx = Sign(target->x - focus.x);
    if (from != line.end() && to != line.end()) {
      step.distance = CountAvailable(line, size_t(from - line.begin()), size_t(to - line.begin()));
    } else {
      step.distance = int(std::max(index_of(xs, target->x), index_of(xs, focus.x)) -
                          std::min(index_of(xs, target->x), index_of(xs, focus.x)));
    }
    if (target->y != focus.y) step.distance += 1;  // and the rows after (approximate: re-planned per press)
    return step;
  }
  auto line = line_of(true, focus.x);
  auto from = std::find_if(line.begin(), line.end(), [&](auto& e) { return e.first == focus.y; });
  auto to = std::find_if(line.begin(), line.end(), [&](auto& e) { return e.first == target->y; });
  step.dy = Sign(target->y - focus.y);
  step.distance = CountAvailable(line, size_t(from - line.begin()), size_t(to - line.begin()));
  return step;
}

Step PlanList(const ListBox& l, int32_t px, int32_t py) {
  Step step;
  if (l.row_height <= 0 || l.visible <= 0 || l.count <= 0 || px < l.x || px >= l.x + l.w || py < l.y) return step;
  const int32_t k = (py - l.y) / l.row_height;
  const int32_t target = l.top + k;
  if (k >= l.visible || target >= l.count || py >= l.y + l.h) return step;
  step.over_row = true;
  step.available = true;
  if (target == l.index) {
    step.on_selected = true;
    return step;
  }
  step.dy = Sign(target - l.index);
  step.distance = std::abs(target - l.index);
  return step;
}

}  // namespace kuf::menu_pointer
