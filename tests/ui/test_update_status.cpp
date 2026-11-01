// The in-game updater's texts (src/runtime/update_status.cpp): the updater's JSON lines and the message box texts.

#include <catch2/catch_test_macros.hpp>

#include <string>

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

TEST_CASE("dialog texts: versions, date, notes made safe for the game's box", "[updates]") {
  Reply r;
  r.current = "0.7.0";
  r.latest = "0.8.0";
  r.published = "2026-11-02";
  r.notes = "Line one\nmail@host \\ path\n" + std::string(400, 'x');
  const std::string t = DialogText(Dialog::kOffer, r, "0.7.0-dev");
  CHECK(t.find("Installed: 0.7.0\\nAvailable: 0.8.0 (released 2026-11-02)") != std::string::npos);
  CHECK(t.find("Line one\\nmailhost  path") != std::string::npos);  // '@' and '\' removed, new lines as "\n"
  CHECK(t.find("...") != std::string::npos);                         // shortened
  CHECK(t.find("@(A)Install  @(B)Close") != std::string::npos);
  // The installed version from the build when the updater did not report one.
  CHECK(DialogText(Dialog::kNotSetUp, Reply{}, "0.7.0-dev.5").find("Installed: 0.7.0-dev.5") != std::string::npos);
  Reply f;
  f.error = "refused: signature not valid";
  CHECK(DialogText(Dialog::kFailed, f, "0.7.0").find("refused: signature not valid") != std::string::npos);
  // A note ending in a broken escape is never left as half of the box's "\n".
  Reply n;
  n.notes = std::string(299, 'y') + "\nz";
  const std::string cut = DialogText(Dialog::kOffer, n, "0.7.0");
  CHECK(cut.find("y\\...") == std::string::npos);
}
