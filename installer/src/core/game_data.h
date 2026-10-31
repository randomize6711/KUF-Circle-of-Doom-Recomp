// The player's original game files (installer/README.md "Game data"): the supported version's reference list
// (installer/assets/game_v1.tsv: SHA-256, size and path of each file the game reads; the console's $SystemUpdate
// folder is not needed) and the checks against a chosen folder. The folder is only ever read.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/util.h"

namespace kuf::setup {

struct GameFile {
  std::string sha256;
  uint64_t size = 0;
  std::string path;  // relative, '/'-separated
};

struct GameManifest {
  std::string name;  // "Xbox 360, base version (default.xex v1)"
  std::vector<GameFile> files;
  uint64_t total = 0;
  const GameFile* Find(std::string_view path) const;
};
// Parses the reference list (tab-separated: sha256, size, path; '#' comment lines). Empty on a malformed list.
GameManifest ParseGameManifest(std::string_view text, std::string name);

// The quick check made when a folder is chosen: every file present with its size, and default.xex's SHA-256 (the
// executable decides the version; the rest is hashed while it is copied).
struct GameCheck {
  bool folder_ok = false;        // a readable folder
  bool executable_found = false;
  bool executable_known = false; // default.xex is the supported version
  std::vector<std::string> missing;
  std::vector<std::string> wrong_size;
  uint64_t bytes = 0;            // of the files found with their sizes
  bool Ok() const { return folder_ok && executable_known && missing.empty() && wrong_size.empty(); }
};
GameCheck CheckGameFolder(const fs::path& folder, const GameManifest& manifest);

// The folder a player picked may be one level above the game (a disc dump's parent): the folder itself, else its
// only subfolder that holds default.xex.
fs::path FindGameRoot(const fs::path& picked);

}  // namespace kuf::setup
