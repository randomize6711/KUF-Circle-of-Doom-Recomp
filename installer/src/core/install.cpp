#include "core/install.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>

#include <unistd.h>

#include "core/localbuild.h"

namespace kuf::setup {

namespace {

// The entries the setup creates inside an installation folder (and the updater's lock): only these are removed.
constexpr const char* kOwned[] = {"versions", "current", "previous", "kuf-cod", "kuf-update", "update.conf",
                                  "allowed_signers", "game", "share", "build-cache", ".update.lock", ".first-start",
                                  kRecordFile};

std::string NowUtc() {
  const std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
  return buf;
}

// The package's files from its MANIFEST.sha256 ("<sha256>  <path>"), the manifest itself added.
bool PackageFiles(const fs::path& package, std::vector<std::pair<std::string, std::string>>* out) {
  std::string text;
  if (!ReadFile(package / "MANIFEST.sha256", &text)) return false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 67 || line[64] != ' ' || line[65] != ' ') return false;
    const std::string path = line.substr(66);
    if (path.empty() || path[0] == '/' || path.find("..") != std::string::npos) return false;
    out->push_back({line.substr(0, 64), path});
  }
  return !out->empty();
}

std::string PackageVersion(const fs::path& package) {
  std::string v;
  ReadFile(package / "VERSION", &v);
  while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
  return v;
}

// Copies a file and checks it against `sha`; the executable bit kept.
bool CopyChecked(const fs::path& from, const fs::path& to, const std::string& sha,
                 const std::function<void(uint64_t)>& progress, const std::atomic<bool>& cancel, std::string* error) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  const std::string got = CopyHashed(from, to, progress, [&] { return cancel.load(); });
  if (cancel) {
    *error = "Installation cancelled.";
    return false;
  }
  if (got.empty()) {
    *error = "Could not copy " + from.string() + " to " + to.string() + ".";
    return false;
  }
  if (got != sha) {
    fs::remove(to, ec);
    *error = "The file " + from.filename().string() + " does not match the supported game version (or is damaged).";
    return false;
  }
  const auto perms = fs::status(from, ec).permissions();
  if (!ec) fs::permissions(to, perms & fs::perms(0755), ec);
  return true;
}

bool WriteText(const fs::path& path, const std::string& text, fs::perms mode, std::string* error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (!AtomicWrite(path, text, mode)) {
    *error = "Could not write " + path.string() + ".";
    return false;
  }
  return true;
}

// Points `link` at `target` atomically (a new symlink renamed over the old one).
bool Relink(const fs::path& link, const std::string& target) {
  const fs::path tmp = link.string() + ".kuf-new";
  std::error_code ec;
  fs::remove(tmp, ec);
  fs::create_symlink(target, tmp, ec);
  if (ec) return false;
  fs::rename(tmp, link, ec);
  return !ec;
}

// A desktop entry the setup created for `root` (its marker line), so it is never another program's file.
bool OurEntry(const fs::path& file, const fs::path& root) {
  std::string text;
  return ReadFile(file, &text) && text.find("X-KUF-Setup-Root=" + DesktopString(root.string()) + "\n") != std::string::npos;
}

}  // namespace

fs::path DefaultRoot() { return HomeDir() / "Games" / "Kingdom Under Fire - Circle of Doom"; }

const char* StageName(Stage s) {
  switch (s) {
    case kValidate: return "Validating game files";
    case kPrepare: return "Preparing installation";
    case kRuntime: return "Installing native runtime";
    case kGameData: return "Installing game data";
    case kGenerate: return "Generating native code";
    case kCompile: return "Compiling the executable";
    case kLink: return "Linking the executable";
    case kShortcuts: return "Creating shortcuts";
    case kFinalize: return "Finalizing installation";
    default: return "";
  }
}

std::string FormatRecord(const Record& r) {
  std::string out = "# Kingdom Under Fire: Circle of Doom, installed by its setup. The setup and its uninstaller read "
                    "this file; do not edit.\nformat=1\n";
  out += "version=" + r.version + "\ninstalled=" + r.installed + "\n";
  for (const auto& s : r.shortcuts) out += "shortcut=" + s.string() + "\n";
  for (const auto& [root, id] : r.steam) out += "steam=" + id + "|" + root.string() + "\n";
  return out;
}

std::optional<Record> ParseRecord(std::string_view text) {
  Record r;
  bool format = false;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return std::nullopt;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "format") {
      format = value == "1";
    } else if (key == "version") {
      r.version = value;
    } else if (key == "installed") {
      r.installed = value;
    } else if (key == "shortcut") {
      r.shortcuts.push_back(value);
    } else if (key == "steam") {
      const size_t bar = value.find('|');
      if (bar == std::string::npos) return std::nullopt;
      r.steam.push_back({value.substr(bar + 1), value.substr(0, bar)});
    }
  }
  if (!format || r.version.empty()) return std::nullopt;
  return r;
}

std::optional<Record> ReadRecord(const fs::path& root) {
  std::string text;
  if (!ReadFile(root / kRecordFile, &text)) return std::nullopt;
  return ParseRecord(text);
}

Target CheckTarget(const fs::path& root) {
  if (root.empty() || !root.is_absolute() || root == root.root_path()) return Target::kInvalid;
  std::error_code ec;
  if (root == HomeDir() || fs::equivalent(root, HomeDir(), ec)) return Target::kInvalid;
  if (fs::exists(root, ec)) {
    if (!fs::is_directory(root, ec)) return Target::kInvalid;
    if (ReadRecord(root)) return access(root.c_str(), W_OK) == 0 ? Target::kExisting : Target::kNotWritable;
    if (!fs::is_empty(root, ec)) return Target::kNotEmpty;
    return access(root.c_str(), W_OK) == 0 ? Target::kNew : Target::kNotWritable;
  }
  return CanCreateIn(root) ? Target::kNew : Target::kNotWritable;
}

std::string LauncherScript(const fs::path& root) {
  return "#!/usr/bin/env bash\n"
         "# Kingdom Under Fire: Circle of Doom: the stable launcher (application menu, desktop, Steam). Written by the\n"
         "# setup; the game's own launcher (current/kuf-cod.sh) sets up the data folders and runs the game.\n"
         "export KUF_GAME_DIR=" + ShellQuote((root / "game").string()) + "\n"
         "exec " + ShellQuote((root / "current" / "kuf-cod.sh").string()) + " \"$@\"\n";
}

std::string UpdaterScript(const fs::path& root) {
  return "#!/usr/bin/env bash\n"
         "exec python3 -I " + ShellQuote((root / "current" / "kuf_update.py").string()) + " --install-root " +
         ShellQuote(root.string()) + " \"$@\"\n";
}

std::string GameDesktopEntry(const fs::path& root) {
  return "[Desktop Entry]\n"
         "Type=Application\n"
         "Version=1.0\n"
         "Name=Kingdom Under Fire: Circle of Doom\n"
         "GenericName=Action RPG\n"
         "Comment=The native PC version (requires your own game files)\n"
         "Exec=" + DesktopExecArg((root / "kuf-cod").string()) + "\n"
         "Path=" + DesktopString(root.string()) + "\n"
         "Icon=" + DesktopString((root / "share" / "kuf-cod-recomp.png").string()) + "\n"
         "Terminal=false\n"
         "StartupNotify=true\n"
         "Categories=Game;ActionGame;RolePlaying;\n"
         "X-KUF-Setup-Root=" + DesktopString(root.string()) + "\n";
}

std::string UninstallDesktopEntry(const fs::path& root) {
  return "[Desktop Entry]\n"
         "Type=Application\n"
         "Version=1.0\n"
         "Name=Uninstall Kingdom Under Fire: Circle of Doom\n"
         "Comment=Removes the installed game (your saves and settings are kept unless you choose otherwise)\n"
         "Exec=" + DesktopExecArg((root / "share" / "setup" / "kuf-setup").string()) + " --uninstall " +
         DesktopExecArg(root.string()) + "\n"
         "Icon=" + DesktopString((root / "share" / "kuf-cod-recomp.png").string()) + "\n"
         "Terminal=false\n"
         "Categories=Game;\n"
         "X-KUF-Setup-Root=" + DesktopString(root.string()) + "\n";
}

uint64_t SpaceNeeded(const Plan& plan) {
  uint64_t package = 0;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(plan.package, ec)) {
    if (e.is_regular_file(ec)) package += e.file_size(ec);
  }
  return package + plan.manifest.total + kBuildSpace + (uint64_t(64) << 20);
}

Result Install(const Plan& plan, const ProgressFn& progress, const std::atomic<bool>& cancel) {
  Result result;
  std::error_code ec;
  auto fail = [&](std::string why) {
    result.error = std::move(why);
    return result;
  };
  auto report = [&](Stage s, double f, const std::string& d) {
    if (progress) progress(s, f, d);
  };

  // 1. Validate: the target, the game files (present, sizes, the executable's version), the package, the space.
  report(kValidate, 0, "Checking the game files");
  const Target target = CheckTarget(plan.root);
  if (target == Target::kInvalid || target == Target::kNotEmpty || target == Target::kNotWritable) {
    return fail(target == Target::kNotEmpty ? "The installation folder is not empty and holds no installation of "
                                              "this game. Choose an empty or new folder."
                : target == Target::kNotWritable ? "The installation folder cannot be written."
                                                 : "This installation folder cannot be used.");
  }
  const bool repair = target == Target::kExisting;
  const GameCheck check = CheckGameFolder(plan.game_source, plan.manifest);
  if (!check.Ok()) {
    return fail(!check.folder_ok          ? "The game folder cannot be read."
                : check.executable_encrypted
                    ? "This default.xex is encrypted. This setup contains no console decryption keys and cannot decrypt "
                      "it; it can only use an unencrypted default.xex."
                : !check.executable_known ? "The game folder does not hold the supported game version (default.xex)."
                                          : "Game files are missing or have the wrong size (" +
                                                std::to_string(check.missing.size() + check.wrong_size.size()) +
                                                " files).");
  }
  std::vector<std::pair<std::string, std::string>> package_files;
  const std::string version = PackageVersion(plan.package);
  if (version.empty() || !PackageFiles(plan.package, &package_files)) {
    return fail("The setup's game package is damaged (no VERSION or MANIFEST.sha256).");
  }
  const uint64_t needed = SpaceNeeded(plan);
  const uint64_t free = FreeBytes(plan.root);
  if (!repair && free < needed) {
    return fail("Not enough free space: " + HumanBytes(needed) + " needed, " + HumanBytes(free) + " free.");
  }
  report(kValidate, 1, "The game files are complete");

  // 2. Prepare: a new installation is built in a staging folder next to the target and renamed into place at the
  // end; a repair works in the installation itself (the program as the updater installs a version, the game data
  // file by file through temporary names).
  report(kPrepare, 0, repair ? "Repairing the existing installation" : "Preparing a new installation");
  fs::path work = plan.root;
  if (!repair) {
    fs::create_directories(plan.root.parent_path(), ec);
    // A crash or power cut leaves the setup's own staging folder behind (several GB): it is never used again.
    const std::string leftover = "." + plan.root.filename().string() + ".kuf-staging-";
    for (const auto& e : fs::directory_iterator(plan.root.parent_path(), ec)) {
      if (e.path().filename().string().rfind(leftover, 0) == 0) fs::remove_all(e.path(), ec);
    }
    std::string tmpl = (plan.root.parent_path() / ("." + plan.root.filename().string() + ".kuf-staging-XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) return fail("Could not create a folder next to " + plan.root.string() + ".");
    work = buf.data();
    fs::permissions(work, fs::perms(0755), ec);  // mkdtemp makes it private; the installation is a normal folder
  }
  auto abort = [&](std::string why) {
    if (!repair) fs::remove_all(work, ec);  // the staging folder is the setup's own
    return fail(std::move(why));
  };
  report(kPrepare, 1, "");

  // 3. The native runtime: the package into versions/<version>, each file checked against its manifest.
  const fs::path versions = work / "versions";
  const fs::path final_dir = versions / version;
  const fs::path stage_dir = repair ? versions / (".staging-" + version) : final_dir;
  fs::remove_all(stage_dir, ec);
  uint64_t runtime_total = 0, runtime_done = 0;
  for (const auto& [sha, path] : package_files) runtime_total += fs::file_size(plan.package / path, ec);
  for (const auto& [sha, path] : package_files) {
    std::string error;
    if (!CopyChecked(plan.package / path, stage_dir / path, sha,
                     [&](uint64_t n) {
                       runtime_done += n;
                       report(kRuntime, runtime_total ? double(runtime_done) / double(runtime_total) : 1, path);
                     },
                     cancel, &error)) {
      if (repair) fs::remove_all(stage_dir, ec);
      return abort(error.find("supported game") != std::string::npos
                       ? "The setup's game package is damaged (" + path + " does not match its manifest)."
                       : error);
    }
  }
  fs::copy_file(plan.package / "MANIFEST.sha256", stage_dir / "MANIFEST.sha256", fs::copy_options::overwrite_existing,
                ec);
  if (ec) return abort("Could not copy the package's manifest.");
  // 4. The game data: each file copied and checked against the supported version's SHA-256 (a repair checks the
  // installed copy and copies only what differs).
  const fs::path game = work / "game";
  uint64_t done = 0;
  for (const GameFile& f : plan.manifest.files) {
    if (cancel) return abort("Installation cancelled.");
    const fs::path to = game / f.path;
    const bool is_xex = f.path == "default.xex";  // compared by its memory image (core/game_data.h)
    if (repair && (is_xex ? InspectXex(to).image_sha256 == f.sha256
                          : fs::file_size(to, ec) == f.size && !ec && Sha256File(to) == f.sha256)) {
      done += f.size;
      report(kGameData, double(done) / double(plan.manifest.total), f.path);
      continue;
    }
    std::string error;
    if (!CopyChecked(plan.game_source / f.path, to, is_xex ? Sha256File(plan.game_source / f.path) : f.sha256,
                     [&](uint64_t n) {
                       done += n;
                       report(kGameData, double(done) / double(plan.manifest.total), f.path);
                     },
                     cancel, &error)) {
      return abort(error);
    }
    fs::permissions(to, fs::perms(0644), ec);  // the copy is the installation's, not read-only like a disc
  }

  // 5. The native game: generated and compiled here from the game's default.xex. A cancelled new installation removes
  // its cache with it; after a failure or a crash the finished objects are kept for the next attempt. The cache of a
  // new installation lives next to the staging folder until the installation is complete.
  const fs::path cache = repair ? work / "build-cache"
                                : plan.root.parent_path() / ("." + plan.root.filename().string() + ".kuf-buildcache");
  {
    BuildRequest build;
    build.package = stage_dir;
    build.xex = game / "default.xex";
    build.cache = cache;
    build.output = stage_dir / "bin" / "kuf_cod";
    build.jobs = plan.build_jobs;
    std::string why;
    const bool built = LocalBuild(
        build,
        [&](BuildPhase p, double f, const std::string& d) {
          report(p == BuildPhase::kGenerate ? kGenerate : p == BuildPhase::kCompile ? kCompile : kLink, f, d);
        },
        cancel, &why);
    if (!built) {
      if (repair) fs::remove_all(stage_dir, ec);
      if (cancel && !repair) fs::remove_all(cache, ec);
      return abort(why);
    }
    if (!fs::exists(stage_dir / "bin" / "kuf_cod", ec)) return abort("The build wrote no program.");
  }

  if (repair) {
    const std::string old = [&] {
      std::error_code e;
      return fs::read_symlink(work / "current", e).filename().string();
    }();
    if (fs::exists(final_dir, ec)) {  // the same version: the verified copy replaces it
      const fs::path aside = versions / (".old-" + version);
      fs::remove_all(aside, ec);
      fs::rename(final_dir, aside, ec);
      fs::rename(stage_dir, final_dir, ec);
      fs::remove_all(aside, ec);
    } else {
      fs::rename(stage_dir, final_dir, ec);
      if (!old.empty() && old != version) WriteText(work / "previous", old + "\n", fs::perms(0644), &result.error);
    }
    if (ec) return abort("Could not put the new program files in place.");
  }

  if (!Relink(work / "current", "versions/" + version)) return abort("Could not switch to the new version.");

  // The launchers, the updater's configuration (kept when present: it may be the player's), the icon, the setup.
  std::string error;
  if (!WriteText(work / "kuf-cod", LauncherScript(plan.root), fs::perms(0755), &error) ||
      !WriteText(work / "kuf-update", UpdaterScript(plan.root), fs::perms(0755), &error)) {
    return abort(error);
  }
  if (!fs::exists(work / "update.conf", ec)) {
    if (!WriteText(work / "update.conf",
                   std::string("# Updates (kuf-update check / update): the release repository and the release key "
                               "identity.\nrepo = ") + kDefaultRepo + "\nkey_identity = release@kuf-cod-recomp\n",
                   fs::perms(0644), &error)) {
      return abort(error);
    }
  }
  if (!plan.signers.empty() && !fs::exists(work / "allowed_signers", ec)) {
    fs::copy_file(plan.signers, work / "allowed_signers", ec);
  }
  fs::create_directories(work / "share" / "setup", ec);
  if (!plan.icon.empty()) fs::copy_file(plan.icon, work / "share" / "kuf-cod-recomp.png", fs::copy_options::overwrite_existing, ec);
  if (!plan.setup_binary.empty() && fs::exists(plan.setup_binary, ec)) {
    const fs::path setup = work / "share" / "setup";
    fs::copy_file(plan.setup_binary, setup / "kuf-setup.new", fs::copy_options::overwrite_existing, ec);
    fs::permissions(setup / "kuf-setup.new", fs::perms(0755), ec);
    fs::rename(setup / "kuf-setup.new", setup / "kuf-setup", ec);
    if (!plan.setup_assets.empty()) {
      fs::remove_all(setup / "assets", ec);
      fs::copy(plan.setup_assets, setup / "assets", fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
    }
  }
  Record record;
  if (auto old = ReadRecord(work)) record = *old;
  record.version = version;
  record.installed = NowUtc();
  if (!WriteText(work / kRecordFile, FormatRecord(record), fs::perms(0644), &error)) return abort(error);

  // Commit: the staging folder becomes the installation (an empty target folder is replaced).
  if (!repair) {
    fs::rename(cache, work / "build-cache", ec);
    if (ec) return abort("Could not move the build's objects into the installation.");
    if (fs::exists(plan.root, ec)) fs::remove(plan.root, ec);  // empty (CheckTarget)
    fs::rename(work, plan.root, ec);
    if (ec) return abort("Could not move the installation into " + plan.root.string() + ".");
  }

  // 6. Shortcuts (optional: a failure here is reported, the installation stays).
  report(kShortcuts, 0, "");
  auto add_entry = [&](const fs::path& file, const std::string& text, bool executable, const char* what) {
    if (fs::exists(file, ec) && !OurEntry(file, plan.root)) {
      result.notes.push_back(std::string(what) + ": " + file.string() + " exists and is not this installation's; "
                             "it was left as it is.");
      return;
    }
    std::string why;
    if (WriteText(file, text, executable ? fs::perms(0755) : fs::perms(0644), &why)) {
      if (std::find(record.shortcuts.begin(), record.shortcuts.end(), file) == record.shortcuts.end()) {
        record.shortcuts.push_back(file);
      }
      result.notes.push_back(std::string(what) + " created.");
    } else {
      result.notes.push_back(std::string(what) + " could not be created: " + why);
    }
  };
  const fs::path apps = XdgDataHome() / "applications";
  if (plan.menu) {
    add_entry(apps / (std::string(kDesktopId) + ".desktop"), GameDesktopEntry(plan.root), false,
              "Application-menu shortcut");
    add_entry(apps / (std::string(kDesktopId) + "-uninstall.desktop"), UninstallDesktopEntry(plan.root), false,
              "Uninstall entry in the application menu");
  }
  report(kShortcuts, 0.4, "");
  if (plan.desktop) {
    if (auto desktop = DesktopDir()) {
      add_entry(*desktop / (std::string(kDesktopId) + ".desktop"), GameDesktopEntry(plan.root), true,
                "Desktop shortcut");
      // Some desktops (GNOME's, for one) ask before starting a launcher placed on the desktop; that setting is the
      // player's: it is explained, not changed.
      result.notes.push_back("Some desktops ask the first time: right-click the desktop icon and choose \"Allow "
                             "Launching\".");
    } else {
      result.notes.push_back("Desktop shortcut: no desktop folder was found; none was created.");
    }
  }
  report(kShortcuts, 0.7, "");
  if (plan.steam_profile) {
    std::string message;
    steam::Shortcut s{kGameName, plan.root / "kuf-cod", plan.root, plan.root / "share" / "kuf-cod-recomp.png"};
    if (steam::AddShortcut(*plan.steam_profile, s, &message)) {
      const auto entry = std::make_pair(plan.steam_profile->install.root, plan.steam_profile->account_id);
      if (std::find(record.steam.begin(), record.steam.end(), entry) == record.steam.end()) {
        record.steam.push_back(entry);
      }
      result.notes.push_back("Steam: " + message + " It appears in the library after Steam starts again.");
      if (plan.steam_profile->install.flatpak) {
        result.notes.push_back("Steam (Flatpak) may need access to the installation folder: flatpak override --user "
                               "--filesystem=" + plan.root.string() + " com.valvesoftware.Steam");
      }
    } else {
      result.notes.push_back("Steam: " + message + " To add it by hand: in Steam, Games > Add a Non-Steam Game to My "
                             "Library > Browse, choose " + (plan.root / "kuf-cod").string() + ".");
    }
  }
  WriteText(plan.root / kRecordFile, FormatRecord(record), fs::perms(0644), &error);
  report(kShortcuts, 1, "");

  // 7. Finalize: the layout the launcher and the updater expect.
  report(kFinalize, 0, "Checking the installation");
  for (const char* must : {"kuf-cod", "kuf-update", "current/kuf-cod.sh", "current/bin/kuf_cod", "game/default.xex",
                           kRecordFile}) {
    if (!fs::exists(plan.root / must, ec)) return fail(std::string("The installation is incomplete: ") + must + " is missing.");
  }
  report(kFinalize, 1, "Done");
  result.ok = true;
  return result;
}

Result Uninstall(const fs::path& root, const RemoveOptions& options, const ProgressFn& progress) {
  Result result;
  std::error_code ec;
  const auto record = ReadRecord(root);
  if (!record) {
    result.error = "No installation of this game by its setup was found in " + root.string() + ".";
    return result;
  }
  if (progress) progress(kShortcuts, 0, "Removing shortcuts");
  bool steam_failed = false;
  if (options.steam) {
    for (const auto& [steam_root, account] : record->steam) {
      steam::Profile p{{steam_root, false}, account, account, false};
      std::string message;
      if (!steam::RemoveShortcut(p, root / "kuf-cod", &message)) steam_failed = true;
      result.notes.push_back("Steam: " + message);
    }
    if (steam_failed) {
      result.error = "The Steam shortcut could not be removed (is Steam running?). Nothing else was removed.";
      return result;
    }
  }
  if (options.shortcuts) {
    for (const fs::path& s : record->shortcuts) {
      if (fs::exists(s, ec) && OurEntry(s, root)) fs::remove(s, ec);
    }
    result.notes.push_back("Shortcuts removed.");
  }
  if (progress) progress(kFinalize, 0.3, "Removing the game");
  for (const char* name : kOwned) {
    if (std::string(name) == kRecordFile) continue;  // last: the record says this folder is ours until the end
    const fs::path p = root / name;
    if (fs::is_symlink(p, ec) || fs::is_regular_file(p, ec)) {
      fs::remove(p, ec);
    } else if (fs::is_directory(p, ec)) {
      fs::remove_all(p, ec);
    }
  }
  fs::remove(root / kRecordFile, ec);
  if (fs::is_empty(root, ec)) {
    fs::remove(root, ec);
    result.notes.push_back("The installation folder was removed.");
  } else {
    result.notes.push_back("Files you added to " + root.string() + " were kept.");
  }
  if (options.user_data) {
    fs::remove_all(XdgDataHome() / "kuf-cod-recomp" / "data", ec);
    fs::remove_all(XdgConfigHome() / "kuf-cod-recomp", ec);
    result.notes.push_back("Saves and settings were removed.");
  } else {
    result.notes.push_back("Your saves and settings were kept (" + (XdgDataHome() / "kuf-cod-recomp" / "data").string() +
                           ").");
  }
  if (progress) progress(kFinalize, 1, "Done");
  result.ok = true;
  return result;
}

}  // namespace kuf::setup
