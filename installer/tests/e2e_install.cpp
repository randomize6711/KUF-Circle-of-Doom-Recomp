// A full installation with the real payload and the player's game files into a disposable folder (not a unit test:
// it copies about 4 GB). Never starts the game.
//   kuf_setup_e2e <payload package dir> <assets dir> <game folder> <target root>
// HOME / XDG_* should point into a temporary folder (installer/README.md "Testing").
#include <cstdio>
#include <string>

#include "core/install.h"

using namespace kuf::setup;

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--uninstall") {  // removes a test installation (saves kept)
    const Result u = Uninstall(argv[2], RemoveOptions{}, nullptr);
    std::printf("uninstall: %s %s\n", u.ok ? "ok" : "FAILED", u.error.c_str());
    for (const auto& n : u.notes) std::printf("  note: %s\n", n.c_str());
    return u.ok ? 0 : 1;
  }
  if (argc != 5) {
    std::fprintf(stderr, "usage: kuf_setup_e2e PAYLOAD ASSETS GAME ROOT\n");
    return 2;
  }
  std::string list;
  if (!ReadFile(fs::path(argv[2]) / "game_v1.tsv", &list)) return 2;
  Plan plan;
  plan.package = argv[1];
  plan.manifest = ParseGameManifest(list, "v1");
  plan.game_source = argv[3];
  plan.root = argv[4];
  plan.icon = fs::path(argv[2]) / "icon-256.png";
  plan.setup_assets = argv[2];
  plan.menu = true;
  plan.desktop = true;
  std::atomic<bool> cancel{false};
  int last = -1;
  const Result r = Install(plan, [&](Stage s, double f, const std::string&) {
    const int pct = int(f * 10);
    if (int(s) * 100 + pct != last) {
      last = int(s) * 100 + pct;
      if (pct == 0 || pct == 10) std::printf("  %s %d%%\n", StageName(s), pct * 10);
    }
  }, cancel);
  std::printf("install: %s %s\n", r.ok ? "ok" : "FAILED", r.error.c_str());
  for (const auto& n : r.notes) std::printf("  note: %s\n", n.c_str());
  return r.ok ? 0 : 1;
}
