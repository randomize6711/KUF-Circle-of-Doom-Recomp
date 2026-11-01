// The multiplayer menus' model (src/coop/lobby_menu.cpp) over real loopback lobbies (no game, no Steam).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <random>
#include <thread>

#include "coop/coop_addr.h"
#include "coop/lobby_menu.h"
#include "net/steam_lobby.h"

using namespace kuf::coop;
using namespace std::chrono_literals;

namespace {

ServiceConfig Local(uint16_t discovery = 0) {
  ServiceConfig c;
  c.lobby_port = 0;
  c.discovery_port = discovery;
  c.bind_address = "127.0.0.1";
  c.code_address = *ParseIpv4("127.0.0.1");
  return c;
}

template <typename F>
bool Until(std::initializer_list<LobbyMenu*> menus, F done, std::chrono::milliseconds limit = 5000ms) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    for (LobbyMenu* m : menus) m->Tick();
    if (done()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

size_t RowOf(const MenuView& v, const std::string& label) {
  for (size_t i = 0; i < v.rows.size(); ++i) {
    if (v.rows[i].label == label) return i;
  }
  return size_t(-1);
}

MenuRow Row(const LobbyMenu& m, const std::string& label) {
  const MenuView v = m.View();
  const size_t i = RowOf(v, label);
  return i < v.rows.size() ? v.rows[i] : MenuRow{};
}

using kuf::input::TextEvent;
using kuf::input::TextKey;
// What the input driver's text entry produces (input/text_entry.cpp): typed text and editing keys, in order.
void TypeText(LobbyMenu& m, const std::string& s) { m.Text({TextEvent{false, TextKey::kEnter, s}}); }
void Press(LobbyMenu& m, TextKey k) { m.Text({TextEvent{true, k, {}}}); }

}  // namespace

TEST_CASE("lobby menus: Home, Back and the solo game", "[net][lobby][menu]") {
  LobbyService service(Local());
  LobbyMenu menu(service, "Alice");
  menu.Open();
  MenuView v = menu.View();
  CHECK(v.page == MenuPage::kHome);
  REQUIRE(v.rows.size() == 5);
  CHECK(v.rows[0].label == "Continue Solo Game");
  CHECK(v.rows[4].label == "Back");
  CHECK(menu.Input(MenuInput::kSelect).action == Action::kSolo);
  CHECK(menu.Input(MenuInput::kBack).action == Action::kLeave);
  menu.Input(MenuInput::kUp);  // at the top: stays
  CHECK(menu.View().selected == 0);
  menu.Input(MenuInput::kDown);
  menu.Input(MenuInput::kSelect);
  CHECK(menu.Page() == MenuPage::kCreate);
  menu.Input(MenuInput::kBack);
  CHECK(menu.Page() == MenuPage::kHome);
  CHECK(menu.View().selected == 1);  // back on the row the page was opened from
}

TEST_CASE("lobby menus: Create's fields, text entry, limits; Friends Only only with Steam", "[net][lobby][menu]") {
  LobbyService service(Local());
  LobbyMenu menu(service, "Alice");
  menu.Open();
  menu.Select(1);  // Create Lobby
  CHECK(Row(menu, "Lobby name:").value == "Alice's lobby");
  menu.SetGameDifficulty(2, "Hard");  // the game's own, shown in the title, not a setting here
  CHECK(menu.View().title.find("Difficulty: Hard") != std::string::npos);
  CHECK(menu.View().rows.size() == 6);  // no Steam: no Invitations row; it fits the screen's six rows
  // Select edits the name with all of it selected: typing replaces it; Esc cancels (the old name stays).
  menu.Input(MenuInput::kSelect);
  REQUIRE(menu.Editing());
  TypeText(menu, "Dungeon");
  CHECK(Row(menu, "Lobby name:").value.find("Dungeon") == 0);  // shown at once (with the caret)
  Press(menu, TextKey::kEscape);
  CHECK_FALSE(menu.Editing());
  CHECK(Row(menu, "Lobby name:").value == "Alice's lobby");
  menu.Input(MenuInput::kSelect);  // again: End, type, Backspace, Enter
  Press(menu, TextKey::kEnd);
  TypeText(menu, "!!");
  Press(menu, TextKey::kBackspace);
  Press(menu, TextKey::kEnter);
  CHECK_FALSE(menu.Editing());
  CHECK(Row(menu, "Lobby name:").value == "Alice's lobby!");
  // Max Players 2-4.
  menu.Select(RowOf(menu.View(), "Max players:"));  // Select steps a choice forward
  for (int i = 0; i < 5; ++i) menu.Input(MenuInput::kRight);
  CHECK(Row(menu, "Max players:").value == "4");
  for (int i = 0; i < 5; ++i) menu.Input(MenuInput::kLeft);
  CHECK(Row(menu, "Max players:").value == "2");
  // Visibility without Steam: Public and Private only.
  menu.Select(RowOf(menu.View(), "Visibility:"));
  CHECK(Row(menu, "Visibility:").value == "Private (invite code)");
  menu.Input(MenuInput::kRight);
  CHECK(Row(menu, "Visibility:").value == "Public");
  // Invitations: Steam is not configured: no row (invite codes only).
  CHECK(RowOf(menu.View(), "Invitations:") == size_t(-1));
  // The host's password: clicked, typed, shown as typed (here only), confirmed, cleared.
  menu.Select(RowOf(menu.View(), "Password:"));
  REQUIRE(menu.Editing());
  CHECK(Row(menu, "Password:").value == "|");  // empty: the caret alone
  TypeText(menu, "MyFriends123");
  CHECK(Row(menu, "Password:").value == "MyFriends123|");
  Press(menu, TextKey::kEnter);
  CHECK(Row(menu, "Password:").value == "MyFriends123");
  CHECK(menu.Text({TextEvent{true, TextKey::kCopy, {}}}).action == Action::kNone);  // not editing: nothing
  menu.Select(RowOf(menu.View(), "Password:"));
  CHECK(menu.Text({TextEvent{true, TextKey::kCopy, {}}}).action == Action::kNone);  // a password is never copied
  Press(menu, TextKey::kSelectAll);
  Press(menu, TextKey::kBackspace);
  Press(menu, TextKey::kEnter);
  CHECK(Row(menu, "Password:").value == "None");
  menu.Select(RowOf(menu.View(), "Password:"));
  TypeText(menu, "pw");
  Press(menu, TextKey::kEnter);
  CHECK(Row(menu, "Password:").value == "pw");
  // The controller's letters: Right adds one, Up / Down change it, Left deletes, A is done.
  menu.Select(RowOf(menu.View(), "Lobby name:"));
  menu.Input(MenuInput::kRight);  // all selected: to the end
  menu.Input(MenuInput::kRight);  // a new letter (A)
  menu.Input(MenuInput::kDown);   // A -> B
  menu.Input(MenuInput::kSelect);
  CHECK(Row(menu, "Lobby name:").value == "Alice's lobby!B");
  menu.Select(RowOf(menu.View(), "Lobby name:"));
  menu.Input(MenuInput::kRight);  // all selected: Right goes to the end
  menu.Input(MenuInput::kLeft);   // deletes the B
  menu.Input(MenuInput::kSelect);
  CHECK(Row(menu, "Lobby name:").value == "Alice's lobby!");
}

TEST_CASE("lobby menus: host and guest by code; Start waits for the game's session; the guest is handed over",
          "[net][lobby][menu]") {
  LobbyService hs(Local()), gs(Local());
  LobbyMenu host(hs, "Alice"), guest(gs, "Bob");
  host.Open();
  host.Select(1);
  host.Select(RowOf(host.View(), "Create Lobby"));
  REQUIRE(host.Page() == MenuPage::kLobby);
  CHECK(Row(host, "Players:").value == "1 / 4: Alice");
  CHECK(RowOf(host.View(), "Start Game") == 0);  // the actions first, in reach of the pointer
  const std::string code = Row(host, "Invite code:").value;
  REQUIRE(code.rfind("KUF-", 0) == 0);
  CHECK(Row(host, "Invite code:").note == "This computer only");
  const MenuEffect copy = host.Select(RowOf(host.View(), "Copy Invite Code"));
  CHECK(copy.action == Action::kCopy);
  CHECK(copy.text == code);
  CHECK(RowOf(host.View(), "Invite Friends") == size_t(-1));  // no invitation provider: not offered
  CHECK_FALSE(Row(host, "Start Game").enabled);                  // alone

  guest.Open();
  guest.Select(3);  // Join by Code
  std::string lower = code;
  for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
  guest.Input(MenuInput::kSelect);  // edit the code field
  TypeText(guest, lower);           // pasted in lower case: shown and used in upper case
  Press(guest, TextKey::kEnter);
  CHECK(Row(guest, "Invite code:").value == code);
  guest.Select(RowOf(guest.View(), "Join"));
  REQUIRE(Until({&host, &guest}, [&] { return Row(host, "Players:").value == "2 / 4: Alice, Bob"; }));
  REQUIRE(Until({&host, &guest}, [&] { return Row(guest, "Status:").value == "Waiting for the host to start"; }));

  REQUIRE(Row(host, "Start Game").enabled);
  const MenuEffect start = host.Select(RowOf(host.View(), "Start Game"));
  CHECK(start.action == Action::kHostGame);
  CHECK(start.public_session);  // the default visibility is Public: the game's Host Public
  host.Tick();
  CHECK(hs.State().phase == Phase::kHosting);  // not started: the game's session does not exist yet
  CHECK_FALSE(Row(host, "Start Game").enabled);
  hs.SetSessionData({1, 2, 3});  // the game created its session (the runtime publishes it)
  MenuEffect handed;
  REQUIRE(Until({&host}, [&] {  // the guest is ticked here only: its effect is the handover
    if (handed.action == Action::kNone) handed = guest.Tick();
    return hs.State().phase == Phase::kInGame && handed.action == Action::kJoinGame;
  }));
  CHECK(host.View().message == "The game has started.");
}

TEST_CASE("lobby menus: Start fails visibly when the game's session never appears", "[net][lobby][menu]") {
  LobbyService hs(Local()), gs(Local());
  LobbyMenu host(hs, "Alice");
  host.start_timeout = 200ms;
  host.Open();
  host.Select(1);
  host.Select(RowOf(host.View(), "Create Lobby"));
  std::string error;
  REQUIRE(gs.JoinCode(hs.State().code, "Bob", "", &error));
  REQUIRE(Until({&host}, [&] { gs.Poll(); return hs.State().members.size() == 2; }));
  host.Select(RowOf(host.View(), "Start Game"));
  REQUIRE(Until({&host}, [&] { return host.View().message.find("could not be created") != std::string::npos; }));
  CHECK(hs.State().phase == Phase::kHosting);  // never claimed as started
  CHECK(Row(host, "Start Game").enabled);      // can be tried again
}

TEST_CASE("lobby menus: invalid code stays on the page with the reason", "[net][lobby][menu]") {
  LobbyService s(Local());
  LobbyMenu menu(s, "Bob");
  menu.Open();
  menu.Select(3);
  menu.Input(MenuInput::kSelect);
  TypeText(menu, "kuf-zzzz");
  Press(menu, TextKey::kEnter);
  menu.Select(RowOf(menu.View(), "Join"));
  CHECK(menu.Page() == MenuPage::kJoinCode);
  CHECK(menu.View().message.find("not a valid invite code") != std::string::npos);
}

TEST_CASE("lobby menus: the browser shows found lobbies only; a password lobby asks for it", "[net][lobby][menu]") {
  std::random_device rd;
  const uint16_t discovery = uint16_t(42000 + rd() % 8000);
  LobbyService hs(Local(discovery)), bs(Local());
  bs.AddProvider(MakeDirectProvider(discovery, 500ms, {"127.0.0.1"}));
  LobbyMenu host(hs, "Alice"), browser(bs, "Bob");
  browser.Open();
  browser.Select(2);  // Browse Lobbies
  REQUIRE(Until({&browser}, [&] { return !bs.State().refreshing; }));
  CHECK(RowOf(browser.View(), "No lobbies found") != size_t(-1));

  host.Open();
  host.Select(1);
  CHECK(Row(host, "Visibility:").value == "Public");  // LAN discovery answers public lobbies only
  host.Select(RowOf(host.View(), "Password:"));
  TypeText(host, "pw");
  Press(host, TextKey::kEnter);
  host.Select(RowOf(host.View(), "Create Lobby"));
  REQUIRE(host.Page() == MenuPage::kLobby);

  browser.Select(RowOf(browser.View(), "Refresh"));
  REQUIRE(Until({&host, &browser}, [&] { return !bs.State().refreshing && !bs.State().lobbies.empty(); }));
  const MenuView v = browser.View();
  CHECK(v.rows[0].label == "Refresh");
  REQUIRE(v.rows[1].label == "Alice's lobby");
  CHECK(v.rows[1].value.find("1/4") != std::string::npos);
  CHECK(v.rows[1].value.find("Password") != std::string::npos);  // the lobby has one (a marker, not a label)
  browser.Select(1);
  REQUIRE(browser.Page() == MenuPage::kPassword);
  browser.Input(MenuInput::kSelect);  // edit the password field
  TypeText(browser, "pw");
  Press(browser, TextKey::kEnter);
  browser.Select(RowOf(browser.View(), "Join"));
  REQUIRE(Until({&host, &browser}, [&] { return Row(host, "Players:").value.rfind("2 / 4", 0) == 0; }));
}

TEST_CASE("lobby menus: a long page scrolls with the selection", "[net][lobby][menu]") {
  LobbyService s(Local());
  LobbyMenu menu(s, "Alice");
  menu.SetVisible(4);
  menu.Open();
  menu.Select(1);  // Create: 8 rows, 4 shown
  for (int i = 0; i < 10; ++i) menu.Input(MenuInput::kDown);
  MenuView v = menu.View();
  CHECK(v.selected == v.rows.size() - 1);
  CHECK(v.top == v.rows.size() - 4);
  menu.Input(MenuInput::kPageUp);
  v = menu.View();
  CHECK(v.top <= v.selected);
  CHECK(v.selected < v.top + 4);
}

TEST_CASE("lobby menus: editing the name: replace, caret, Delete, Home/End, Ctrl+A, paste, no duplicates",
          "[net][lobby][menu][text]") {
  LobbyService s(Local());
  LobbyMenu menu(s, "Alice");
  menu.Open();
  menu.Select(1);                    // Create Lobby
  menu.Select(0);                    // a click on Lobby Name: editing, all selected
  REQUIRE(menu.Editing());
  CHECK(Row(menu, "Lobby name:").value == "[Alice's lobby]");
  TypeText(menu, "Keep");            // replaces the selection
  CHECK(Row(menu, "Lobby name:").value == "Keep|");
  Press(menu, TextKey::kHome);
  TypeText(menu, ">");
  Press(menu, TextKey::kRight);
  Press(menu, TextKey::kDelete);     // the 'e' after the caret
  Press(menu, TextKey::kEnd);
  TypeText(menu, "s");
  CHECK(Row(menu, "Lobby name:").value == ">Keps|");
  Press(menu, TextKey::kSelectAll);
  TypeText(menu, "Paste Me");        // Ctrl+V's text arrives as text: once
  Press(menu, TextKey::kEnter);
  CHECK(Row(menu, "Lobby name:").value == "Paste Me");
  // A long name stays inside the row while edited (clipped around the caret).
  menu.Select(0);
  TypeText(menu, std::string(24, 'x'));
  CHECK(Row(menu, "Lobby name:").value.size() <= kFieldChars);
  Press(menu, TextKey::kEnter);
  CHECK(Row(menu, "Lobby name:").value == std::string(24, 'x'));  // the limit: 24 characters
}

TEST_CASE("lobby menus: clicking Create Lobby while typing keeps the text and creates the lobby",
          "[net][lobby][menu][text]") {
  LobbyService s(Local());
  LobbyMenu menu(s, "Alice");
  menu.Open();
  menu.Select(1);
  menu.Select(0);
  TypeText(menu, "Night Raid");
  const MenuEffect e = menu.Select(RowOf(menu.View(), "Create Lobby"));  // the click ends the edit, then acts
  CHECK(e.action == Action::kNone);
  REQUIRE(menu.Page() == MenuPage::kLobby);
  CHECK(s.State().info.name == "Night Raid");
  CHECK(menu.View().message == "Lobby created.");
}

TEST_CASE("lobby menus: no stale 'You left the lobby'; Back while editing only cancels the edit",
          "[net][lobby][menu][text]") {
  LobbyService s(Local());
  LobbyMenu menu(s, "Alice");
  menu.Open();
  menu.Select(1);
  menu.Select(RowOf(menu.View(), "Create Lobby"));
  REQUIRE(menu.Page() == MenuPage::kLobby);
  menu.Select(RowOf(menu.View(), "Leave Lobby"));
  CHECK(menu.Page() == MenuPage::kHome);
  CHECK(menu.View().message == "You left the lobby.");  // where it happened
  menu.Select(1);
  CHECK(menu.Page() == MenuPage::kCreate);
  CHECK(menu.View().message.empty());  // not carried to the next page
  // The controller's B (and Esc) while a field is edited: only the edit ends, the page stays.
  menu.Select(0);
  TypeText(menu, "zz");
  menu.Input(MenuInput::kBack);
  CHECK(menu.Page() == MenuPage::kCreate);
  CHECK_FALSE(menu.Editing());
  CHECK(Row(menu, "Lobby name:").value == "Alice's lobby");
  CHECK(menu.View().message.empty());
  // Text events without an edit do nothing (no field changes, no page change).
  TypeText(menu, "q");
  Press(menu, TextKey::kBackspace);
  CHECK(menu.Page() == MenuPage::kCreate);
  CHECK(Row(menu, "Lobby name:").value == "Alice's lobby");
}

TEST_CASE("text field: edits, selection, filter, display", "[net][lobby][text]") {
  TextField f;
  f.Begin("abc", 6, TextField::Filter::kText);
  CHECK(f.all_selected());
  CHECK(f.Display(20, false, true) == "[abc]");
  f.Left();  // ends the selection at the start
  CHECK(f.caret() == 0);
  f.Insert("Z");
  f.End();
  f.Insert("defgh");  // up to the limit
  CHECK(f.text() == "Zabcde");
  f.Backspace();
  CHECK(f.text() == "Zabcd");
  CHECK(f.Display(20, true, true) == "*****|");  // masked, the caret shown
  CHECK(f.Display(20, true, false) == "***** ");  // blinking
  f.Begin("kuf-ab12", 40, TextField::Filter::kCode);
  f.End();
  f.Insert("x!y");
  CHECK(f.text() == "KUF-AB12XY");  // codes: upper case, letters, digits and '-' only
  f.Begin(std::string(30, 'a'), 40, TextField::Filter::kText);
  f.End();
  const std::string shown = f.Display(10, false, true);
  CHECK(shown.size() == 10);
  CHECK(shown.front() == '<');
  CHECK(shown.back() == '|');
  f.SelectAll();
  f.Delete();
  CHECK(f.text().empty());
}

TEST_CASE("lobby menus: Swedish names, Ctrl+C of a name, a guest's password masked", "[net][lobby][menu][text]") {
  LobbyService s(Local());
  LobbyMenu menu(s, "Alice");
  menu.Open();
  menu.Select(1);
  menu.Select(0);
  TypeText(menu, "Gr\xC3\xB6na \xC3\x85sen");  // Gröna Åsen
  Press(menu, TextKey::kBackspace);           // one character, not one byte
  CHECK(Row(menu, "Lobby name:").value == "Gr\xC3\xB6na \xC3\x85se|");
  const MenuEffect copy = menu.Text({TextEvent{true, TextKey::kCopy, {}}});
  CHECK(copy.action == Action::kCopy);
  CHECK(copy.text == "Gr\xC3\xB6na \xC3\x85se");
  Press(menu, TextKey::kEnter);
  menu.Select(RowOf(menu.View(), "Create Lobby"));
  REQUIRE(menu.Page() == MenuPage::kLobby);
  CHECK(s.State().info.name == "Gr\xC3\xB6na \xC3\x85se");  // the lobby keeps the UTF-8 name
  // A guest typing a lobby's password: masked.
  LobbyService g(Local());
  LobbyMenu guest(g, "Bob");
  guest.Open();
  guest.Select(3);  // Join by Code
  guest.Select(RowOf(guest.View(), "Password:"));
  TypeText(guest, "secret");
  CHECK(Row(guest, "Password:").value == "******|");
  Press(guest, TextKey::kEnter);
  CHECK(Row(guest, "Password:").value == "******");
}

TEST_CASE("lobby service: the host's password is in no advertisement, listing or invite", "[net][lobby][text]") {
  ServiceConfig c = Local();
  std::random_device rd;
  c.discovery_port = uint16_t(42000 + rd() % 8000);
  LobbyService host(c);
  LobbySettings set;
  set.name = "Night";
  set.player = "Alice";
  set.password = "MyFriends123";
  std::string error;
  REQUIRE(host.Create(set, &error));
  const Snapshot st = host.State();
  CHECK(st.info.password);  // only whether there is one
  CHECK(st.code.find("MyFriends123") == std::string::npos);
  // LAN discovery's answer and Steam's metadata: never the password.
  const auto found = kuf::net::Discover({"127.0.0.1"}, c.discovery_port, 500ms);
  REQUIRE(found.size() == 1);
  CHECK(found[0].info.password);
  for (const auto& [k, v] : kuf::net::steam::LobbyMetadata(found[0].info)) {
    CHECK(v.find("MyFriends123") == std::string::npos);
  }
  const auto answer = kuf::net::EncodeDiscoveryAnswer(kuf::net::Nonce{}, st.info);
  CHECK(std::string(answer.begin(), answer.end()).find("MyFriends123") == std::string::npos);
}

TEST_CASE("lobby menus: a row's label is fixed whatever its value; values are clipped to their column",
          "[net][lobby][menu][layout]") {
  LobbyService s(Local());
  LobbyMenu menu(s, "Alice");
  menu.Open();
  menu.Select(1);  // Create Lobby
  auto split = [&](const std::string& label) { return SplitRow(Row(menu, label), 28); };
  const RowText before = split("Lobby name:");
  CHECK(before.label == "Lobby name:");
  CHECK(before.button.empty());  // the button keeps its frame, highlight and hit box only
  CHECK(before.value == "Alice's lobby");
  // Typing, deleting, an empty value, a long value: the label stays the same text (its control never moves).
  menu.Select(0);
  CHECK(split("Lobby name:").label == "Lobby name:");
  Press(menu, TextKey::kBackspace);  // all selected: emptied
  CHECK(split("Lobby name:").label == "Lobby name:");
  CHECK(split("Lobby name:").value == "|");
  TypeText(menu, "My Co-op Lobby");
  CHECK(split("Lobby name:").label == "Lobby name:");
  CHECK(split("Lobby name:").value == "My Co-op Lobby|");  // the caret at the end, inside the value column
  Press(menu, TextKey::kHome);
  CHECK(split("Lobby name:").value == "|My Co-op Lobby");
  Press(menu, TextKey::kEnd);
  TypeText(menu, std::string(30, 'x'));  // past the limit: 24 characters, scrolled around the caret
  const RowText longer = split("Lobby name:");
  CHECK(longer.label == "Lobby name:");
  CHECK(longer.value.size() <= 28);
  CHECK(longer.value.back() == '|');
  Press(menu, TextKey::kEnter);
  // The labels as asked, each in the label column; the host's password visible.
  CHECK(split("Max players:").value == "4");
  CHECK(split("Visibility:").value == "Public");
  menu.Select(RowOf(menu.View(), "Password:"));
  TypeText(menu, "nisse");
  Press(menu, TextKey::kEnter);
  CHECK(split("Password:").label == "Password:");
  CHECK(split("Password:").value == "nisse");
  // Actions are buttons (their own text, no columns).
  const RowText create = split("Create Lobby");
  CHECK(create.button == "Create Lobby");
  CHECK(create.label.empty());
  CHECK(create.value.empty());
  // A value longer than its column: cut with '>' (on a character, not inside one).
  MenuRow wide{MenuRow::kInfo, "Host:", std::string(10, 'a') + "\xC3\xA4\xC3\xA4" + std::string(30, 'b'), true, ""};
  const RowText cut = SplitRow(wide, 12);
  CHECK(cut.value == std::string(10, 'a') + "\xC3\xA4>");
}

TEST_CASE("lobby menus: short descriptions; the editing hint while typing", "[net][lobby][menu][layout]") {
  LobbyService s(Local());
  LobbyMenu menu(s, "Alice");
  menu.Open();
  menu.Select(1);
  CHECK(Row(menu, "Lobby name:").note == "Choose a name for your lobby.");
  CHECK(Row(menu, "Password:").note == "Optional password required to join.");
  CHECK(Row(menu, "Max players:").note == "Choose how many players can join (2-4).");
  CHECK(Row(menu, "Visibility:").note == "Choose who can discover your lobby.");
  CHECK(Row(menu, "Create Lobby").note.rfind("Create your multiplayer lobby", 0) == 0);
  menu.Select(0);
  CHECK(menu.View().help == "Type to edit \xC2\xB7 Enter to confirm \xC2\xB7 Esc to cancel");
}

TEST_CASE("lobby menus: only the selected row takes the selected look; a field being typed into stays steady",
          "[net][lobby][menu][layout]") {
  CHECK(SelectedLook(true, false, false));   // label of the selected row
  CHECK(SelectedLook(true, false, true));    // and its value
  CHECK_FALSE(SelectedLook(false, false, false));
  CHECK_FALSE(SelectedLook(false, true, true));
  CHECK(SelectedLook(true, true, false));    // typing: the label keeps the look
  CHECK_FALSE(SelectedLook(true, true, true));  // the text and caret do not
}
