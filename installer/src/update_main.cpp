// kuf-update: the updater of the portable edition and the Windows installation (M17G; core/update/updater.h).
// The same commands, JSON lines and exit codes as tools/updater/kuf_update.py, which the game's Check for Updates
// reads (src/runtime/update_status.cpp):
//
//   kuf-update [--program-root DIR] [--json] [--source-dir DIR] [--prerelease] [--allow-downgrade] [--yes]
//              check | update | apply | rollback | status
//
// The program folder defaults to the one this program is in (Runtime/bin/../..). `update` stages the new version
// for the next start; `apply` puts a staged one in place (the launcher does this itself before starting the game).
// On Windows `apply` and `rollback` cannot move Runtime/ while this program runs from it (they fail and change
// nothing): there the launcher does both (Play KUF.exe at its start, and Play KUF.exe --rollback-update).
// Exit: 0 done / update available, 1 error, 2 usage, 3 up to date, 4 refused (verification), 5 cancelled.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include "core/platform.h"
#include "core/update/json.h"
#include "core/update/updater.h"

namespace up = kuf::setup::update;
namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_cancel{false};
void OnSignal(int) { g_cancel = true; }

bool g_json = false;

void Line(const std::string& json) {
  std::printf("%s\n", json.c_str());
  std::fflush(stdout);
}

int Report(int exit, const std::vector<std::pair<std::string, std::string>>& fields, const std::string& message) {
  if (g_json) {
    std::string out = "{\"exit\": " + std::to_string(exit);
    for (const auto& [k, v] : fields) out += ", " + up::JsonQuote(k) + ": " + v;
    out += ", \"message\": " + up::JsonQuote(message) + "}";
    Line(out);
  } else {
    std::printf("%s\n", message.c_str());
  }
  return exit;
}

std::string Q(const std::string& s) { return up::JsonQuote(s); }
std::string Or(const std::string& s) { return s.empty() ? "null" : Q(s); }

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args = kuf::setup::Utf8Arguments(argc, argv);
  fs::path root, source_dir;
  std::string cmd;
  up::UpdateOptions options;
  bool yes = false;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--program-root" && i + 1 < args.size()) root = kuf::setup::PathFromUtf8(args[++i]);
    else if (a == "--source-dir" && i + 1 < args.size()) source_dir = kuf::setup::PathFromUtf8(args[++i]);
    else if (a == "--json") g_json = true;
    else if (a == "--prerelease") options.prerelease = true;
    else if (a == "--allow-downgrade") options.allow_downgrade = true;
    else if (a == "--yes") yes = true;
    else if (cmd.empty() && a[0] != '-') cmd = a;
    else return 2;
  }
  if (cmd.empty()) {
    std::fprintf(stderr, "usage: kuf-update [--program-root DIR] [--json] [--source-dir DIR] [--prerelease] "
                         "[--allow-downgrade] [--yes] check|update|apply|rollback|status\n");
    return 2;
  }
  if (root.empty()) root = kuf::setup::SelfExecutable().parent_path().parent_path().parent_path();
  const up::ProgramFolder folder{fs::absolute(root)};
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  if (cmd == "status") {
    return Report(0, {{"current", Or(folder.CurrentVersion())}, {"previous", Or(folder.PreviousVersion())},
                      {"pending", Or(folder.PendingVersion())}},
                  "current " + folder.CurrentVersion() + ", previous " +
                      (folder.PreviousVersion().empty() ? "none" : folder.PreviousVersion()) + ", staged " +
                      (folder.PendingVersion().empty() ? "none" : folder.PendingVersion()));
  }
  if (cmd == "apply") {
    std::string applied, why;
    if (!up::ApplyStagedUpdate(folder, &applied, &why)) return Report(1, {{"error", Q(why)}}, "error: " + why);
    return Report(0, {{"current", Or(folder.CurrentVersion())}},
                  applied.empty() ? "nothing staged" : "now " + applied);
  }
  if (cmd == "rollback") {
    std::string now, why;
    if (!up::Rollback(folder, &now, &why)) return Report(1, {{"error", Q(why)}}, "error: " + why);
    return Report(0, {{"current", Q(now)}}, "switched back to " + now);
  }
  if (cmd != "check" && cmd != "update") return 2;

  std::unique_ptr<up::ReleaseSource> source;
  std::string why;
  if (!source_dir.empty()) {
    source = up::DirectorySource(source_dir);
  } else {
    up::UpdateConfig conf;
    if (!up::ReadUpdateConfig(folder, &conf, &why)) return Report(1, {{"error", Q(why)}}, "error: " + why);
    source = up::HttpSource(conf.repo, conf.api, &why);
    if (!source) return Report(1, {{"error", Q(why)}}, "error: " + why);
  }
  const bool update = cmd == "update";
  if (update && !yes && !g_json) {
    std::printf("Install the newest version? [y/N] ");
    std::fflush(stdout);
    int c = std::getchar();
    if (c != 'y' && c != 'Y') return Report(1, {}, "not installed");
  }
  int last = -1;
  const up::CheckResult r =
      update ? up::Update(folder, *source, options,
                          [&](uint64_t done, uint64_t total) {
                            const int percent = total ? int(done * 100 / total) : 0;
                            if (g_json && percent != last) {
                              last = percent;
                              Line("{\"progress\": " + std::to_string(done) + ", \"total\": " + std::to_string(total) +
                                   "}");
                            }
                          },
                          [&](const std::string& stage) {
                            if (g_json) Line("{\"stage\": " + Q(stage) + "}");
                          },
                          g_cancel)
             : up::Check(folder, *source, options);
  const int exit = int(r.exit);
  if (!r.error.empty()) {
    return Report(exit, {{"current", Or(r.current)}, {"latest", Or(r.latest)}, {"error", Q(r.error)}},
                  (r.exit == up::Exit::kRefused ? "refused: " : r.exit == up::Exit::kUpToDate ? "" : "error: ") +
                      r.error);
  }
  if (update && r.exit == up::Exit::kDone) {
    return Report(0, {{"current", Q(r.latest)}, {"previous", Or(r.current)}, {"restart", "true"}},
                  "updated " + r.current + " -> " + r.latest + " (in place at the next start)");
  }
  return Report(exit,
                {{"current", Or(r.current)}, {"latest", Or(r.latest)}, {"update", r.update ? "true" : "false"},
                 {"published", Q(r.published)}, {"notes", Q(r.notes)}, {"summary", Q(r.summary)},
                 {"page", Q(r.page)}, {"size", std::to_string(r.size)}},
                r.update ? "update available: " + r.latest + " (installed " + r.current + ")"
                         : "up to date (" + r.current + ")");
}
