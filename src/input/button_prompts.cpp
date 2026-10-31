#include "input/button_prompts.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace kuf::input {
namespace {

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// One key for a control: in the menus Enter when bound (their A), else the first key (in gameplay the first: the mouse
// button for the weapons).
std::string KeyFor(const Bindings& b, Context context, Control control) {
  const auto& keys = b.keys(context, control);
  if (keys.empty()) return {};
  const HostKey enter = ParseHostKey("Return");
  const bool prefer_enter = context != Context::kGameplay && std::find(keys.begin(), keys.end(), enter) != keys.end();
  return HostKeyLabel(prefer_enter ? enter : keys.front());
}

// Four directions (up, down, left, right): "Arrows" for the arrow keys, the letters joined for single letters
// ("WASD": up, left, down, right as written), else joined with '/'.
std::string Directions(const Bindings& b, Context context, std::array<Control, 4> dirs) {
  std::array<std::string, 4> k;
  for (size_t i = 0; i < 4; ++i) {
    k[i] = KeyFor(b, context, dirs[i]);
    if (k[i].empty()) return {};
  }
  if (k[0] == "Up" && k[1] == "Down" && k[2] == "Left" && k[3] == "Right") return "Arrows";
  if (std::all_of(k.begin(), k.end(), [](const std::string& s) { return s.size() == 1; })) {
    return k[0] + k[2] + k[1] + k[3];
  }
  return k[0] + "/" + k[2] + "/" + k[1] + "/" + k[3];
}

}  // namespace

std::optional<Control> IconControl(std::string_view icon) {
  const std::string i = Lower(icon);
  if (i == "a") return Control::kA;
  if (i == "b") return Control::kB;
  if (i == "x") return Control::kX;
  if (i == "y") return Control::kY;
  if (i == "l" || i == "lb") return Control::kLeftShoulder;
  if (i == "r" || i == "rb") return Control::kRightShoulder;
  if (i == "lt" || i == "l_trigger") return Control::kLeftTrigger;
  if (i == "rt" || i == "r_trigger") return Control::kRightTrigger;
  if (i == "start") return Control::kStart;
  if (i == "back") return Control::kBack;
  return std::nullopt;
}

std::string IconKeys(std::string_view icon, const Bindings& bindings, Context context, bool mouse_look) {
  const std::string i = Lower(icon);
  std::string keys;
  if (const auto control = IconControl(icon)) {
    keys = KeyFor(bindings, context, *control);
  } else if (i == "dpad_small" || i == "dpad") {
    keys = Directions(bindings, context, {Control::kDpadUp, Control::kDpadDown, Control::kDpadLeft, Control::kDpadRight});
  } else if (i == "apad_l_small") {
    keys = Directions(bindings, context,
                      {Control::kLeftStickUp, Control::kLeftStickDown, Control::kLeftStickLeft, Control::kLeftStickRight});
  } else if (i == "apad_r_small") {
    keys = mouse_look ? std::string("Mouse")
                      : Directions(bindings, context, {Control::kRightStickUp, Control::kRightStickDown,
                                                       Control::kRightStickLeft, Control::kRightStickRight});
  }
  return keys.empty() ? std::string() : "[" + keys + "]";
}

std::string RewritePrompts(std::string_view text, const std::function<std::string(std::string_view)>& replace) {
  std::string out;
  out.reserve(text.size() + 16);
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '@' && i + 1 < text.size() && text[i + 1] == '(') {
      const size_t close = text.find(')', i + 2);
      if (close != std::string_view::npos) {
        const std::string_view name = text.substr(i + 2, close - i - 2);
        const std::string with = name.find_first_of("=,") == std::string_view::npos ? replace(name) : std::string();
        if (!with.empty()) {
          out += with;
          if (close + 1 < text.size() && text[close + 1] != ' ' && text[close + 1] != '@') out += ' ';
        } else {
          out.append(text.substr(i, close + 1 - i));
        }
        i = close + 1;
        continue;
      }
    }
    out += text[i++];
  }
  return out;
}

int CountButtonIcons(std::string_view text) {
  const Bindings bindings = Bindings::Defaults();
  int n = 0;
  RewritePrompts(text, [&](std::string_view icon) {
    if (!IconKeys(icon, bindings, Context::kMenu, false).empty()) ++n;
    return std::string();
  });
  return n;
}

std::string ConciseHelpBar(std::string_view text) {
  if (CountButtonIcons(text) < 3) return std::string(text);
  // The entries: each button icon with the words after it, up to the next button icon; markup before the first
  // (a scale) is kept.
  struct Entry {
    size_t begin, end;
    bool direction;
  };
  std::vector<Entry> entries;
  size_t prefix_end = std::string_view::npos;
  for (size_t i = text.find("@("); i != std::string_view::npos; i = text.find("@(", i + 2)) {
    const size_t close = text.find(')', i + 2);
    if (close == std::string_view::npos) break;
    std::string icon(text.substr(i + 2, close - i - 2));
    for (char& c : icon) c = char(std::tolower(static_cast<unsigned char>(c)));
    const bool direction = icon == "dpad_small" || icon == "dpad" || icon == "apad_l_small" || icon == "apad_r_small";
    if (!direction && !IconControl(icon)) continue;  // other markup belongs to the entry it is in
    if (prefix_end == std::string_view::npos) prefix_end = i;
    if (!entries.empty()) entries.back().end = i;
    entries.push_back({i, text.size(), direction});
  }
  if (entries.empty()) return std::string(text);
  auto trimmed = [&](const Entry& e) {
    std::string_view v = text.substr(e.begin, e.end - e.begin);
    while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
    return v;
  };
  std::string out(text.substr(0, prefix_end));
  size_t kept = 0;
  for (const Entry& e : entries) {
    if (e.direction) continue;
    if (kept++) out += "  ";
    out += trimmed(e);
  }
  return kept ? out : std::string(text);
}

std::vector<std::string> SplitHelpBar(std::string_view text) {
  // A leading markup prefix that is not a button icon (the scale): kept for each entry.
  std::string prefix;
  while (text.size() > 2 && text[0] == '@' && text[1] == '(') {
    const size_t close = text.find(')');
    if (close == std::string_view::npos) break;
    const std::string_view name = text.substr(2, close - 2);
    if (name.find_first_of("=,") == std::string_view::npos) break;  // a button icon: the first entry starts here
    prefix.append(text.substr(0, close + 1));
    text.remove_prefix(close + 1);
  }
  std::vector<std::string> out;
  size_t start = 0;
  auto flush = [&](size_t end) {
    std::string_view e = text.substr(start, end - start);
    while (!e.empty() && e.front() == ' ') e.remove_prefix(1);
    while (!e.empty() && e.back() == ' ') e.remove_suffix(1);
    if (!e.empty()) out.push_back(prefix + std::string(e));
  };
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] == ' ' && text[i + 1] == ' ') {
      flush(i);
      while (i < text.size() && text[i] == ' ') ++i;
      start = i;
    }
  }
  flush(text.size());
  return out;
}

std::optional<Control> HelpEntryControl(std::string_view entry) {
  for (size_t i = entry.find("@("); i != std::string_view::npos; i = entry.find("@(", i + 2)) {
    const size_t close = entry.find(')', i + 2);
    if (close == std::string_view::npos) break;
    if (const auto control = IconControl(entry.substr(i + 2, close - i - 2))) return control;
  }
  return std::nullopt;
}

namespace {
// `entry` without the markup that is not a button icon in front ("@(scale=0.6,0.7)").
std::string_view WithoutLeadingStyle(std::string_view entry) {
  while (entry.size() > 2 && entry[0] == '@' && entry[1] == '(') {
    const size_t close = entry.find(')');
    if (close == std::string_view::npos || entry.substr(2, close - 2).find_first_of("=,") == std::string_view::npos) {
      break;
    }
    entry.remove_prefix(close + 1);
  }
  return entry;
}
}  // namespace

std::optional<Control> EntryControl(std::string_view entry, const Bindings& bindings) {
  if (const auto icon = HelpEntryControl(entry)) return icon;
  const std::string_view text = WithoutLeadingStyle(entry);
  constexpr const char* kButtons[] = {"A", "B", "X", "Y", "L", "R", "LT", "RT", "Start", "Back"};
  for (const char* name : kButtons) {
    const std::string keys = IconKeys(name, bindings, Context::kMenu, false);
    if (!keys.empty() && text.substr(0, keys.size()) == keys &&
        (text.size() == keys.size() || text[keys.size()] == ' ')) {
      return IconControl(name);
    }
  }
  return std::nullopt;
}

size_t VisibleLength(std::string_view entry) {
  size_t n = 0;
  for (size_t i = 0; i < entry.size(); ++i) {
    if (entry[i] == '@' && i + 1 < entry.size() && entry[i + 1] == '(') {
      const size_t close = entry.find(')', i + 2);
      if (close != std::string_view::npos) {
        // A button icon is drawn about two characters wide; other markup not at all.
        if (entry.substr(i + 2, close - i - 2).find_first_of("=,") == std::string_view::npos) n += 2;
        i = close;
        continue;
      }
    }
    ++n;
  }
  return n;
}

}  // namespace kuf::input
