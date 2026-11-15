// kuf-localbuild: the local build without a window, for the updater and for tests (docs/LOCAL_BUILD.md).
//   kuf-localbuild --package DIR --xex FILE --cache DIR --output FILE [--jobs N]
// Prints one line per step, "[generate|compile|link] percent detail"; at the end one "stats" line. Exit: 0 done, 1
// failed (the reason on stderr), 2 usage, 130 cancelled by SIGINT or SIGTERM.
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include "core/localbuild.h"
#include "core/platform.h"


namespace {
std::atomic<bool> g_cancel{false};
void OnSignal(int) { g_cancel = true; }
}  // namespace

int main(int argc, char** argv) {
  kuf::setup::BuildRequest request;
  const std::vector<std::string> args = kuf::setup::Utf8Arguments(argc, argv);
  for (size_t i = 1; i + 1 < args.size(); i += 2) {
    const std::string& a = args[i];
    const auto path = [&] { return kuf::setup::PathFromUtf8(args[i + 1]); };
    if (a == "--package") request.package = path();
    else if (a == "--xex") request.xex = path();
    else if (a == "--cache") request.cache = path();
    else if (a == "--output") request.output = path();
    else if (a == "--jobs") request.jobs = std::atoi(args[i + 1].c_str());
    else return 2;
  }
  if (request.package.empty() || request.xex.empty() || request.cache.empty() || request.output.empty()) {
    std::fprintf(stderr, "usage: kuf-localbuild --package DIR --xex FILE --cache DIR --output FILE [--jobs N]\n");
    return 2;
  }
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  std::string error;
  kuf::setup::BuildStats stats;
  int last = -1;
  const bool ok = kuf::setup::LocalBuild(
      request,
      [&](kuf::setup::BuildPhase phase, double f, const std::string& detail) {
        const char* name = phase == kuf::setup::BuildPhase::kGenerate ? "generate"
                           : phase == kuf::setup::BuildPhase::kCompile ? "compile" : "link";
        const int percent = int(f * 100);
        if (percent == last && phase == kuf::setup::BuildPhase::kCompile) return;
        last = percent;
        std::printf("[%s] %d %s\n", name, percent, detail.c_str());
        std::fflush(stdout);
      },
      g_cancel, &error, &stats);
  if (!ok) {
    std::fprintf(stderr, "kuf-localbuild: %s\n", error.c_str());
    return g_cancel ? 130 : 1;
  }
  std::printf("stats id=%s jobs=%d compiled=%d reused=%d generate=%.1f compile=%.1f link=%.1f\n",
              stats.generation_id.c_str(), stats.jobs, stats.compiled, stats.reused, stats.generate_seconds,
              stats.compile_seconds, stats.link_seconds);
  return 0;
}
