// Preparing the native game on the player's machine (docs/LOCAL_BUILD.md): the package carries the code generator, a
// compiler, the headers and the project's own prebuilt code, and never the game's code. This generates C++ from the
// player's default.xex, compiles it into a private cache and links bin/kuf_cod. The objects stay in the cache, so a
// later update whose generation id is the same only links again, and an interrupted build continues.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "core/util.h"

namespace kuf::setup {

enum class BuildPhase { kGenerate, kCompile, kLink };

struct BuildRequest {
  fs::path package;     // the unpacked package (builder/, bin/, ...)
  fs::path xex;         // the checked, unencrypted default.xex
  fs::path cache;       // private to the installation; objects of one generation id are kept here
  fs::path output;      // the program to write, usually <package>/bin/kuf_cod; written last, by a rename
  int jobs = 0;         // 0: from the processors and the free memory
};

struct BuildStats {
  std::string generation_id;
  int jobs = 0;
  int compiled = 0;     // objects made now
  int reused = 0;       // objects found in the cache
  double generate_seconds = 0, compile_seconds = 0, link_seconds = 0;
};

using BuildProgress = std::function<void(BuildPhase, double fraction, const std::string& detail)>;

// False with `error` set; the output is then untouched and finished objects stay in the cache. `cancel` is polled.
bool LocalBuild(const BuildRequest& request, const BuildProgress& progress, const std::atomic<bool>& cancel,
                std::string* error, BuildStats* stats = nullptr);

// Compile jobs for this machine: the processors, limited so each job has kBuildJobMemory of the available memory.
inline constexpr uint64_t kBuildJobMemory = uint64_t(1) << 30;  // 1 GiB (measured peak of one job: 0.6 GiB)
int DefaultBuildJobs(unsigned processors, uint64_t available_memory);

// Temporary and cache space a build needs, on top of the program it writes.
inline constexpr uint64_t kBuildSpace = uint64_t(1200) << 20;

// One line of the recipe's flags with @B@ (the package's builder/) and @W@ (the work folder) put in (for tests).
std::string ExpandRecipe(std::string text, const fs::path& builder, const fs::path& work);

}  // namespace kuf::setup
