#include "input/input_script_parse.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>

#include <sstream>

#include "input/keyboard_pad.h"

namespace kuf::input::script {
namespace {

constexpr uint64_t kPressFrames = 4;  // `press`: held for this many frames

bool Fail(std::string* error, const std::string& msg) {
  if (error) {
    *error = msg;
  }
  return false;
}

bool IsInt(const std::string& s) {
  size_t i = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
  if (i >= s.size()) {
    return false;
  }
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return s.size() < 12;
}

Event Like(const Event& e, std::string verb, std::vector<std::string> args, std::string text) {
  Event x{std::move(verb), std::move(args), std::move(text)};
  x.anchor_context = e.anchor_context;
  x.anchor_count = e.anchor_count;
  x.repeat_interval = e.repeat_interval;
  x.until_context = e.until_context;
  x.until_count = e.until_count;
  return x;
}

// Validates one item and expands press/tap into primitive events.
bool Add(std::multimap<uint64_t, Event>& out, uint64_t frame, Event e, std::string* error) {
  const auto& a = e.args;
  auto bad = [&](const char* why) { return Fail(error, "input script: '" + e.text + "': " + why); };
  if (e.verb == "down" || e.verb == "up" || e.verb == "tap" || e.verb == "press") {
    if (a.size() != 1 || !ParseHostKey(a[0])) {
      return bad("expected one known key name");
    }
    if (e.verb == "tap") {  // down and up at the same instant: only the latch makes the guest see it
      out.emplace(frame, Like(e, "down", a, e.text));
      out.emplace(frame, Like(e, "up", a, e.text));
    } else if (e.verb == "press") {
      out.emplace(frame, Like(e, "down", a, e.text));
      Event up = Like(e, "up", a, e.text + " (release)");
      up.release = true;
      out.emplace(frame + kPressFrames, std::move(up));
    } else {
      out.emplace(frame, std::move(e));
    }
    return true;
  }
  if (e.verb == "wheel") {
    if (a.size() != 1 || !IsInt(a[0])) {
      return bad("expected wheel <detents>");
    }
  } else if (e.verb == "mouse") {
    if (a.size() != 4 || !IsInt(a[0]) || !IsInt(a[1]) || !IsInt(a[2]) || !IsInt(a[3])) {
      return bad("expected mouse <x> <y> <width> <height>");
    }
  } else if (e.verb == "focus") {
    if (a.size() != 1 || (a[0] != "on" && a[0] != "off")) {
      return bad("expected focus on|off");
    }
  } else if (e.verb == "pad") {
    if (a.empty()) {
      return bad("expected a pad command");
    }
    if (a[0] == "connect" || a[0] == "disconnect") {
      if (a.size() != 1) {
        return bad("pad connect/disconnect take no arguments");
      }
    } else if (a[0] == "down" || a[0] == "up" || a[0] == "press") {
      if (a.size() != 2 || PadButton(a[1]) < 0) {
        return bad("expected pad down|up|press <button>");
      }
      if (a[0] == "press") {
        out.emplace(frame, Like(e, "pad", {"down", a[1]}, e.text));
        Event up = Like(e, "pad", {"up", a[1]}, e.text + " (release)");
        up.release = true;
        out.emplace(frame + kPressFrames, std::move(up));
        return true;
      }
    } else if (a[0] == "axis") {
      if (a.size() != 3 || PadAxis(a[1]) < 0 || !IsInt(a[2])) {
        return bad("expected pad axis <axis> <value>");
      }
      const long v = std::stol(a[2]);
      if (v < -32768 || v > 32767) {
        return bad("axis value out of range");
      }
    } else {
      return bad("unknown pad command");
    }
  } else if (e.verb == "sdl") {
    // Real SDL events into the window's queue (input_script.h).
    const std::string what = a.empty() ? "" : a[0];
    if (what == "key") {
      if (a.size() != 3 || (a[1] != "down" && a[1] != "up" && a[1] != "repeat") ||
          SDL_GetScancodeFromName(a[2].c_str()) == SDL_SCANCODE_UNKNOWN) {
        return bad("expected sdl key down|up|repeat <SDL key name>");
      }
    } else if (what == "button") {
      if (a.size() != 5 || (a[1] != "down" && a[1] != "up") ||
          (a[2] != "left" && a[2] != "right" && a[2] != "middle") || !IsInt(a[3]) || !IsInt(a[4])) {
        return bad("expected sdl button down|up left|right|middle <x> <y>");
      }
    } else if (what == "click") {
      if (a.size() != 4 || (a[1] != "left" && a[1] != "right" && a[1] != "middle") || !IsInt(a[2]) ||
          !IsInt(a[3])) {
        return bad("expected sdl click left|right|middle <x> <y>");
      }
      out.emplace(frame, Like(e, "sdl", {"button", "down", a[1], a[2], a[3]}, e.text));
      out.emplace(frame, Like(e, "sdl", {"button", "up", a[1], a[2], a[3]}, e.text));
      return true;
    } else if (what == "wheel") {
      if (a.size() != 4 || !IsInt(a[1]) || !IsInt(a[2]) || !IsInt(a[3])) {
        return bad("expected sdl wheel <detents> <x> <y>");
      }
    } else if (what == "motion") {
      if (a.size() != 3 || !IsInt(a[1]) || !IsInt(a[2])) {
        return bad("expected sdl motion <x> <y>");
      }
    } else if (what == "focus") {
      if (a.size() != 2 || (a[1] != "on" && a[1] != "off")) {
        return bad("expected sdl focus on|off");
      }
    } else if (what == "resize") {
      if (a.size() != 3 || !IsInt(a[1]) || !IsInt(a[2]) || std::stol(a[1]) < 64 || std::stol(a[2]) < 64) {
        return bad("expected sdl resize <width> <height> (>= 64)");
      }
    } else {
      return bad("unknown sdl event");
    }
  } else if (e.verb == "capture") {
    if (a.size() != 1 || a[0].empty() ||
        a[0].find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
      return bad("expected capture <label> (a-z, 0-9, _)");
    }
  } else if (e.verb == "freeze") {
    if (a.size() != 2 || !IsInt(a[0]) || std::stol(a[0]) < 1 || std::stol(a[0]) > 30 || a[1].empty() ||
        a[1].find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
      return bad("expected freeze <seconds 1-30> <label> (a-z, 0-9, _)");
    }
  } else if (e.verb == "trace") {
    if (!a.empty()) {
      return bad("trace takes no arguments");
    }
  } else if (e.verb == "look") {
    if (a.size() != 2 || !IsInt(a[0]) || !IsInt(a[1])) {
      return bad("expected look <dx> <dy>");
    }
  } else if (e.verb == "pointer") {
    if (!a.empty()) {
      return bad("pointer takes no arguments");
    }
  } else if (e.verb == "framerate") {
    if (a.size() != 1 || !IsInt(a[0]) || (std::stol(a[0]) != 0 && (std::stol(a[0]) < 30 || std::stol(a[0]) > 480))) {
      return bad("expected framerate <30-480, or 0 for unlimited>");
    }
  } else if (e.verb != "log") {
    return bad("unknown verb");
  }
  out.emplace(frame, std::move(e));
  return true;
}

}  // namespace

bool Releases(const Event& e) {
  const auto& a = e.args;
  if (e.release || e.verb == "up") return true;
  if (e.verb == "pad") return (a[0] == "up") || (a[0] == "axis" && a.size() == 3 && std::stol(a[2]) == 0);
  if (e.verb == "sdl") return (a[0] == "key" || a[0] == "button") && a.size() > 1 && a[1] == "up";
  return false;
}

void TakeDue(std::multimap<uint64_t, Event>& events, uint64_t tick, const std::map<std::string, int>& entries,
             std::vector<std::pair<uint64_t, Event>>& due) {
  while (!events.empty() && events.begin()->first < tick) {
    auto node = events.extract(events.begin());
    Event& e = node.mapped();
    const uint64_t when = node.key();
    if (e.repeat_interval) {
      const auto entered = entries.find(e.until_context);
      if ((entered == entries.end() ? 0 : entered->second) < e.until_count) {
        events.emplace(when + e.repeat_interval, e);
      } else if (!Releases(e)) {
        continue;  // the target context was reached: stop repeating (a pending release still applies)
      }
    }
    due.emplace_back(when, std::move(e));
  }
}

int PadButton(const std::string& name) {
  static const std::map<std::string, int> kButtons = {
      {"a", SDL_GAMEPAD_BUTTON_SOUTH},   {"south", SDL_GAMEPAD_BUTTON_SOUTH},
      {"b", SDL_GAMEPAD_BUTTON_EAST},    {"east", SDL_GAMEPAD_BUTTON_EAST},
      {"x", SDL_GAMEPAD_BUTTON_WEST},    {"west", SDL_GAMEPAD_BUTTON_WEST},
      {"y", SDL_GAMEPAD_BUTTON_NORTH},   {"north", SDL_GAMEPAD_BUTTON_NORTH},
      {"back", SDL_GAMEPAD_BUTTON_BACK}, {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
      {"start", SDL_GAMEPAD_BUTTON_START}, {"lstick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
      {"rstick", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
      {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},
      {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
      {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
  };
  auto it = kButtons.find(name);
  return it == kButtons.end() ? -1 : it->second;
}

int PadAxis(const std::string& name) {
  static const std::map<std::string, int> kAxes = {
      {"leftx", SDL_GAMEPAD_AXIS_LEFTX},   {"lefty", SDL_GAMEPAD_AXIS_LEFTY},
      {"rightx", SDL_GAMEPAD_AXIS_RIGHTX}, {"righty", SDL_GAMEPAD_AXIS_RIGHTY},
      {"lt", SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, {"rt", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
  };
  auto it = kAxes.find(name);
  return it == kAxes.end() ? -1 : it->second;
}

std::string ScriptFromFileText(const std::string& file_text) {
  std::string out;
  std::stringstream lines(file_text);
  for (std::string line; std::getline(lines, line);) {
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    const size_t last = line.find_last_not_of(" \t\r");
    if (!out.empty()) {
      out += "; ";
    }
    out += line.substr(first, last + 1 - first);
  }
  return out;
}

int Parse(const std::string& text, std::multimap<uint64_t, Event>& out, std::string* error) {
  out.clear();
  std::stringstream items(text);
  int count = 0;
  for (std::string item; std::getline(items, item, ';');) {
    std::stringstream words(item);
    std::string frame_word, verb;  // frame_word: a frame number or an anchor
    if (!(words >> frame_word)) {
      continue;  // empty item
    }
    Event e;
    uint64_t when = 0;
    if (!frame_word.empty() && frame_word[0] == '@') {
      // @<context>[#n]+<offset>: <offset> frames after the n-th entry into <context> (n defaults to 1).
      const size_t plus = frame_word.find('+');
      std::string head = frame_word.substr(1, plus == std::string::npos ? std::string::npos : plus - 1);
      const std::string off = plus == std::string::npos ? "" : frame_word.substr(plus + 1);
      const size_t hash = head.find('#');
      const std::string count = hash == std::string::npos ? "1" : head.substr(hash + 1);
      e.anchor_context = head.substr(0, hash);
      if ((e.anchor_context != "menu" && e.anchor_context != "prompt" && e.anchor_context != "gameplay") ||
          !IsInt(count) || std::stoi(count) < 1 || !IsInt(off) || std::stoll(off) < 0 || !(words >> verb)) {
        Fail(error, "input script: '" + item + "': expected '@menu|prompt|gameplay[#n]+<frames> <verb> [args]'");
        out.clear();
        return -1;
      }
      e.anchor_count = std::stoi(count);
      when = std::stoull(off);
    } else {
      if (!IsInt(frame_word) || std::stoll(frame_word) < 1 || !(words >> verb)) {
        Fail(error, "input script: '" + item + "': expected '<frame> <verb> [args]' with frame >= 1");
        out.clear();
        return -1;
      }
      when = std::stoull(frame_word);
    }
    if (verb == "repeat") {
      std::string interval, until, target;
      if (!(words >> interval >> until >> target >> verb) || !IsInt(interval) || std::stoll(interval) < 1 ||
          until != "until") {
        Fail(error, "input script: '" + item + "': expected 'repeat <frames> until <context>[#n] <verb> [args]'");
        out.clear();
        return -1;
      }
      const size_t hash = target.find('#');
      const std::string count = hash == std::string::npos ? "1" : target.substr(hash + 1);
      e.until_context = target.substr(0, hash);
      if ((e.until_context != "menu" && e.until_context != "prompt" && e.until_context != "gameplay") ||
          !IsInt(count) || std::stoi(count) < 1 || verb == "repeat" || verb == "log") {
        Fail(error, "input script: '" + item + "': bad repeat target or verb (tap, press, down/up, pad, sdl, ...)");
        out.clear();
        return -1;
      }
      e.until_count = std::stoi(count);
      e.repeat_interval = std::stoull(interval);
    }
    e.verb = verb;
    if (verb == "log") {
      std::string rest;
      std::getline(words, rest);
      e.args = {rest};
    } else {
      for (std::string w; words >> w;) {
        e.args.push_back(w);
      }
    }
    e.text = item.substr(item.find_first_not_of(' '));
    if (!Add(out, when, std::move(e), error)) {
      out.clear();
      return -1;
    }
    ++count;
  }
  return count;
}

}  // namespace kuf::input::script
