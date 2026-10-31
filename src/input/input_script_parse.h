// Parser for --kuf_input_script (input_script.h documents the syntax). Pure: no runtime state, unit-tested.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kuf::input::script {

struct Event {
  std::string verb;              // down, up, wheel, mouse, focus, pad, log (press/tap are expanded)
  std::vector<std::string> args;
  std::string text;              // the original item, for the log
  // Anchored events ("@gameplay#2+30 ..."): the map key is the offset in ticks after the n-th time the game
  // entered that context (menu, prompt, gameplay); anchor_count 0 = an absolute frame.
  std::string anchor_context;
  int anchor_count = 0;
  // "repeat <interval> until <context>[#n] <verb...>": applied every <interval> ticks until the game has
  // entered <context> n times (e.g. skipping a cutscene until the player has control). 0 = once.
  uint64_t repeat_interval = 0;
  std::string until_context;
  int until_count = 0;
  // The release half of a `press` (see Releases()).
  bool release = false;
};

// True for an item that lets go of an input: a press's release, a key or pad button up, a stick or trigger back to
// 0, a real SDL key or mouse button up. When a repeat's target context is reached, its pending releases still apply
// once (a release cannot leave anything held; dropped, it could: K42), the rest stop.
bool Releases(const Event& e);

// The input items due when game tick `tick` starts (script time before `tick`), in order, taken from `events`; a
// repeated item is queued again for its next time until its target context has been entered (`entries`: how often
// each context was entered so far).
void TakeDue(std::multimap<uint64_t, Event>& events, uint64_t tick, const std::map<std::string, int>& entries,
             std::vector<std::pair<uint64_t, Event>>& due);

// Scripted events by game tick (script time, input_script.h; insertion order within a tick); anchored events
// are keyed by their offset. Returns the number of script items, or -1 with *error set.
int Parse(const std::string& text, std::multimap<uint64_t, Event>& out, std::string* error);

// The text of a script file (--kuf_input_script_file) as a Parse() string: one or more items per line
// (separated by ';'), blank lines, and comment lines whose first non-blank character is '#' ('#' elsewhere
// is part of an item, as in anchors like @menu#2+60).
std::string ScriptFromFileText(const std::string& file_text);

// SDL_GamepadButton / SDL_GamepadAxis for a script name, or -1.
int PadButton(const std::string& name);
int PadAxis(const std::string& name);

}  // namespace kuf::input::script
