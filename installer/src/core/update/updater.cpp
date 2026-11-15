#include "core/update/updater.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "core/platform.h"
#include "core/sha256.h"
#include "core/update/json.h"
#include "core/update/sshsig.h"
#include "core/update/zip.h"
#include "core/util.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace kuf::setup::update {
namespace {

constexpr const char* kNamespace = "kuf-cod-recomp-release";
constexpr const char* kManifestNamespace = "kuf-cod-recomp-manifest";
constexpr const char* kProduct = "kuf-cod-recomp";
constexpr uint64_t kMaxPackage = uint64_t(2) << 30;
constexpr uint64_t kMaxManifest = 64 << 10;
constexpr uint64_t kMaxSignature = 16 << 10;
constexpr size_t kMaxNotes = 2000;

#if defined(_WIN32)
constexpr const char* kLauncherName = "Play KUF.exe";
constexpr const char* kProgramName = "kuf_cod.exe";
#else
constexpr const char* kLauncherName = "Play KUF";
constexpr const char* kProgramName = "kuf_cod";
#endif

std::string Trim(std::string s) {
  while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back();
  size_t k = 0;
  while (k < s.size() && std::isspace(uint8_t(s[k]))) ++k;
  return s.substr(k);
}

std::string FirstLine(const fs::path& p) {
  std::string text;
  if (!ReadFile(p, &text)) return "";
  return Trim(text.substr(0, text.find('\n')));
}

// Release notes as plain text: control characters other than new lines dropped, at most kMaxNotes bytes.
std::string Notes(const std::string& body) {
  std::string out;
  for (size_t k = 0; k < body.size() && out.size() < kMaxNotes; ++k) {
    const char c = body[k];
    if (c == '\r') continue;
    if (c == '\n' || uint8_t(c) >= 0x20) out.push_back(c);
  }
  return Trim(out);
}

std::string GenerationId(const fs::path& runtime) {
  std::string text;
  if (!ReadFile(runtime / "builder" / "recipe.txt", &text)) return "";
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("generation_id ", 0) == 0) return Trim(line.substr(14));
  }
  return "";
}

// One updater at a time per program folder.
class Lock {
 public:
  explicit Lock(const fs::path& file) {
#if defined(_WIN32)
    handle_ = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    ok_ = handle_ != INVALID_HANDLE_VALUE;
#else
    fd_ = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    ok_ = fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0;
#endif
  }
  ~Lock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (fd_ >= 0) close(fd_);
#endif
  }
  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

class Directory : public ReleaseSource {
 public:
  explicit Directory(fs::path dir) : dir_(std::move(dir)) {}
  bool Releases(std::vector<Release>* out, std::string* why) override {
    std::string text;
    if (!ReadFile(dir_ / "releases.json", &text)) {
      *why = "no releases.json in " + PathUtf8(dir_);
      return false;
    }
    const auto json = ParseJson(text);
    if (!json || json->type != Json::kArray) {
      *why = "releases.json is not a JSON list";
      return false;
    }
    for (const Json& r : json->items) {
      Release rel;
      rel.tag = r.String("tag_name");
      rel.html_url = r.String("html_url");
      rel.published = r.String("published_at");
      rel.draft = r.Bool("draft");
      rel.prerelease = r.Bool("prerelease");
      if (const Json* assets = r.Get("assets"); assets && assets->type == Json::kArray) {
        for (const Json& a : assets->items) rel.assets.push_back({a.String("name"), "", a.Integer("size").value_or(0)});
      }
      out->push_back(std::move(rel));
    }
    return true;
  }
  bool Fetch(const Asset& a, uint64_t limit, std::string* data, std::string* why) override {
    const fs::path p = dir_ / PathFromUtf8(fs::path(a.name).filename().string());
    std::error_code ec;
    if (fs::file_size(p, ec) > limit || ec) {
      *why = a.name + ": missing or larger than " + std::to_string(limit) + " bytes";
      return false;
    }
    return ReadFile(p, data) || (*why = "cannot read " + a.name, false);
  }
  bool Download(const Asset& a, const fs::path& dest, uint64_t limit, const DownloadProgress& progress,
                const std::atomic<bool>& cancel, std::string* why) override {
    const fs::path p = dir_ / PathFromUtf8(fs::path(a.name).filename().string());
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    if (ec || size > limit) {
      *why = a.name + ": missing or larger than " + std::to_string(limit) + " bytes";
      return false;
    }
    std::ifstream in(p, std::ios::binary);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    std::vector<char> buf(1 << 16);
    uint64_t done = 0;
    while (in) {
      if (cancel) {
        *why = "cancelled";
        return false;
      }
      in.read(buf.data(), std::streamsize(buf.size()));
      const auto n = in.gcount();
      out.write(buf.data(), n);
      done += uint64_t(n);
      if (progress) progress(done, size);
    }
    return bool(out) || (*why = "cannot write " + PathUtf8(dest), false);
  }

 private:
  fs::path dir_;
};

struct Chosen {
  Version version;
  Release release;
  Asset package, package_sig, manifest, manifest_sig;
};

bool Newest(const std::vector<Release>& releases, const UpdateConfig& c, bool prerelease, Chosen* best) {
  bool found = false;
  for (const Release& r : releases) {
    if (r.draft || (r.prerelease && !prerelease)) continue;
    const std::string v = r.tag.rfind('v', 0) == 0 ? r.tag.substr(1) : r.tag;
    const auto version = ParseVersion(v);
    if (!version) continue;
    const std::string base = c.asset_prefix + "-" + v + "-" + c.platform;
    const auto asset = [&](const std::string& name) -> const Asset* {
      for (const Asset& a : r.assets) {
        if (a.name == name) return &a;
      }
      return nullptr;
    };
    const Asset *pkg = asset(base + ".zip"), *pkg_sig = asset(base + ".zip.sig"), *man = asset(base + ".update.json"),
                *man_sig = asset(base + ".update.json.sig");
    if (!pkg || !pkg_sig || !man || !man_sig) continue;
    if (found && CompareVersions(*version, best->version) <= 0) continue;
    *best = {*version, r, *pkg, *pkg_sig, *man, *man_sig};
    found = true;
  }
  return found;
}

}  // namespace

// ---- versions ------------------------------------------------------------------------------------------------------

std::optional<Version> ParseVersion(const std::string& text) {
  Version v;
  v.text = text;
  size_t at = 0;
  const auto number = [&](int* out) {
    if (at >= text.size() || !std::isdigit(uint8_t(text[at]))) return false;
    long long n = 0;
    while (at < text.size() && std::isdigit(uint8_t(text[at]))) {
      n = n * 10 + (text[at++] - '0');
      if (n > 1000000000) return false;
    }
    *out = int(n);
    return true;
  };
  if (!number(&v.major) || at >= text.size() || text[at++] != '.' || !number(&v.minor) || at >= text.size() ||
      text[at++] != '.' || !number(&v.patch)) {
    return std::nullopt;
  }
  if (at == text.size()) return v;
  if (text[at++] != '-' || at == text.size()) return std::nullopt;
  std::string part;
  for (; at <= text.size(); ++at) {
    if (at == text.size() || text[at] == '.') {
      if (part.empty()) return std::nullopt;
      v.pre.push_back(part);
      part.clear();
    } else if (std::isalnum(uint8_t(text[at])) || text[at] == '-') {
      part.push_back(text[at]);
    } else {
      return std::nullopt;
    }
  }
  return v;
}

int CompareVersions(const Version& a, const Version& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;  // a release after its pre-releases
  const auto numeric = [](const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(uint8_t(c)); });
  };
  for (size_t k = 0; k < std::min(a.pre.size(), b.pre.size()); ++k) {
    const std::string &x = a.pre[k], &y = b.pre[k];
    const bool nx = numeric(x), ny = numeric(y);
    if (nx && ny) {
      if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
      if (x != y) return x < y ? -1 : 1;
    } else if (nx != ny) {
      return nx ? -1 : 1;  // numbers before words
    } else if (x != y) {
      return x < y ? -1 : 1;
    }
  }
  if (a.pre.size() != b.pre.size()) return a.pre.size() < b.pre.size() ? -1 : 1;
  return 0;
}

// ---- the program folder --------------------------------------------------------------------------------------------

fs::path ProgramFolder::Launcher() const { return root / kLauncherName; }
std::string ProgramFolder::CurrentVersion() const { return FirstLine(Runtime() / "VERSION"); }
std::string ProgramFolder::PreviousVersion() const { return FirstLine(Previous() / "VERSION"); }
std::string ProgramFolder::PendingVersion() const { return FirstLine(Pending()); }

// The Windows packages bring their own update.conf (no channel line), so a channel written by hand does not survive
// an update: the installed version decides then.
bool FollowsPrereleases(const UpdateConfig& c, const std::string& installed, bool forced) {
  if (forced || c.channel == "prerelease") return true;
  if (!c.channel.empty()) return false;
  const auto v = ParseVersion(installed);
  return v ? !v->pre.empty() : installed.find('-') != std::string::npos;
}

bool ReadUpdateConfig(const ProgramFolder& f, UpdateConfig* c, std::string* why) {
  std::string text;
  const fs::path p = f.Runtime() / "update.conf";
  if (!ReadFile(p, &text)) {
    *why = "updates are not set up (no " + PathUtf8(p) + ")";
    return false;
  }
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    line = Trim(line);
    if (line.empty() || line[0] == '#' || line.find('=') == std::string::npos) continue;
    const std::string k = Trim(line.substr(0, line.find('='))), v = Trim(line.substr(line.find('=') + 1));
    if (k == "repo") c->repo = v;
    else if (k == "key_identity") c->key_identity = v;
    else if (k == "asset_prefix") c->asset_prefix = v;
    else if (k == "platform") c->platform = v;
    else if (k == "api") c->api = v;
    else if (k == "channel") c->channel = v;
  }
  const auto repo_ok = [](const std::string& r) {
    const size_t slash = r.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == r.size() || r.find('/', slash + 1) != std::string::npos) {
      return false;
    }
    return std::all_of(r.begin(), r.end(), [](char ch) {
      return std::isalnum(uint8_t(ch)) || ch == '_' || ch == '.' || ch == '-' || ch == '/';
    });
  };
  if (!repo_ok(c->repo)) {
    *why = PathUtf8(p) + ": repo = owner/name is not set";
    return false;
  }
  if (c->platform.empty()) {
    *why = PathUtf8(p) + ": platform is not set";
    return false;
  }
  return true;
}

std::unique_ptr<ReleaseSource> DirectorySource(const fs::path& dir) { return std::make_unique<Directory>(dir); }

#if !defined(_WIN32)
std::unique_ptr<ReleaseSource> HttpSource(const std::string&, const std::string&, std::string* why) {
  *why = "downloading releases is only built into the Windows updater (Linux: tools/updater/kuf_update.py)";
  return nullptr;
}
#endif

// ---- check and update ----------------------------------------------------------------------------------------------

namespace {

struct Session {
  const ProgramFolder& f;
  ReleaseSource& source;
  UpdateConfig conf;
  std::string signers;
  Chosen best;
  Json manifest;
  CheckResult r;
};

CheckResult Refuse(CheckResult r, Exit e, const std::string& why) {
  r.exit = e;
  r.error = why;
  return r;
}

// Steps shared by Check and Update; false with s->r filled when the answer is already known.
bool Prepare(Session* s, const UpdateOptions& o, const fs::path& tmp) {
  CheckResult& r = s->r;
  r.current = s->f.CurrentVersion();
  std::string why;
  if (!ReadUpdateConfig(s->f, &s->conf, &why)) {
    r = Refuse(r, Exit::kError, why);
    return false;
  }
  if (!ReadFile(s->f.Runtime() / "allowed_signers", &s->signers) || Trim(s->signers).empty()) {
    r = Refuse(r, Exit::kRefused, "no release key installed (Runtime/allowed_signers)");
    return false;
  }
  std::vector<Release> releases;
  if (!s->source.Releases(&releases, &why)) {
    r = Refuse(r, Exit::kError, why);
    return false;
  }
  if (!Newest(releases, s->conf, FollowsPrereleases(s->conf, r.current, o.prerelease), &s->best)) {
    r.exit = Exit::kUpToDate;
    r.error = "no signed release found";
    return false;
  }
  const std::string version = s->best.version.text;
  // The update manifest: verified before it is read.
  std::string text, sig;
  if (!s->source.Fetch(s->best.manifest, kMaxManifest, &text, &why) ||
      !s->source.Fetch(s->best.manifest_sig, kMaxSignature, &sig, &why)) {
    r = Refuse(r, Exit::kError, why);
    return false;
  }
  if (!VerifySshSignature(s->signers, s->conf.key_identity, kManifestNamespace, text, sig, &why)) {
    r = Refuse(r, Exit::kRefused, "update manifest of " + version + ": " + why);
    return false;
  }
  const auto m = ParseJson(text);
  std::vector<std::string> problems;
  const Json* pkg = m ? m->Get("package") : nullptr;
  if (!m || m->type != Json::kObject || !pkg || pkg->type != Json::kObject) {
    r = Refuse(r, Exit::kRefused, "update manifest of " + version + ": not a manifest");
    return false;
  }
  if (!m->Get("schema") || m->Get("schema")->text != "1") problems.push_back("schema (this updater reads 1)");
  if (m->String("product") != kProduct) problems.push_back("product");
  if (m->String("version") != version) problems.push_back("version is not the release's " + version);
  if (m->String("platform") != s->conf.platform) problems.push_back("platform is not " + s->conf.platform);
  if (pkg->String("name") != s->best.package.name) problems.push_back("package is not " + s->best.package.name);
  const auto size = pkg->Integer("size");
  if (!size || *size == 0 || *size > kMaxPackage) problems.push_back("package size");
  const std::string sha = pkg->String("sha256");
  if (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](char c) {
        return std::isdigit(uint8_t(c)) || (c >= 'a' && c <= 'f');
      })) {
    problems.push_back("package sha256");
  }
  if (m->Get("summary") && (m->Get("summary")->type != Json::kString || m->String("summary").size() > kMaxNotes)) {
    problems.push_back("summary");
  }
  if (const Json* req = m->Get("requires"); req && req->type == Json::kObject && !req->fields.empty()) {
    r = Refuse(r, Exit::kRefused, version + " has system requirements this updater cannot check: install it with "
                                            "the setup instead");
    return false;
  }
  if (!problems.empty()) {
    std::string all;
    for (const auto& p : problems) all += (all.empty() ? "" : "; ") + p;
    r = Refuse(r, Exit::kRefused, "update manifest of " + version + ": " + all);
    return false;
  }
  s->manifest = *m;
  r.latest = version;
  r.size = *size;
  r.published = m->String("published").substr(0, 10);
  r.notes = Notes(m->String("notes"));
  r.summary = Notes(m->Get("summary") ? m->String("summary") : m->String("notes"));
  const std::string page = "https://github.com/" + s->conf.repo + "/releases/tag/" + s->best.release.tag;
  r.page = s->best.release.html_url == page ? page : "";
  const auto current = ParseVersion(r.current);
  r.update = !current || CompareVersions(s->best.version, *current) > 0;
  if (r.update && current && m->Get("min_installed")) {
    const auto need = ParseVersion(m->String("min_installed"));
    if (!need) {
      r = Refuse(r, Exit::kRefused, "update manifest of " + version + ": min_installed");
      return false;
    }
    if (CompareVersions(*current, *need) < 0) {
      r = Refuse(r, Exit::kRefused, version + " cannot update " + r.current + " (it needs " + need->text +
                                        " or later): install it with the setup instead");
      return false;
    }
  }
  (void)tmp;
  return true;
}

}  // namespace

CheckResult Check(const ProgramFolder& f, ReleaseSource& source, const UpdateOptions& o) {
  Session s{f, source};
  if (!Prepare(&s, o, {})) return s.r;
  s.r.exit = s.r.update ? Exit::kDone : Exit::kUpToDate;
  return s.r;
}

CheckResult Update(const ProgramFolder& f, ReleaseSource& source, const UpdateOptions& o,
                   const DownloadProgress& progress, const std::function<void(const std::string&)>& stage,
                   const std::atomic<bool>& cancel) {
  Session s{f, source};
  CheckResult& r = s.r;
  std::error_code ec;
  Lock lock(f.root / ".update.lock");
  if (!lock.ok()) return Refuse(r, Exit::kError, "another updater is running");
  // Leftovers of an interrupted update (we hold the lock).
  for (const auto& e : fs::directory_iterator(f.root, ec)) {
    const std::string n = PathUtf8(e.path().filename());
    if (n.rfind(".update-download-", 0) == 0 || n.rfind(".update-staging-", 0) == 0) fs::remove_all(e.path(), ec);
  }
  if (!Prepare(&s, o, {})) return r;
  if (!r.update && !o.allow_downgrade) {
    r.exit = Exit::kUpToDate;
    return r;
  }
  const std::string version = s.best.version.text;
  const fs::path tmp = f.root / (".update-download-" + std::to_string(uint64_t(std::hash<std::string>{}(version))));
  const fs::path staging = f.root / ".update-staging";
  fs::remove_all(tmp, ec);
  fs::remove_all(staging, ec);
  fs::create_directories(tmp, ec);
  const auto cleanup = [&] {
    fs::remove_all(tmp, ec);
    fs::remove_all(staging, ec);
  };
  std::string why, sig;
  const fs::path package = tmp / PathFromUtf8(s.best.package.name);
  const Json& pkg = *s.manifest.Get("package");
  const uint64_t size = r.size;
  if (!source.Fetch(s.best.package_sig, kMaxSignature, &sig, &why) ||
      !source.Download(s.best.package, package, size,
                       [&](uint64_t done, uint64_t total) {
                         if (progress) progress(std::min(done, size), std::max(total, size));
                       },
                       cancel, &why)) {
    cleanup();
    // Larger than the signed manifest says: a verification failure, as the Linux updater reports it.
    const bool too_large = why.find("larger than") != std::string::npos;
    return Refuse(r, cancel ? Exit::kCancelled : too_large ? Exit::kRefused : Exit::kError,
                  too_large ? s.best.package.name + ": larger than the manifest's " + std::to_string(size) + " bytes"
                            : why);
  }
  if (stage) stage("verifying");
  if (fs::file_size(package, ec) != size || Sha256File(package) != pkg.String("sha256")) {
    cleanup();
    return Refuse(r, Exit::kRefused, s.best.package.name + ": not the manifest's size and SHA-256");
  }
  if (!VerifySshSignatureFile(s.signers, s.conf.key_identity, kNamespace, package, sig, &why)) {
    cleanup();
    return Refuse(r, Exit::kRefused, s.best.package.name + ": " + why);
  }
  if (cancel) {
    cleanup();
    return Refuse(r, Exit::kCancelled, "cancelled");
  }
  if (stage) stage("installing");
  // The package: one top folder with Play KUF and Runtime/; only those two are taken.
  std::vector<ZipEntry> entries;
  if (!ReadZipDirectory(package, &entries, &why)) {
    cleanup();
    return Refuse(r, Exit::kRefused, s.best.package.name + ": " + why);
  }
  std::string top;
  for (const ZipEntry& e : entries) {
    const std::string first = e.name.substr(0, e.name.find('/'));
    if (top.empty()) top = first;
    if (first != top || e.name.find('/') == std::string::npos) {
      cleanup();
      return Refuse(r, Exit::kRefused, s.best.package.name + ": not one top folder");
    }
  }
  const std::string launcher = std::string(kLauncherName);
  if (!ExtractZip(package, entries,
                  [&](const std::string& n) -> std::string {
                    const std::string rel = n.substr(top.size() + 1);
                    if (rel == launcher || rel.rfind("Runtime/", 0) == 0) return rel;
                    return "";
                  },
                  staging, &why)) {
    cleanup();
    return Refuse(r, Exit::kRefused, s.best.package.name + ": " + why);
  }
  if (FirstLine(staging / "Runtime" / "VERSION") != version || !fs::exists(staging / launcher, ec)) {
    cleanup();
    return Refuse(r, Exit::kRefused, s.best.package.name + ": its Runtime/VERSION is not " + version +
                                         " or Play KUF is missing");
  }
  // The prepared game carried over when its inputs did not change (no recompiling); otherwise it is prepared again.
  const fs::path program = f.Runtime() / "bin" / kProgramName;
  if (!GenerationId(f.Runtime()).empty() && GenerationId(f.Runtime()) == GenerationId(staging / "Runtime") &&
      fs::exists(program, ec)) {
    fs::copy_file(program, staging / "Runtime" / "bin" / kProgramName, fs::copy_options::overwrite_existing, ec);
  }
#if !defined(_WIN32)
  fs::permissions(staging / launcher, fs::perms(0755), ec);
#endif
  // Staged for the next start (the running version is not touched).
  fs::remove_all(f.Next(), ec);
  fs::remove(f.root / (launcher + ".next"), ec);
  fs::rename(staging / "Runtime", f.Next(), ec);
  if (!ec) fs::rename(staging / launcher, f.root / (launcher + ".next"), ec);
  if (ec || !AtomicWrite(f.Pending(), version + "\n")) {
    fs::remove_all(f.Next(), ec);
    cleanup();
    return Refuse(r, Exit::kError, "cannot write into " + PathUtf8(f.root));
  }
  cleanup();
  r.exit = Exit::kDone;
  return r;
}

// ---- apply and roll back -------------------------------------------------------------------------------------------

namespace {

// Renames done so far, undone in reverse if a later one fails.
struct Renames {
  std::vector<std::pair<fs::path, fs::path>> done;
  bool Do(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return true;
    fs::rename(from, to, ec);
    if (ec) return false;
    done.emplace_back(from, to);
    return true;
  }
  void Undo() {
    std::error_code ec;
    for (auto it = done.rbegin(); it != done.rend(); ++it) fs::rename(it->second, it->first, ec);
    done.clear();
  }
};

}  // namespace

bool ApplyStagedUpdate(const ProgramFolder& f, std::string* applied, std::string* why) {
  applied->clear();
  std::error_code ec;
  const std::string version = f.PendingVersion();
  if (version.empty()) return true;  // nothing staged
  const fs::path launcher = f.Launcher(), launcher_next = f.root / (std::string(kLauncherName) + ".next"),
                 launcher_prev = f.root / (std::string(kLauncherName) + ".previous");
  if (FirstLine(f.Next() / "VERSION") != version) {  // a staging that never completed
    fs::remove_all(f.Next(), ec);
    fs::remove(launcher_next, ec);
    fs::remove(f.Pending(), ec);
    *why = "an incomplete update was removed";
    return false;
  }
  fs::remove_all(f.Previous(), ec);
  fs::remove(launcher_prev, ec);
  Renames r;
  // A running launcher can be renamed on Windows (not deleted): this one keeps running as the .previous file.
  if (!r.Do(f.Runtime(), f.Previous()) || !r.Do(f.Next(), f.Runtime()) || !r.Do(launcher, launcher_prev) ||
      !r.Do(launcher_next, launcher)) {
    r.Undo();
    *why = "the update could not be put in place (is the game still running?); it stays ready for the next start";
    return false;
  }
  fs::remove(f.Pending(), ec);
  *applied = version;
  return true;
}

bool Rollback(const ProgramFolder& f, std::string* now, std::string* why) {
  std::error_code ec;
  const std::string previous = f.PreviousVersion();
  if (previous.empty()) {
    *why = "no previous version to go back to";
    return false;
  }
  const fs::path swap = f.root / "Runtime.swap";
  const fs::path launcher = f.Launcher(), launcher_prev = f.root / (std::string(kLauncherName) + ".previous"),
                 launcher_swap = f.root / (std::string(kLauncherName) + ".swap");
  fs::remove_all(swap, ec);
  fs::remove(launcher_swap, ec);
  Renames r;
  const bool has_launcher_prev = fs::exists(launcher_prev, ec);
  if (!r.Do(f.Runtime(), swap) || !r.Do(f.Previous(), f.Runtime()) || !r.Do(swap, f.Previous()) ||
      (has_launcher_prev && (!r.Do(launcher, launcher_swap) || !r.Do(launcher_prev, launcher) ||
                             !r.Do(launcher_swap, launcher_prev)))) {
    r.Undo();
    *why = "the previous version could not be put back (is the game running?)";
    return false;
  }
  fs::remove(f.Pending(), ec);  // a staged update of the newer line is not wanted any more
  fs::remove_all(f.Next(), ec);
  *now = previous;
  return true;
}

}  // namespace kuf::setup::update
