// The in-game updater's texts (M7 updater; src/runtime/title_menu.cpp runs tools/updater/kuf_update.py and shows
// these in the game's own message box and the CHECK FOR UPDATES row). Pure: unit tests in tests/ui.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kuf::updates {

// One line the updater prints with --json: a result ({"exit": N, ...}) or a progress line ({"progress": bytes,
// "total": bytes} / {"stage": "verifying"}). Fields missing from the line keep their defaults.
struct Reply {
  int exit = -1;          // -1: a progress line
  bool update = false;    // check: a newer release exists
  std::string current;    // the installed version
  std::string latest;     // the newest signed release
  std::string published;  // its date (YYYY-MM-DD)
  std::string notes;      // its notes (plain text)
  std::string error;
  std::string stage;
  int64_t progress = -1, total = 0;
};
// nullopt if the line is not a JSON object.
std::optional<Reply> ParseLine(std::string_view line);

// What the dialog is for.
enum class Dialog { kOffer, kUpToDate, kFailed, kNotSetUp, kCancelAsk, kCancelled, kRestart, kInstalledNoRestart };
// The message box text (the game's markup: "\n" as the two characters backslash and n; button icons as "@(A)").
// `installed`: this build's version (shown when the updater did not say). Notes are shortened to `max_notes`
// characters and their markup characters ('@', '\') removed.
std::string DialogText(Dialog dialog, const Reply& reply, std::string_view installed, size_t max_notes = 300);

// The CHECK FOR UPDATES row while downloading: "DOWNLOADING 45%".
std::string ProgressLabel(const Reply& progress);

}  // namespace kuf::updates
