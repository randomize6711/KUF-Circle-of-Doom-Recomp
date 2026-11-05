// The in-game updater's texts (src/runtime/update_status.cpp): the updater's JSON lines and the message box texts.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

#include "runtime/update_status.h"

using namespace kuf::updates;

TEST_CASE("updater lines: a check result with escaped notes", "[updates]") {
  const auto r = ParseLine(
      R"({"current": "0.7.0", "latest": "0.8.0", "update": true, "published": "2026-11-02", )"
      R"("notes": "Fixes:\n- \"menus\"é", "size": 123, "installed": ["0.7.0"], "message": "x", "exit": 0})");
  REQUIRE(r);
  CHECK(r->exit == 0);
  CHECK(r->update);
  CHECK(r->current == "0.7.0");
  CHECK(r->latest == "0.8.0");
  CHECK(r->published == "2026-11-02");
  CHECK(r->notes == "Fixes:\n- \"menus\"\xC3\xA9");
  const auto m = ParseLine(R"({"latest": "0.7.1", "summary": "Short.", "notes": "Long.", "page": "https://x", "exit": 0})");
  REQUIRE(m);
  CHECK(m->summary == "Short.");
  CHECK(m->page == "https://x");
}

TEST_CASE("updater lines: progress, stage, up to date with null, errors; garbage refused", "[updates]") {
  const auto p = ParseLine(R"({"progress": 52428800, "total": 104857600})");
  REQUIRE(p);
  CHECK(p->exit == -1);
  CHECK(ProgressLabel(*p) == "DOWNLOADING 50%");
  const auto v = ParseLine(R"({"stage": "verifying"})");
  REQUIRE(v);
  CHECK(ProgressLabel(*v) == "VERIFYING THE UPDATE...");
  const auto i = ParseLine(R"({"stage": "installing"})");
  REQUIRE(i);
  CHECK(ProgressLabel(*i) == "INSTALLING THE UPDATE...");
  const auto u = ParseLine(R"({"current": "0.7.0", "latest": null, "message": "no signed release found", "exit": 3})");
  REQUIRE(u);
  CHECK(u->exit == 3);
  CHECK(u->latest.empty());
  const auto e = ParseLine(R"({"error": "api.github.com: timed out", "exit": 1})");
  REQUIRE(e);
  CHECK(e->error == "api.github.com: timed out");
  CHECK_FALSE(ParseLine("Traceback (most recent call last):"));
  CHECK_FALSE(ParseLine(R"({"exit": 0)"));
  CHECK_FALSE(ParseLine(R"({"notes": "unterminated)"));
}

TEST_CASE("offer: only the versions and the question; never notes, dates, URLs or setup text", "[updates]") {
  Reply r;
  r.current = "0.7.0-rc.1";
  r.latest = "0.7.0";
  r.published = "2026-11-02";
  r.notes = "Install: download the Setup AppImage and run it.";
  r.summary = "Faster rendering.";
  r.page = "https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp/releases/tag/v0.7.0";
  const std::string t = DialogText(Dialog::kOffer, r, "0.7.0-dev");
  CHECK(t.rfind("UPDATE AVAILABLE\\n\\nInstalled Version: 0.7.0-rc.1\\nNew Version: 0.7.0\\n\\n"
                "Download and install the update now?", 0) == 0);
  for (const char* never : {"AppImage", "Install:", "Faster", "2026", "github", "http", "sha", "..."}) {
    CHECK(t.find(never) == std::string::npos);
  }
  // Room for the two button rows below the question.
  size_t lines = 0;
  for (size_t at = t.find("now?"); (at = t.find("\\n", at)) != std::string::npos; at += 2) ++lines;
  CHECK(lines == size_t(kOfferButtonLines));
  CHECK(t.find("@(") == std::string::npos);  // the answers are buttons and the box's own line
  // Versions from the updater; the build's own version when it did not say. Markup characters dropped.
  Reply v;
  v.latest = "1.0.0-rc.1@x\\y";
  const std::string u = DialogText(Dialog::kOffer, v, "0.7.0-dev.12+gabc");
  CHECK(u.find("Installed Version: 0.7.0-dev.12+gabc\\nNew Version: 1.0.0-rc.1xy") != std::string::npos);
  CHECK(u.size() < 1023);  // the box's text buffer
}

TEST_CASE("other update boxes: concise, no notes", "[updates]") {
  Reply r;
  r.current = "0.7.0";
  r.latest = "0.7.1";
  r.notes = "Install: download the Setup AppImage.";
  r.error = "could not reach api.github.com (no network connection?)";
  CHECK(DialogText(Dialog::kUpToDate, r, "x").rfind("NO UPDATE AVAILABLE\\n\\nInstalled Version: 0.7.0", 0) == 0);
  CHECK(DialogText(Dialog::kFailed, r, "x").find("could not reach api.github.com") != std::string::npos);
  CHECK(DialogText(Dialog::kFailed, r, "x").rfind("UPDATE FAILED", 0) == 0);
  CHECK(DialogText(Dialog::kRestart, r, "x").rfind("RESTART REQUIRED\\n\\nVersion 0.7.1 is installed", 0) == 0);
  CHECK(DialogText(Dialog::kNotSetUp, Reply{}, "0.7.0-dev.5").find("Installed Version: 0.7.0-dev.5") !=
        std::string::npos);
  for (Dialog d : {Dialog::kUpToDate, Dialog::kFailed, Dialog::kNotSetUp, Dialog::kCancelAsk, Dialog::kCancelled,
                   Dialog::kRestart, Dialog::kInstalledNoRestart}) {
    CHECK(DialogText(d, r, "x").find("AppImage") == std::string::npos);
  }
}

TEST_CASE("Open GitHub Page: only the official repository's release page, else its releases", "[updates]") {
  const std::string releases = kReleasesPage;
  CHECK(ReleasePage(releases + "/tag/v0.7.0") == releases + "/tag/v0.7.0");
  CHECK(ReleasePage(releases + "/tag/v1.2.3-rc.4") == releases + "/tag/v1.2.3-rc.4");
  for (const char* bad : {"", "https://github.com/other/repo/releases/tag/v0.7.0", "http://github.com/randomize6711/"
                          "KUF-Circle-of-Doom-Recomp/releases/tag/v0.7.0",
                          "https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp/releases/tag/v0.7.0/../../x",
                          "https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp/releases/tag/v0.7.0?x=1",
                          "https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp/releases/tag/v0.7.0 --x",
                          "https://github.com/randomize6711/KUF-Circle-of-Doom-Recomp/releases/tag/vx",
                          "https://evil.example/randomize6711/KUF-Circle-of-Doom-Recomp/releases/tag/v0.7.0",
                          "https://github.com.evil.example/randomize6711/KUF-Circle-of-Doom-Recomp/releases"}) {
    CHECK(ReleasePage(bad) == releases);
  }
}

TEST_CASE("answers: the box's own line names the actions", "[updates]") {
  CHECK(AnswerLabels(Dialog::kOffer) == std::pair<std::string, std::string>{"Select", "Cancel"});
  CHECK(AnswerLabels(Dialog::kRestart).first == "Restart Now");
  CHECK(AnswerLabels(Dialog::kFailed) == std::pair<std::string, std::string>{"OK", "Close"});
  // The game's help line (as traced: @(align=1)@(A) Yes @(B) No).
  CHECK(RelabelAnswers("@(align=1)@(A) Yes @(B) No", "Install Update", "Cancel") ==
        "@(align=1)@(A) Install Update @(B) Cancel");
  CHECK(RelabelAnswers("@(A)Yes  @(B)No", "OK", "Close") == "@(A)OK  @(B)Close");
  // Other texts unchanged: no Yes / No answer pair, or words that only start with them.
  CHECK(RelabelAnswers("@(align=1)@(A) Confirm", "Install Update", "Cancel") == "@(align=1)@(A) Confirm");
  CHECK(RelabelAnswers("@(A) Yesterday @(B) Nothing", "x", "y") == "@(A) Yesterday @(B) Nothing");
  CHECK(RelabelAnswers("@(B) Back", "x", "y") == "@(B) Back");
}
