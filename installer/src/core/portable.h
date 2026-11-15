// The portable edition (M16C, docs/PORTABLE.md): one folder that holds the program, the player's game files and
// everything the game writes. Download, extract, add the game files, launch. No installation, no system changes.
//
//   <root>/Play KUF(.exe)            the launcher (src/frontend, this logic)
//   <root>/GameFiles/                the player's game files: a game folder, a disc image, or both parts of a retail
//                                    dump (its folder and an unencrypted default.xex); only read
//   <root>/Runtime/                  the game package (bin/, builder/, licenses/); bin/kuf_cod(.exe) is made here
//   <root>/Data/{user,saves,cache,logs,game}   settings, saves, caches, logs; game/: files taken from a disc image
//   <root>/Data/portable.txt         what was prepared (paths relative to <root>, so the folder can be moved)
//
// Everything is relative to the launcher's own folder. Shared by the Linux and Windows launchers.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/game_data.h"
#include "core/localbuild.h"
#include "core/util.h"

namespace kuf::setup {

struct PortableLayout {
  fs::path root;  // the launcher's folder (read only is allowed: then `data` is elsewhere)
  fs::path data;  // <root>/Data, or a writable folder the player chose
  fs::path GameFiles() const { return root / "GameFiles"; }
  fs::path Runtime() const { return root / "Runtime"; }
  fs::path User() const { return data / "user"; }
  fs::path Saves() const { return data / "saves"; }
  fs::path Cache() const { return data / "cache"; }
  fs::path Logs() const { return data / "logs"; }
  fs::path ExtractedGame() const { return data / "game"; }  // files taken from a disc image or a retail dump
  fs::path BuildCache() const { return data / "build-cache"; }
  fs::path Program() const;          // where kuf_cod(.exe) is made: <Runtime>/bin, or <data>/bin when Runtime is read only
  fs::path StateFile() const { return data / "portable.txt"; }
};
PortableLayout DefaultPortableLayout(const fs::path& root);

// A possible game source found in GameFiles/, with its check (core/game_data.h) and a separate unencrypted
// default.xex paired automatically when the source's own is encrypted and one of the supported version is there too.
struct PortableSource {
  fs::path path;        // a folder or a disc image
  fs::path executable;  // the separate unencrypted default.xex used, or empty
  GameCheck check;
  std::string label;    // for the selection list: the path below GameFiles and what it is
};
// Every source below `game_files` (the folder itself, folders and disc images up to two levels down), best first:
// usable ones, then the others with their problem. Hidden files and our own notes are skipped.
std::vector<PortableSource> FindPortableSources(const fs::path& game_files, const GameManifest& manifest);

// What Data/portable.txt records about the last preparation.
struct PortableState {
  std::string source;        // relative to <root> (UTF-8, '/' separators)
  std::string executable;    // likewise, or ""
  std::string game_dir;      // the folder the game reads: relative to <root> or to <data> ("data:game")
  std::string image_sha256;  // of the executable used (the version prepared)
  std::string generation;    // the build's generation id (core/localbuild.h)
  std::string program_sha256;
  std::string package_version;
};
bool ReadPortableState(const fs::path& file, PortableState* state);
bool WritePortableState(const fs::path& file, const PortableState& state);

// Ready to play without preparing: the state names this source, the program exists with its recorded hash, and the
// package's generation id is the one recorded. `why` says what is missing otherwise.
bool PortableReady(const PortableLayout& layout, const PortableSource& source, const PortableState& state,
                   std::string* why);

enum class PortablePhase { kCopy, kGenerate, kCompile, kLink };
using PortableProgress = std::function<void(PortablePhase, double fraction, const std::string& detail)>;

// Copies the source's game files (a folder or a disc image; with `source.executable` as default.xex) into `to`, every
// file checked against the reference list (through `to`.part, renamed when complete; an old `to` replaced only then).
bool CopyGameFiles(const PortableSource& source, const GameManifest& manifest, const fs::path& to,
                   const std::function<void(double, const std::string&)>& progress, const std::atomic<bool>& cancel,
                   std::string* error);

// Prepares the game: a disc image's or retail dump's files copied (and checked by SHA-256) into Data/game, a game
// folder used where it is; then the local build into Program(); then the state file. The player's files are only
// read. On failure or cancel nothing that worked before is changed (the old program stays until the new one is done).
bool PreparePortable(const PortableLayout& layout, const PortableSource& source, const GameManifest& manifest,
                     const PortableProgress& progress, const std::atomic<bool>& cancel, std::string* error,
                     int jobs = 0);

// Verify Game Files: every file of the source against the reference list's SHA-256 (slow: reads 4 GB).
// `bad` lists the files that are missing or differ.
bool VerifyPortableGameFiles(const PortableLayout& layout, const PortableState& state, const GameManifest& manifest,
                             std::vector<std::string>* bad, const std::function<void(double)>& progress,
                             const std::atomic<bool>& cancel);

// The game's command line and the environment entries it needs (Linux: the bundled libstdc++ when the system's is
// older), with saves, settings, caches and logs inside the portable folder.
struct LaunchCommand {
  std::vector<std::string> argv;
  std::vector<std::string> env;
};
LaunchCommand PortableLaunchCommand(const PortableLayout& layout, const PortableState& state,
                                    const std::vector<std::string>& extra_args = {});

// The package's version (Runtime/VERSION) and generation id (Runtime/builder/recipe.txt).
std::string PackageVersion(const fs::path& runtime);
std::string PackageGeneration(const fs::path& runtime);

}  // namespace kuf::setup
