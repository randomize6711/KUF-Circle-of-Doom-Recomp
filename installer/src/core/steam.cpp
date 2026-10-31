#include "core/steam.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>

#include <unistd.h>

#include "core/vdf.h"

namespace kuf::setup::steam {

namespace {

constexpr uint64_t kSteamId64Base = 76561197960265728ull;

std::string Quoted(const fs::path& p) { return "\"" + p.string() + "\""; }

std::string Unquoted(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

// The entry's executable (the "Exe" / "exe" key, quotes removed).
std::string ExeOf(const vdf::Node& entry) {
  const vdf::Node* exe = entry.ChildNoCase("Exe");
  return exe && exe->type == vdf::kString ? Unquoted(exe->str) : std::string();
}

bool Tagged(const vdf::Node& entry) {
  const vdf::Node* tags = entry.ChildNoCase("tags");
  if (!tags || tags->type != vdf::kMap) return false;
  for (const vdf::Node& t : tags->children) {
    if (t.type == vdf::kString && t.str == kTag) return true;
  }
  return false;
}

bool Ours(const vdf::Node& entry, const fs::path& exe) { return Tagged(entry) || ExeOf(entry) == exe.string(); }

// The file's tree ("shortcuts" map at the top), or a new one if the file does not exist.
bool Load(const fs::path& file, vdf::Node* root, bool* existed, std::string* message) {
  std::string data;
  std::error_code ec;
  *existed = fs::exists(file, ec);
  if (!*existed) {
    *root = vdf::Node{};
    root->children.push_back(vdf::Node::Map("shortcuts"));
    return true;
  }
  if (!ReadFile(file, &data)) {
    *message = "Steam's shortcuts file could not be read.";
    return false;
  }
  std::string error;
  auto parsed = vdf::ParseBinary(data, &error);
  if (!parsed || !parsed->ChildNoCase("shortcuts")) {
    *message = "Steam's shortcuts file has a format this setup does not know (" +
               (error.empty() ? std::string("no shortcuts list") : error) + "); it was not changed.";
    return false;
  }
  *root = std::move(*parsed);
  return true;
}

vdf::Node* ShortcutsMap(vdf::Node* root) {
  for (vdf::Node& c : root->children) {
    if (c.type == vdf::kMap && (c.key == "shortcuts" || c.key == "Shortcuts")) return &c;
  }
  return nullptr;
}

// Backs up, writes and verifies (the backup is put back if the written file does not read back as `root`).
bool Store(const fs::path& file, const vdf::Node& root, bool existed, std::string* message) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path backup;
  if (existed) {
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
    backup = file.string() + ".kuf-setup-backup-" + std::to_string(stamp);
    fs::copy_file(file, backup, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      *message = "Steam's shortcuts file could not be backed up; it was not changed.";
      return false;
    }
  }
  const std::string data = vdf::WriteBinary(root);
  if (!AtomicWrite(file, data)) {
    *message = "Steam's shortcuts file could not be written; it was not changed.";
    return false;
  }
  std::string back;
  std::string error;
  if (!ReadFile(file, &back) || back != data || !vdf::ParseBinary(back, &error)) {
    if (!backup.empty()) fs::copy_file(backup, file, fs::copy_options::overwrite_existing, ec);
    *message = "Steam's shortcuts file did not read back as written; the previous one was put back.";
    return false;
  }
  return true;
}

uint32_t Crc32(std::string_view data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char c : data) {
    crc ^= c;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

}  // namespace

std::vector<Install> FindInstalls(const fs::path& home) {
  const std::pair<fs::path, bool> candidates[] = {
      {home / ".local/share/Steam", false},
      {home / ".steam/steam", false},
      {home / ".steam/root", false},
      {home / ".var/app/com.valvesoftware.Steam/.local/share/Steam", true},
      {home / ".var/app/com.valvesoftware.Steam/data/Steam", true},
  };
  std::vector<Install> out;
  std::vector<fs::path> seen;
  for (const auto& [path, flatpak] : candidates) {
    std::error_code ec;
    if (!fs::is_directory(path / "userdata", ec)) continue;
    const fs::path real = fs::canonical(path, ec);
    if (ec || std::find(seen.begin(), seen.end(), real) != seen.end()) continue;
    seen.push_back(real);
    out.push_back({real, flatpak});
  }
  return out;
}

std::vector<Profile> FindProfiles(const Install& install) {
  // Names from loginusers.vdf: "users" { "<steamid64>" { "AccountName" "...", "PersonaName" "...", "MostRecent" "1" } }
  std::vector<std::pair<std::string, std::pair<std::string, bool>>> names;
  std::string text;
  if (ReadFile(install.root / "config" / "loginusers.vdf", &text)) {
    if (auto root = vdf::ParseText(text)) {
      if (const vdf::Node* users = root->ChildNoCase("users")) {
        for (const vdf::Node& u : users->children) {
          uint64_t id64 = 0;
          try {
            id64 = std::stoull(u.key);
          } catch (...) {
            continue;
          }
          if (id64 < kSteamId64Base) continue;
          const vdf::Node* persona = u.ChildNoCase("PersonaName");
          const vdf::Node* account = u.ChildNoCase("AccountName");
          const vdf::Node* recent = u.ChildNoCase("MostRecent");
          std::string name = persona && !persona->str.empty() ? persona->str : account ? account->str : "";
          names.push_back({std::to_string(id64 - kSteamId64Base), {name, recent && recent->str == "1"}});
        }
      }
    }
  }
  std::vector<Profile> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(install.root / "userdata", ec)) {
    const std::string id = e.path().filename().string();
    if (!e.is_directory(ec) || id.empty() || id == "0" ||
        !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      continue;
    }
    Profile p{install, id, id, false};
    for (const auto& [account, info] : names) {
      if (account == id) {
        if (!info.first.empty()) p.name = info.first;
        p.most_recent = info.second;
      }
    }
    out.push_back(std::move(p));
  }
  std::sort(out.begin(), out.end(), [](const Profile& a, const Profile& b) { return a.account_id < b.account_id; });
  return out;
}

namespace {
bool (*g_running_check)() = nullptr;
}  // namespace

void SetRunningCheckForTests(bool (*check)()) { g_running_check = check; }

bool SteamRunning() {
  if (g_running_check) return g_running_check();
  const uid_t uid = getuid();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator("/proc", ec)) {
    const std::string pid = e.path().filename().string();
    if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
    std::string comm, status;
    if (!ReadFile(e.path() / "comm", &comm)) continue;
    while (!comm.empty() && (comm.back() == '\n')) comm.pop_back();
    if (comm != "steam" && comm != "steamwebhelper") continue;
    if (ReadFile(e.path() / "status", &status)) {
      const size_t at = status.find("\nUid:\t");
      if (at != std::string::npos && std::strtoul(status.c_str() + at + 6, nullptr, 10) != uid) continue;
    }
    return true;
  }
  return false;
}

uint32_t ShortcutAppId(std::string_view exe_quoted, std::string_view name) {
  return Crc32(std::string(exe_quoted) + std::string(name)) | 0x80000000u;
}

bool AddShortcut(const Profile& profile, const Shortcut& s, std::string* message) {
  if (SteamRunning()) {
    *message = "Steam is running: close Steam, then try again.";
    return false;
  }
  const fs::path file = profile.ShortcutsFile();
  vdf::Node root;
  bool existed = false;
  if (!Load(file, &root, &existed, message)) return false;
  vdf::Node* list = ShortcutsMap(&root);
  // The entry, as Steam writes one (its keys in Steam's order and spelling).
  const std::string exe_q = Quoted(s.exe);
  vdf::Node entry = vdf::Node::Map("");
  entry.children = {
      vdf::Node::Int("appid", int32_t(ShortcutAppId(exe_q, s.name))),
      vdf::Node::String("AppName", s.name),
      vdf::Node::String("Exe", exe_q),
      vdf::Node::String("StartDir", Quoted(s.start_dir)),
      vdf::Node::String("icon", s.icon.string()),
      vdf::Node::String("ShortcutPath", ""),
      vdf::Node::String("LaunchOptions", ""),
      vdf::Node::Int("IsHidden", 0),
      vdf::Node::Int("AllowDesktopConfig", 1),
      vdf::Node::Int("AllowOverlay", 1),
      vdf::Node::Int("OpenVR", 0),
      vdf::Node::Int("Devkit", 0),
      vdf::Node::String("DevkitGameID", ""),
      vdf::Node::Int("DevkitOverrideAppID", 0),
      vdf::Node::Int("LastPlayTime", 0),
      vdf::Node::String("FlatpakAppID", ""),
  };
  vdf::Node tags = vdf::Node::Map("tags");
  tags.children.push_back(vdf::Node::String("0", kTag));
  entry.children.push_back(std::move(tags));
  // A previous entry of the setup's is replaced in place (its position and index kept); otherwise added last.
  bool replaced = false;
  for (vdf::Node& e : list->children) {
    if (e.type == vdf::kMap && Ours(e, s.exe)) {
      entry.key = e.key;
      e = entry;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    int next = 0;
    for (const vdf::Node& e : list->children) {
      try {
        next = std::max(next, std::stoi(e.key) + 1);
      } catch (...) {
      }
    }
    entry.key = std::to_string(next);
    list->children.push_back(std::move(entry));
  }
  if (!Store(file, root, existed, message)) return false;
  if (!HasShortcut(profile, s.exe)) {
    *message = "The Steam shortcut could not be verified after writing it.";
    return false;
  }
  *message = replaced ? "The Steam shortcut was updated." : "The Steam shortcut was added.";
  return true;
}

bool RemoveShortcut(const Profile& profile, const fs::path& exe, std::string* message) {
  if (SteamRunning()) {
    *message = "Steam is running: close Steam, then try again.";
    return false;
  }
  const fs::path file = profile.ShortcutsFile();
  std::error_code ec;
  if (!fs::exists(file, ec)) {
    *message = "No Steam shortcuts file: nothing to remove.";
    return true;
  }
  vdf::Node root;
  bool existed = false;
  if (!Load(file, &root, &existed, message)) return false;
  vdf::Node* list = ShortcutsMap(&root);
  const size_t before = list->children.size();
  std::erase_if(list->children, [&](const vdf::Node& e) { return e.type == vdf::kMap && Ours(e, exe); });
  if (list->children.size() == before) {
    *message = "The Steam shortcut was not there (nothing to remove).";
    return true;
  }
  int index = 0;  // Steam's entries are numbered 0, 1, 2 ... in order
  for (vdf::Node& e : list->children) e.key = std::to_string(index++);
  if (!Store(file, root, existed, message)) return false;
  if (HasShortcut(profile, exe)) {
    *message = "The Steam shortcut is still there after removing it.";
    return false;
  }
  *message = "The Steam shortcut was removed.";
  return true;
}

bool HasShortcut(const Profile& profile, const fs::path& exe) {
  std::string data, error;
  if (!ReadFile(profile.ShortcutsFile(), &data)) return false;
  auto root = vdf::ParseBinary(data, &error);
  if (!root) return false;
  vdf::Node* list = ShortcutsMap(&*root);
  if (!list) return false;
  for (const vdf::Node& e : list->children) {
    if (e.type == vdf::kMap && Tagged(e) && ExeOf(e) == exe.string()) return true;
  }
  return false;
}

}  // namespace kuf::setup::steam
