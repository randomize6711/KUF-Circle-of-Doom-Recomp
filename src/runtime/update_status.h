// The in-game updater's texts (M7 updater; src/runtime/title_menu.cpp runs tools/updater/kuf_update.py and shows
// these in the game's own message box and the CHECK FOR UPDATES row). Pure: unit tests in tests/ui.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  std::string summary;    // its short in-game update summary (the signed manifest's; else its notes): not shown
  std::string page;       // its GitHub release page, as the updater reported it (validated by ReleasePage)
  std::string error;
  std::string stage;
  int64_t progress = -1, total = 0;
};
// nullopt if the line is not a JSON object.
std::optional<Reply> ParseLine(std::string_view line);

// What the dialog is for.
enum class Dialog { kOffer, kUpToDate, kFailed, kNotSetUp, kCancelAsk, kCancelled, kRestart, kInstalledNoRestart };
// The message box text (the game's markup: "\n" as the two characters backslash and n). `installed`: this build's
// version (shown when the updater did not say). Concise: a title, the versions, one line; never release notes,
// dates, URLs or setup instructions. The offer ends with kOfferButtonLines empty lines: the place of its two button
// rows (Open GitHub Page; Install Update and Cancel), above the box's own answer line.
constexpr int kOfferButtonLines = 3;
std::string DialogText(Dialog dialog, const Reply& reply, std::string_view installed);
// The labels of the box's two answers (its own help line, A and B) where the dialog has no buttons of its own:
// the restart "Restart Now" / "Later", the cancel question "Cancel Download" / "Keep Downloading", the others "OK" /
// "Close"; the offer (its own buttons) "Select" / "Cancel".
std::pair<std::string, std::string> AnswerLabels(Dialog dialog);
// The official release page to open for Open GitHub Page: the page the updater reported when it is exactly
// https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp/releases/tag/v<version> (the version's characters only),
// else the repository's releases page.
constexpr const char* kReleasesPage = "https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp/releases";
std::string ReleasePage(std::string_view reported);

// The game's message box help line with its two answers renamed: "@(A) Yes @(B) No" -> "@(A) <a> @(B) <b>"
// (whatever spaces and markup around them); other texts unchanged.
std::string RelabelAnswers(std::string_view text, std::string_view a, std::string_view b);

// The CHECK FOR UPDATES row while downloading: "DOWNLOADING 45%".
std::string ProgressLabel(const Reply& progress);

}  // namespace kuf::updates
