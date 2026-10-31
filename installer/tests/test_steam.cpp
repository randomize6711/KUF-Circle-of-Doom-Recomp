// Steam integration on synthetic Steam folders (installer/src/core/steam.cpp, vdf.cpp): never the real Steam.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/steam.h"
#include "core/vdf.h"
#include "sandbox.h"

using namespace kuf::setup;
using kuf::setup::test::Sandbox;

namespace {

bool g_running = false;
bool Running() { return g_running; }

// A shortcuts.vdf as Steam writes one, with another game's entry (a uint64 value among them: kept byte for byte).
std::string OtherGames() {
  vdf::Node root;
  vdf::Node list = vdf::Node::Map("shortcuts");
  vdf::Node e = vdf::Node::Map("0");
  e.children = {vdf::Node::Int("appid", int32_t(0x81234567)), vdf::Node::String("AppName", "Other Game"),
                vdf::Node::String("Exe", "\"/opt/other/run\""), vdf::Node::String("StartDir", "\"/opt/other\"")};
  vdf::Node big;
  big.type = vdf::kUInt64;
  big.key = "LastPlayTime64";
  big.raw = {1, 2, 3, 4, 5, 6, 7, 8};
  e.children.push_back(big);
  vdf::Node tags = vdf::Node::Map("tags");
  tags.children.push_back(vdf::Node::String("0", "favorite"));
  e.children.push_back(tags);
  list.children.push_back(e);
  vdf::Node e2 = e;
  e2.key = "1";
  e2.Child("AppName")->str = "Third Game";
  e2.Child("Exe")->str = "\"/opt/third/run\"";
  list.children.push_back(e2);
  root.children.push_back(list);
  return vdf::WriteBinary(root);
}

struct Steam {
  Sandbox s;
  steam::Profile profile;
  Steam() {
    g_running = false;
    steam::SetRunningCheckForTests(Running);
    const fs::path root = s.Home() / ".local/share/Steam";
    fs::create_directories(root / "userdata/0");
    fs::create_directories(root / "userdata/1001/config");
    fs::create_directories(root / "userdata/2002/config");
    Sandbox::Write(root / "config/loginusers.vdf",
                   "\"users\"\n{\n\t\"76561197960266729\"\n\t{\n\t\t\"AccountName\"\t\t\"alpha\"\n\t\t\"PersonaName\"\t\t"
                   "\"Alpha One\"\n\t\t\"MostRecent\"\t\t\"1\"\n\t}\n\t\"76561197960267730\"\n\t{\n\t\t\"AccountName\"\t"
                   "\"beta\"\n\t}\n}\n");
    fs::create_directories(s.Home() / ".steam");
    fs::create_directory_symlink(root, s.Home() / ".steam/steam");  // the usual second name: found once
    profile = steam::FindProfiles(steam::FindInstalls(s.Home()).at(0)).at(0);
  }
  ~Steam() { steam::SetRunningCheckForTests(nullptr); }
};

const vdf::Node* List(const std::optional<vdf::Node>& root) { return root ? root->ChildNoCase("shortcuts") : nullptr; }

}  // namespace

TEST_CASE("binary VDF: read and written back byte for byte; unknown types refused", "[steam]") {
  const std::string data = OtherGames();
  std::string error;
  const auto root = vdf::ParseBinary(data, &error);
  REQUIRE(root);
  CHECK(vdf::WriteBinary(*root) == data);
  CHECK(List(root)->children.size() == 2);
  CHECK_FALSE(vdf::ParseBinary(data.substr(0, data.size() - 3), &error));  // cut short
  std::string wide = std::string(1, '\0') + "shortcuts" + '\0' + char(0x05) + "w" + '\0';
  CHECK_FALSE(vdf::ParseBinary(wide, &error));
  CHECK(error.find("unknown value type") != std::string::npos);
}

TEST_CASE("Steam folders and profiles: symlinked names found once, names from loginusers.vdf", "[steam]") {
  Steam st;
  const auto installs = steam::FindInstalls(st.s.Home());
  REQUIRE(installs.size() == 1);
  const auto profiles = steam::FindProfiles(installs[0]);
  REQUIRE(profiles.size() == 2);  // 0 is not a profile
  CHECK(profiles[0].account_id == "1001");
  CHECK(profiles[0].name == "Alpha One");
  CHECK(profiles[0].most_recent);
  CHECK(profiles[1].name == "beta");
}

TEST_CASE("the shortcut is added, updated in place, removed; other games untouched", "[steam]") {
  Steam st;
  const fs::path file = st.profile.ShortcutsFile();
  Sandbox::Write(file, OtherGames());
  const steam::Shortcut sc{"Kingdom Under Fire: Circle of Doom", st.s.dir / "Games/KUF/kuf-cod", st.s.dir / "Games/KUF",
                           st.s.dir / "Games/KUF/share/kuf-cod-recomp.png"};
  std::string message;
  REQUIRE(steam::AddShortcut(st.profile, sc, &message));
  CHECK(steam::HasShortcut(st.profile, sc.exe));
  std::string data, error;
  REQUIRE(ReadFile(file, &data));
  auto root = vdf::ParseBinary(data, &error);
  REQUIRE(List(root)->children.size() == 3);
  const vdf::Node& ours = List(root)->children[2];
  CHECK(ours.key == "2");
  CHECK(ours.ChildNoCase("Exe")->str == "\"" + sc.exe.string() + "\"");
  CHECK(uint32_t(ours.ChildNoCase("appid")->Int32()) == steam::ShortcutAppId("\"" + sc.exe.string() + "\"", sc.name));
  // The other games' entries are exactly as they were (their uint64 value too).
  auto before = vdf::ParseBinary(OtherGames(), &error);
  CHECK(vdf::WriteBinary(List(root)->children[0]) == vdf::WriteBinary(List(before)->children[0]));
  // A backup of the previous file was made.
  bool backup = false;
  for (const auto& e : fs::directory_iterator(file.parent_path())) {
    backup |= e.path().filename().string().find("shortcuts.vdf.kuf-setup-backup-") == 0;
  }
  CHECK(backup);
  // Again (a reinstall): updated, not duplicated.
  REQUIRE(steam::AddShortcut(st.profile, sc, &message));
  CHECK(message.find("updated") != std::string::npos);
  REQUIRE(ReadFile(file, &data));
  CHECK(List(vdf::ParseBinary(data, &error))->children.size() == 3);
  // Removed: the others stay, renumbered 0, 1.
  REQUIRE(steam::RemoveShortcut(st.profile, sc.exe, &message));
  REQUIRE(ReadFile(file, &data));
  root = vdf::ParseBinary(data, &error);
  REQUIRE(List(root)->children.size() == 2);
  CHECK(List(root)->children[1].key == "1");
  CHECK(List(root)->children[1].ChildNoCase("AppName")->str == "Third Game");
  CHECK_FALSE(steam::HasShortcut(st.profile, sc.exe));
}

TEST_CASE("Steam running, a missing file, an unknown format", "[steam]") {
  Steam st;
  const steam::Shortcut sc{"Kingdom Under Fire: Circle of Doom", "/g/kuf-cod", "/g", "/g/icon.png"};
  std::string message;
  g_running = true;
  CHECK_FALSE(steam::AddShortcut(st.profile, sc, &message));
  CHECK(message.find("close Steam") != std::string::npos);
  CHECK_FALSE(fs::exists(st.profile.ShortcutsFile()));
  g_running = false;
  REQUIRE(steam::AddShortcut(st.profile, sc, &message));  // no file yet: a new one
  CHECK(steam::HasShortcut(st.profile, "/g/kuf-cod"));
  Sandbox::Write(st.profile.ShortcutsFile(), std::string(1, '\0') + "shortcuts" + '\0' + char(0x05) + "w" + '\0');
  const std::string odd = std::string(1, '\0') + "shortcuts" + '\0' + char(0x05) + "w" + '\0';
  CHECK_FALSE(steam::AddShortcut(st.profile, sc, &message));
  std::string after;
  REQUIRE(ReadFile(st.profile.ShortcutsFile(), &after));
  CHECK(after == odd);  // left exactly as it was
}
