// Installing, repairing and removing the game (installer/README.md). The installed folder has the updater's layout
// (tools/package/install.sh, tools/updater/kuf_update.py): versions/<version> (the package), current -> it, the
// stable launcher kuf-cod (it sets KUF_GAME_DIR to the installed game data), kuf-update, update.conf,
// allowed_signers; plus game/ (the verified copy of the player's game files), share/ (the icon and the setup itself,
// for uninstalling) and .kuf-install (the setup's record: what it created). Saves and settings stay in
// $XDG_DATA_HOME/kuf-cod-recomp/data, outside the installation.
#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/game_data.h"
#include "core/steam.h"
#include "core/util.h"

namespace kuf::setup {

constexpr const char* kGameName = "Kingdom Under Fire: Circle of Doom";
constexpr const char* kRecordFile = ".kuf-install";
constexpr const char* kDesktopId = "kuf-cod-recomp";  // kuf-cod-recomp.desktop, kuf-cod-recomp-uninstall.desktop
constexpr const char* kDefaultRepo = "randomize6711/KUF-Circle-of-Doom-Recomp";

// What the setup recorded in an installation folder.
struct Record {
  std::string version;
  std::string installed;               // UTC, ISO 8601
  std::vector<fs::path> shortcuts;     // desktop entries it created
  std::vector<std::pair<fs::path, std::string>> steam;  // (Steam folder, account ID) where it added the shortcut
};
std::optional<Record> ReadRecord(const fs::path& root);
std::string FormatRecord(const Record& r);
std::optional<Record> ParseRecord(std::string_view text);

// The default installation folder: ~/Games/Kingdom Under Fire - Circle of Doom.
fs::path DefaultRoot();

// A checked target folder: new (absent or empty), an installation of this setup (repair / reinstall), or refused
// (something else is there: a subfolder is suggested).
enum class Target { kNew, kExisting, kNotEmpty, kNotWritable, kInvalid };
Target CheckTarget(const fs::path& root);

enum Stage { kValidate, kPrepare, kRuntime, kGameData, kShortcuts, kFinalize, kStageCount };
const char* StageName(Stage s);

struct Plan {
  fs::path root;
  fs::path package;          // the unpacked game package (VERSION, MANIFEST.sha256, bin/, kuf-cod.sh, ...)
  fs::path game_source;      // the player's game folder (only read)
  GameManifest manifest;
  fs::path setup_binary;     // this program and its assets, copied to share/setup (the uninstaller)
  fs::path setup_assets;
  fs::path icon;             // a PNG
  fs::path signers;          // the release key for updates (optional)
  bool menu = true;
  bool desktop = false;
  std::optional<steam::Profile> steam_profile;
};

struct Result {
  bool ok = false;
  std::string error;                  // why it failed (nothing was changed when it fails before kShortcuts)
  std::vector<std::string> notes;     // shortcut / Steam outcomes (shown at the end)
};

using ProgressFn = std::function<void(Stage, double fraction, const std::string& detail)>;

// Installs or repairs (an existing installation of this setup at plan.root). `cancel` is polled while copying.
Result Install(const Plan& plan, const ProgressFn& progress, const std::atomic<bool>& cancel);

// Space needed (the package, the game data, 64 MB to spare).
uint64_t SpaceNeeded(const Plan& plan);

struct RemoveOptions {
  bool shortcuts = true;
  bool steam = true;
  bool user_data = false;  // saves and settings: only after a separate confirmation in the interface
};
// Removes the installation at `root` (only the entries the setup creates; the folder itself only if then empty).
Result Uninstall(const fs::path& root, const RemoveOptions& options, const ProgressFn& progress);

// The text of the desktop entries (exposed for tests).
std::string GameDesktopEntry(const fs::path& root);
std::string UninstallDesktopEntry(const fs::path& root);
std::string LauncherScript(const fs::path& root);
std::string UpdaterScript(const fs::path& root);

}  // namespace kuf::setup
