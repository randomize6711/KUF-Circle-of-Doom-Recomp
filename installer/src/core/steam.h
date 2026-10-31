// "Add to Steam as a non-Steam game" (installer/README.md "Steam"): the Steam installations of the user (native,
// distribution packages, Flatpak), their profiles, and the shortcut entry in a profile's shortcuts.vdf. Changes are
// made only with Steam closed, after a backup, written atomically and verified by reading the file back; entries of
// other games are kept as they are. The entry is the setup's (found again by its tag), so a reinstall updates it
// instead of adding a second one, and the uninstaller removes only it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/util.h"

namespace kuf::setup::steam {

struct Install {
  fs::path root;        // the Steam folder (has userdata/)
  bool flatpak = false; // com.valvesoftware.Steam: it sees only the folders its sandbox allows
};
struct Profile {
  Install install;
  std::string account_id;  // userdata/<account_id>
  std::string name;        // the profile's name (PersonaName / AccountName from loginusers.vdf), or the ID
  bool most_recent = false;
  fs::path ShortcutsFile() const { return install.root / "userdata" / account_id / "config" / "shortcuts.vdf"; }
};

// The Steam folders under `home` (deduplicated by their real path; only those with a userdata folder).
std::vector<Install> FindInstalls(const fs::path& home);
std::vector<Profile> FindProfiles(const Install& install);
// True while a Steam client of this user runs (a process named steam or steamwebhelper).
bool SteamRunning();
// Tests: replaces the check above (nullptr: the real one).
void SetRunningCheckForTests(bool (*check)());

// The shortcut's ID as Steam computes it for a non-Steam game (CRC-32 of the quoted executable and the name, high bit
// set); Steam's grid images use it.
uint32_t ShortcutAppId(std::string_view exe_quoted, std::string_view name);

struct Shortcut {
  std::string name;        // "Kingdom Under Fire: Circle of Doom"
  fs::path exe;            // the stable launcher
  fs::path start_dir;
  fs::path icon;
};
constexpr const char* kTag = "kuf-cod-recomp";  // in the entry's tags: the setup's own

// Adds (or updates) the entry; `message` says what happened or why not. Nothing is written when Steam runs, when the
// file cannot be read as shortcuts.vdf, or when the result cannot be verified (the backup is then put back).
bool AddShortcut(const Profile& profile, const Shortcut& s, std::string* message);
// Removes the setup's entries (by tag, or by the same executable) and keeps every other one.
bool RemoveShortcut(const Profile& profile, const fs::path& exe, std::string* message);
// True if the profile's shortcuts.vdf has the setup's entry for `exe`.
bool HasShortcut(const Profile& profile, const fs::path& exe);

}  // namespace kuf::setup::steam
