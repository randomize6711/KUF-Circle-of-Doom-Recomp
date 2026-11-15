#include "core/portable.h"

#include <algorithm>
#include <sstream>

#include "core/platform.h"
#include "core/sha256.h"
#include "core/xex_decrypt.h"

namespace kuf::setup {

namespace {

#if defined(_WIN32)
constexpr const char* kProgramName = "kuf_cod.exe";
#else
constexpr const char* kProgramName = "kuf_cod";
#endif

std::string Lower(std::string s) {
  for (char& c : s) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  return s;
}

// `p` relative to `base` as UTF-8 with '/', or "" when it is not below it.
std::string RelativeUtf8(const fs::path& p, const fs::path& base) {
  std::error_code ec;
  const fs::path rel = fs::relative(p, base, ec);
  if (ec || rel.empty()) return {};
  const std::string s = PathUtf8(rel.generic_u8string());
  if (s == ".." || s.rfind("../", 0) == 0) return {};
  return s;
}

// A recorded location: "data:<rel>" below the data folder, else relative to the root.
std::string Record(const PortableLayout& l, const fs::path& p) {
  if (p.empty()) return {};
  const std::string in_data = RelativeUtf8(p, l.data);
  if (!in_data.empty()) return "data:" + in_data;
  return RelativeUtf8(p, l.root);
}
fs::path Resolve(const PortableLayout& l, const std::string& recorded) {
  if (recorded.empty()) return {};
  if (recorded.rfind("data:", 0) == 0) return l.data / PathFromUtf8(recorded.substr(5));
  return l.root / PathFromUtf8(recorded);
}

// What identifies the executable prepared: its image's SHA-256; for an encrypted one the setup decrypts (whose image
// is not known without the key) its file's SHA-256, which the decryption check ties to the supported image.
std::string UsedImage(const GameCheck& c) {
  if (c.separate) return c.separate_xex.image_sha256;
  return c.decrypt ? "file:" + c.dump_xex.file_sha256 : c.dump_xex.image_sha256;
}

bool Writable(const fs::path& dir) {
  std::error_code ec;
  return fs::is_directory(dir, ec) && CanCreateIn(dir);
}

}  // namespace

fs::path PortableLayout::Program() const {
  return (Writable(Runtime() / "bin") ? Runtime() / "bin" : data / "bin") / kProgramName;
}

PortableLayout DefaultPortableLayout(const fs::path& root) { return {root, root / "Data"}; }

std::string PackageVersion(const fs::path& runtime) {
  std::string v;
  if (!ReadFile(runtime / "VERSION", &v)) return {};
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
  return v;
}

std::string PackageGeneration(const fs::path& runtime) {
  std::string text;
  if (!ReadFile(runtime / "builder" / "recipe.txt", &text)) return {};
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("generation_id ", 0) == 0) return line.substr(14);
  }
  return {};
}

std::vector<PortableSource> FindPortableSources(const fs::path& game_files, const GameManifest& manifest) {
  std::vector<fs::path> folders, images, executables;
  std::error_code ec;
  if (fs::is_directory(game_files, ec) && GameSource::Folder(game_files).Find("default.xex").found) {
    folders.push_back(game_files);
  }
  for (auto it = fs::recursive_directory_iterator(game_files, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::path& p = it->path();
    const std::string name = PathUtf8(p.filename());
    if (!name.empty() && name[0] == '.') {
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (it->is_directory(ec)) {
      if (name == "$SystemUpdate" || it.depth() >= 2) it.disable_recursion_pending();
      if (it.depth() < 2 && GameSource::Folder(p).Find("default.xex").found) folders.push_back(p);
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    const std::string ext = Lower(PathUtf8(p.extension()));
    if (ext == ".iso" && it.depth() < 2) images.push_back(p);
    if (ext == ".xex") executables.push_back(p);
  }
  // The unencrypted executables of the supported version, for pairing with a source whose own is encrypted.
  std::vector<fs::path> plain;
  for (const fs::path& x : executables) {
    if (JudgeExecutable(InspectXex(x), manifest) == Executable::kSupported) plain.push_back(x);
  }
  std::vector<PortableSource> out;
  const auto add = [&](const fs::path& path, const GameSource& src) {
    PortableSource s;
    s.path = path;
    s.check = CheckGame(src, manifest);
    if (!s.check.Ok() && NeedsSeparateExecutable(s.check.dump_verdict)) {
      for (const fs::path& x : plain) {
        const fs::path own = src.kind() == GameSource::Kind::kFolder ? src.Find("default.xex").file : fs::path();
        if (!own.empty() && fs::equivalent(own, x, ec)) continue;
        GameCheck with = CheckGame(src, manifest, x);
        if (with.Ok()) {
          s.check = with;
          s.executable = x;
          break;
        }
      }
    }
    std::string where = RelativeUtf8(path, game_files);
    if (where.empty() || where == ".") where = "GameFiles";
    s.label = where + (src.kind() == GameSource::Kind::kIso ? " (disc image)" : " (folder)");
    if (!s.executable.empty()) s.label += ", with the unencrypted " + RelativeUtf8(s.executable, game_files);
    out.push_back(std::move(s));
  };
  for (const fs::path& f : folders) add(f, GameSource::Folder(f));
  for (const fs::path& i : images) add(i, GameSource::Iso(i));
  std::stable_sort(out.begin(), out.end(), [](const PortableSource& a, const PortableSource& b) {
    return a.check.Ok() && !b.check.Ok();
  });
  return out;
}

bool ReadPortableState(const fs::path& file, PortableState* s) {
  std::string text;
  if (!ReadFile(file, &text)) return false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "source") s->source = v;
    else if (k == "executable") s->executable = v;
    else if (k == "game_dir") s->game_dir = v;
    else if (k == "image_sha256") s->image_sha256 = v;
    else if (k == "generation") s->generation = v;
    else if (k == "program_sha256") s->program_sha256 = v;
    else if (k == "package_version") s->package_version = v;
  }
  return !s->game_dir.empty() && !s->program_sha256.empty();
}

bool WritePortableState(const fs::path& file, const PortableState& s) {
  std::ostringstream o;
  o << "# Written by the portable launcher (docs/PORTABLE.md); paths relative to its folder.\n"
    << "source=" << s.source << "\nexecutable=" << s.executable << "\ngame_dir=" << s.game_dir
    << "\nimage_sha256=" << s.image_sha256 << "\ngeneration=" << s.generation
    << "\nprogram_sha256=" << s.program_sha256 << "\npackage_version=" << s.package_version << "\n";
  return AtomicWrite(file, o.str());
}

bool PortableReady(const PortableLayout& l, const PortableSource& source, const PortableState& s, std::string* why) {
  const auto no = [&](const char* reason) {
    if (why) *why = reason;
    return false;
  };
  if (s.program_sha256.empty()) return no("The game has not been prepared yet.");
  if (s.source != Record(l, source.path) || s.executable != Record(l, source.executable)) {
    return no("Other game files were chosen.");
  }
  if (s.image_sha256 != UsedImage(source.check)) return no("The game files changed.");
  if (s.generation != PackageGeneration(l.Runtime())) return no("This version of the program needs a new preparation.");
  std::error_code ec;
  const fs::path game = Resolve(l, s.game_dir);
  if (!GameSource::Folder(game).Find("default.xex").found) return no("The prepared game files are missing.");
  if (!fs::is_regular_file(l.Program(), ec) || Sha256File(l.Program()) != s.program_sha256) {
    return no("The prepared program is missing or damaged.");
  }
  return true;
}

bool CopyGameFiles(const PortableSource& source, const GameManifest& manifest, const fs::path& to,
                   const std::function<void(double, const std::string&)>& progress, const std::atomic<bool>& cancel,
                   std::string* error) {
  std::error_code ec;
  const GameSource src = GameSource::Open(source.path);
  const GameCheck check = CheckGame(src, manifest, source.executable);
  if (!check.Ok()) {
    *error = GameCheckProblem(check, manifest);
    return false;
  }
  fs::path staging = to;
  staging += ".part";
  fs::remove_all(staging, ec);
  uint64_t done = 0;
  const uint64_t total = std::max<uint64_t>(check.bytes, 1);
  for (const GameFile& f : manifest.files) {
    if (cancel) {
      fs::remove_all(staging, ec);
      *error = "Cancelled.";
      return false;
    }
    const fs::path out = staging / PathFromUtf8(f.path);
    fs::create_directories(out.parent_path(), ec);
    std::string sha;
    const bool executable = Lower(f.path) == "default.xex";
    if (executable && !source.executable.empty()) {
      sha = CopyHashed(source.executable, out, nullptr, [&] { return cancel.load(); });
    } else if (executable && check.decrypt) {
      // The dump's encrypted default.xex (core/xex_decrypt.h): decrypted in memory, written into the copy only; the
      // player's file is only read.
      std::string data;
      if (src.Read(src.Find(f.path), &data, kMaxXexBytes) && DecryptRetailXex(&data) == XexDecrypt::kOk &&
          AtomicWrite(out, data)) {
        sha = "decrypted";
      }
      done += f.size;
      if (progress) progress(double(done) / double(total), f.path);
    } else {
      sha = src.Copy(src.Find(f.path), out, [&](uint64_t n) {
        done += n;
        if (progress) progress(double(done) / double(total), f.path);
      }, [&] { return cancel.load(); });
    }
    if (executable && !sha.empty()) sha = InspectXex(out).image_sha256;  // an executable: by its image
    if (sha != f.sha256) {
      fs::remove_all(staging, ec);
      *error = cancel ? "Cancelled." : "The game file " + f.path + " could not be copied or differs from the "
                                       "supported release.";
      return false;
    }
  }
  fs::path aside = to;
  aside += ".old";
  fs::remove_all(aside, ec);
  if (fs::exists(to, ec)) fs::rename(to, aside, ec);
  fs::rename(staging, to, ec);
  if (ec) {
    *error = "Could not store the game files in " + PathUtf8(to) + ".";
    return false;
  }
  fs::remove_all(aside, ec);
  if (progress) progress(1, "");
  return true;
}

bool PreparePortable(const PortableLayout& l, const PortableSource& source, const GameManifest& manifest,
                     const PortableProgress& progress, const std::atomic<bool>& cancel, std::string* error,
                     int jobs) {
  std::error_code ec;
  for (const fs::path& d : {l.data, l.User(), l.Saves(), l.Cache(), l.Logs()}) fs::create_directories(d, ec);
  if (!Writable(l.data)) {
    *error = "The folder " + PathUtf8(l.data) + " cannot be written.";
    return false;
  }
  const GameSource src = GameSource::Open(source.path);
  const GameCheck check = CheckGame(src, manifest, source.executable);
  if (!check.Ok()) {
    *error = GameCheckProblem(check, manifest);
    return false;
  }
  PortableState old;
  ReadPortableState(l.StateFile(), &old);

  // 1. The game files: a plain folder is read where it is; a disc image or a retail dump (whose executable is replaced
  // by the separate one or decrypted) is copied into Data/game, every file checked against the reference list.
  fs::path game_dir = source.path;
  const bool copy = src.kind() == GameSource::Kind::kIso || !source.executable.empty() || check.decrypt;
  if (copy) {
    game_dir = l.ExtractedGame();
    const bool have = old.game_dir == "data:game" && old.source == Record(l, source.path) &&
                      old.executable == Record(l, source.executable) && old.image_sha256 == UsedImage(check) &&
                      GameSource::Folder(game_dir).Find("default.xex").found;
    if (!have && !CopyGameFiles(source, manifest, game_dir, [&](double f, const std::string& d) {
          if (progress) progress(PortablePhase::kCopy, f, d);
        }, cancel, error)) {
      return false;
    }
  }
  if (progress) progress(PortablePhase::kCopy, 1, "");

  // 2. The native game, built where the runtime's libraries are (Runtime/bin, or a copy in Data/bin when the folder
  // is read only); the old program stays until the new one is complete.
  const fs::path program = l.Program();
  if (program.parent_path() != l.Runtime() / "bin") {
    fs::create_directories(program.parent_path(), ec);
    for (const auto& e : fs::recursive_directory_iterator(l.Runtime() / "bin", ec)) {
      const fs::path to = program.parent_path() / fs::relative(e.path(), l.Runtime() / "bin", ec);
      if (e.is_directory(ec)) {
        fs::create_directories(to, ec);
      } else if (e.path().filename() != program.filename()) {
        fs::copy_file(e.path(), to, fs::copy_options::update_existing, ec);
      }
    }
  }
  BuildRequest request;
  request.package = l.Runtime();
  request.xex = GameSource::Folder(game_dir).Find("default.xex").file;
  request.cache = l.BuildCache();
  request.output = program;
  request.jobs = jobs;
  const BuildProgress build_progress = [&](BuildPhase phase, double fraction, const std::string& detail) {
    if (!progress) return;
    const PortablePhase p = phase == BuildPhase::kGenerate  ? PortablePhase::kGenerate
                            : phase == BuildPhase::kCompile ? PortablePhase::kCompile
                                                            : PortablePhase::kLink;
    progress(p, fraction, detail);
  };
  if (!LocalBuild(request, build_progress, cancel, error)) return false;

  // 3. The record: only now is the new preparation the one used.
  PortableState s;
  s.source = Record(l, source.path);
  s.executable = Record(l, source.executable);
  s.game_dir = Record(l, game_dir);
  s.image_sha256 = UsedImage(check);
  s.generation = PackageGeneration(l.Runtime());
  s.program_sha256 = Sha256File(program);
  s.package_version = PackageVersion(l.Runtime());
  if (s.game_dir.empty() || !WritePortableState(l.StateFile(), s)) {
    *error = "Could not write " + PathUtf8(l.StateFile()) + ".";
    return false;
  }
  return true;
}

bool VerifyPortableGameFiles(const PortableLayout& l, const PortableState& s, const GameManifest& manifest,
                             std::vector<std::string>* bad, const std::function<void(double)>& progress,
                             const std::atomic<bool>& cancel) {
  const GameSource src = GameSource::Folder(Resolve(l, s.game_dir));
  uint64_t total = 0, done = 0;
  for (const GameFile& f : manifest.files) total += f.size;
  for (const GameFile& f : manifest.files) {
    if (cancel) return false;
    const GameSource::Entry e = src.Find(f.path);
    std::string sha;
    const bool executable = Lower(f.path) == "default.xex";  // identified by its image, not its size
    if (e.found && (executable || e.size == f.size)) {
      sha = executable ? InspectXex(e.file).image_sha256 : Sha256File(e.file);
    }
    if (sha != f.sha256) bad->push_back(f.path);
    done += f.size;
    if (progress) progress(double(done) / double(std::max<uint64_t>(total, 1)));
  }
  return true;
}

LaunchCommand PortableLaunchCommand(const PortableLayout& l, const PortableState& s,
                                    const std::vector<std::string>& extra) {
  LaunchCommand c;
  const fs::path program = l.Program();
  const std::string game = PathUtf8(Resolve(l, s.game_dir));
  c.argv = {PathUtf8(program), "--gpu_plugin=xenos", "--game_data_root=" + game,
            "--user_data_root=" + PathUtf8(l.User()), "--content_root=" + PathUtf8(l.Saves()),
            "--cache_root=" + PathUtf8(l.Cache()), "--log_file=" + PathUtf8(l.Logs() / "kuf_cod.log"),
            "--log_max_file_size_mb=64", "--log_max_files=3"};
  for (const std::string& a : extra) c.argv.push_back(a);
  // Check for Updates in the game (M17G): the updater of this folder, the file the game writes for Restart, and the
  // launcher it starts to come back (Play KUF --after-restart <pid> applies the staged update, then plays).
  {
    std::error_code ec;
#if defined(_WIN32)
    const fs::path updater = l.Runtime() / "bin" / "kuf-update.exe", launcher = l.root / "Play KUF.exe";
#else
    const fs::path updater = l.Runtime() / "bin" / "kuf-update", launcher = l.root / "Play KUF";
#endif
    if (fs::exists(l.Runtime() / "update.conf", ec) && fs::exists(updater, ec) && fs::exists(launcher, ec)) {
      c.env.push_back("KUF_UPDATER=" + PathUtf8(updater));
      c.env.push_back("KUF_RESTART_FILE=" + PathUtf8(l.data / "restart-request"));
      c.env.push_back("KUF_LAUNCHER=" + PathUtf8(launcher));
    }
  }
#if !defined(_WIN32)
  // The bundled libstdc++ only when the system's is older than the game needs (as tools/package/kuf-cod.sh).
  const fs::path compat = program.parent_path() / "compat";
  std::error_code ec;
  if (fs::exists(compat / "libstdc++.so.6", ec)) {
    std::string req;
    ReadFile(l.Runtime() / "REQUIRES", &req);
    const Requirements need = ParseRequirements(req);
    const std::string have = HostGlibcxx();
    if (!need.glibcxx.empty() && (have.empty() || CompareVersions(have, need.glibcxx) < 0)) {
      const std::string old = Env("LD_LIBRARY_PATH");
      c.env.push_back("LD_LIBRARY_PATH=" + PathUtf8(compat) + (old.empty() ? "" : ":" + old));
    }
  }
#endif
  return c;
}

}  // namespace kuf::setup
