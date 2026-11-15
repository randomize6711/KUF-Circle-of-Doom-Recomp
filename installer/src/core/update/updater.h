// The updater of the portable edition and the Windows installation (M17G; docs/UPDATES_WINDOWS.md). The same signed
// releases and rules as tools/updater/kuf_update.py (the Linux installation's), for a program folder laid out as
// the portable edition: Play KUF(.exe), Runtime/, GameFiles/, Data/.
//
//   - releases: GitHub (Windows: WinHTTP, HTTPS with the system's certificate checks; redirects only to GitHub's
//     hosts) or a local folder (releases.json and the files: tests and offline use);
//   - the newest release's update manifest is verified (namespace kuf-cod-recomp-manifest) before it is read; its
//     version, platform, package name, size and SHA-256 are checked; the package's signature (kuf-cod-recomp-release)
//     and size and SHA-256 before it is opened; no downgrade or reinstall unless asked;
//   - the package is the portable edition's archive; only Play KUF(.exe) and Runtime/ are taken from it, into
//     Runtime.next and Play KUF.next (GameFiles/ and Data/ - saves, settings, the prepared game - are never touched);
//   - Runtime.next is put in place by the launcher at its next start (ApplyStagedUpdate), when the game is not
//     running (Windows cannot rename a folder with a running program in it); the version before stays as
//     Runtime.previous (Rollback);
//   - the prepared game (Runtime/bin/kuf_cod) is carried over when the new package's generation id is the same, so
//     an update recompiles only when the code generator or compiler changed.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kuf::setup::update {

namespace fs = std::filesystem;

// "1.2.3" or "1.2.3-rc.1" (semver precedence); nullopt otherwise.
struct Version {
  int major = 0, minor = 0, patch = 0;
  std::vector<std::string> pre;  // empty: a release
  std::string text;
};
std::optional<Version> ParseVersion(const std::string& text);
int CompareVersions(const Version& a, const Version& b);  // <0, 0, >0

struct Asset {
  std::string name, url;
  uint64_t size = 0;
};
struct Release {
  std::string tag, html_url, published;
  bool draft = false, prerelease = false;
  std::vector<Asset> assets;
};

using DownloadProgress = std::function<void(uint64_t done, uint64_t total)>;

class ReleaseSource {
 public:
  virtual ~ReleaseSource() = default;
  virtual bool Releases(std::vector<Release>* out, std::string* why) = 0;
  virtual bool Fetch(const Asset& a, uint64_t limit, std::string* data, std::string* why) = 0;
  virtual bool Download(const Asset& a, const fs::path& dest, uint64_t limit, const DownloadProgress& progress,
                        const std::atomic<bool>& cancel, std::string* why) = 0;
};
// releases.json and the files in `dir`.
std::unique_ptr<ReleaseSource> DirectorySource(const fs::path& dir);
// GitHub's API for `repo` (owner/name) at `api` (https://api.github.com, or http://127.0.0.1:<port> for a local
// test server). Windows only (nullptr elsewhere: the Linux installation has tools/updater/kuf_update.py).
std::unique_ptr<ReleaseSource> HttpSource(const std::string& repo, const std::string& api, std::string* why);

struct UpdateConfig {
  std::string repo, key_identity = "release@kuf-cod-recomp", asset_prefix = "kuf-cod-recomp", platform, api,
                    channel;
};

// The program folder (the one with Play KUF and Runtime/).
struct ProgramFolder {
  fs::path root;
  fs::path Runtime() const { return root / "Runtime"; }
  fs::path Next() const { return root / "Runtime.next"; }
  fs::path Previous() const { return root / "Runtime.previous"; }
  fs::path Launcher() const;          // Play KUF(.exe)
  fs::path Pending() const { return root / ".update-pending"; }
  std::string CurrentVersion() const;  // Runtime/VERSION
  std::string PreviousVersion() const;
  std::string PendingVersion() const;
};

bool ReadUpdateConfig(const ProgramFolder& f, UpdateConfig* c, std::string* why);  // Runtime/update.conf
// Whether pre-releases are offered: --prerelease, update.conf's channel ("prerelease" or "stable"), and without a
// channel line, whether the installed version is itself a pre-release (0.9.0-rc.1 follows the pre-releases).
bool FollowsPrereleases(const UpdateConfig& c, const std::string& installed, bool forced);

enum class Exit { kDone = 0, kError = 1, kUpToDate = 3, kRefused = 4, kCancelled = 5 };

struct CheckResult {
  Exit exit = Exit::kError;
  std::string current, latest, published, notes, summary, page, error;
  bool update = false;
  uint64_t size = 0;
};

struct UpdateOptions {
  bool prerelease = false, allow_downgrade = false;
};

// Check: is a newer signed release available (exit kDone with update = true, or kUpToDate)?
CheckResult Check(const ProgramFolder& f, ReleaseSource& source, const UpdateOptions& o);

// Update: Check, then download, verify and stage the release as Runtime.next (applied at the next start).
// `stage` reports "verifying" and "installing".
CheckResult Update(const ProgramFolder& f, ReleaseSource& source, const UpdateOptions& o,
                   const DownloadProgress& progress, const std::function<void(const std::string&)>& stage,
                   const std::atomic<bool>& cancel);

// Puts a staged update in place (the launcher, before the game starts): Runtime -> Runtime.previous, Runtime.next ->
// Runtime, the same for the launcher program. Every rename is undone if one fails. *applied: the version now in
// place, when one was applied.
bool ApplyStagedUpdate(const ProgramFolder& f, std::string* applied, std::string* why);

// Back to Runtime.previous (and the launcher before it); the newer one becomes the previous.
bool Rollback(const ProgramFolder& f, std::string* now, std::string* why);

}  // namespace kuf::setup::update
