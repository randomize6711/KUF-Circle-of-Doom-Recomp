#include "coop/lobby_menu.h"

#include <algorithm>
#include <cctype>

namespace kuf::coop {
namespace {

// Rows of the pages (by index).
enum HomeRow { kHomeSolo, kHomeCreate, kHomeBrowse, kHomeCode, kHomeBack };
// Create: these first rows are fixed; Invitations (4) only with Steam, then Create Lobby and Back (by label).
enum CreateRow { kCreateName, kCreatePlayers, kCreateVisibility, kCreatePassword, kCreateBackend };
enum CodeRow { kCodeCode, kCodePassword, kCodeJoin, kCodeBack };
enum PasswordRow { kPassLobby, kPassPassword, kPassJoin, kPassBack };

const char* VisibilityText(net::Visibility v) {
  switch (v) {
    case net::Visibility::kFriends: return "Friends Only";
    case net::Visibility::kPrivate: return "Private (invite code)";
    default: return "Public";
  }
}

bool Selectable(const MenuRow& r) { return r.kind != MenuRow::kInfo; }

const ProviderStatus* Steam(const Snapshot& s) {
  for (const ProviderStatus& p : s.providers) {
    if (p.backend == net::Backend::kSteam) return &p;
  }
  return nullptr;
}


}  // namespace

RowText SplitRow(const MenuRow& r, size_t value_chars) {
  RowText t;
  if (r.kind == MenuRow::kAction && r.value.empty()) {
    t.button = r.label;  // a plain action: the game's own button text
    return t;
  }
  t.label = r.label;  // fixed: never depends on the value
  // The value in its own column: clipped to it (an edited field is already clipped around its caret).
  size_t chars = 0, cut = r.value.size();
  for (size_t i = 0; i < r.value.size(); ++i) {
    if ((static_cast<unsigned char>(r.value[i]) & 0xC0) == 0x80) continue;
    if (chars == value_chars) {
      cut = i;
      break;
    }
    ++chars;
  }
  t.value = r.value.substr(0, cut);
  if (cut < r.value.size() && !t.value.empty()) {
    while (!t.value.empty() && (static_cast<unsigned char>(t.value.back()) & 0xC0) == 0x80) t.value.pop_back();
    if (!t.value.empty()) t.value.back() = '>';
  }
  return t;
}

bool SelectedLook(bool row_selected, bool editing, bool value_column) {
  return row_selected && !(editing && value_column);
}

std::string Masked(const std::string& s) {
  size_t chars = 0;
  for (unsigned char b : s) chars += (b & 0xC0) != 0x80;  // UTF-8 characters, not bytes
  return std::string(chars, '*');
}

LobbyMenu::LobbyMenu(LobbyService& service, std::string player) : service_(service), player_(std::move(player)) {
  visible_ = 6;
  settings_.name = player_ + "'s lobby";
  settings_.player = player_;
}

void LobbyMenu::SetGameDifficulty(uint8_t index, std::string name) {
  settings_.difficulty = index;
  difficulty_name_ = name.empty() ? "as on the game type screen" : std::move(name);
}

void LobbyMenu::Open() {
  Go(service_.Active() ? MenuPage::kLobby : MenuPage::kHome);
}

void LobbyMenu::Go(MenuPage p, size_t selected) {
  page_ = p;
  editing_ = false;
  message_.clear();  // a message belongs to the page it was shown on (set after Go when it is about the new page)
  top_ = 0;
  selected_ = selected;
  Clamp();
}

void LobbyMenu::Clamp() {
  const auto rows = Rows();
  if (rows.empty()) {
    selected_ = top_ = 0;
    return;
  }
  selected_ = std::min(selected_, rows.size() - 1);
  // The selection rests on a selectable row: the nearest one below, else above.
  size_t s = selected_;
  while (s < rows.size() && !Selectable(rows[s])) ++s;
  if (s == rows.size()) {
    s = selected_;
    while (s > 0 && !Selectable(rows[s])) --s;
  }
  selected_ = s;
  if (selected_ < top_) top_ = selected_;
  if (selected_ >= top_ + visible_) top_ = selected_ + 1 - visible_;
  if (rows.size() <= visible_) top_ = 0;
  top_ = std::min(top_, rows.size() > visible_ ? rows.size() - visible_ : size_t(0));
}

std::vector<MenuRow> LobbyMenu::Rows() const {
  const Snapshot s = service_.State();
  std::vector<MenuRow> r;
  auto action = [&](std::string label, bool enabled = true, std::string note = "") {
    r.push_back({MenuRow::kAction, std::move(label), "", enabled, std::move(note)});
  };
  auto info = [&](std::string label, std::string value = "", std::string note = "") {
    r.push_back({MenuRow::kInfo, std::move(label), std::move(value), true, std::move(note)});
  };
  // The row being edited shows the field: its caret, its selection, clipped to the row, masked for a password.
  auto text = [&](std::string label, std::string value, std::string note) {
    // A password is masked where a guest types one; the host's own on Create Lobby is shown as typed.
    const bool mask = label == "Password:" && page_ != MenuPage::kCreate;
    if (editing_ && r.size() == selected_) value = field_.Display(kFieldChars, mask, caret_on_);
    r.push_back({MenuRow::kText, std::move(label), std::move(value), true, std::move(note)});
  };
  switch (page_) {
    case MenuPage::kHome:
      action("Continue Solo Game");
      action("Create Lobby");
      action("Browse Lobbies");
      action("Join by Code");
      action("Back");
      break;
    case MenuPage::kCreate: {
      const ProviderStatus* steam = Steam(s);
      const bool steam_ok = steam && steam->available;
      text("Lobby name:", settings_.name, "Choose a name for your lobby.");
      r.push_back({MenuRow::kChoice, "Max players:", std::to_string(settings_.max_players), true,
                   "Choose how many players can join (2-4)."});
      r.push_back({MenuRow::kChoice, "Visibility:", VisibilityText(settings_.visibility), true,
                   "Choose who can discover your lobby."});
      text("Password:", settings_.password.empty() ? "None" : settings_.password, "Optional password required to join.");
      if (steam_ok) {  // only a working invitation service is offered (else the code alone; the reason below)
        r.push_back({MenuRow::kChoice, "Invitations:",
                     settings_.backend == net::Backend::kSteam ? "Steam" : "Invite code (LAN / direct)", true,
                     "Steam adds friend invitations; the game itself always connects directly."});
      }
      action("Create Lobby", true, "Create your multiplayer lobby (difficulty: " + difficulty_name_ + ").");
      action("Back", true, "Return to the multiplayer menu.");
      break;
    }
    case MenuPage::kLobby: {
      // The actions first (they stay in view; the pointer reaches them), then the players and the code.
      const bool host = s.host;
      const size_t max = std::max<size_t>(s.info.max_players, s.members.size());
      std::string names;
      for (const net::Member& m : s.members) names += (names.empty() ? "" : ", ") + m.name;
      if (host) {
        const bool enough = s.members.size() >= net::kMinPlayers;
        action("Start Game", enough && !starting_ && s.phase == Phase::kHosting,
               starting_ ? "Starting the game's co-op session..." : enough ? "" : "Waiting for another player.");
        if (!s.code.empty()) action("Copy Invite Code", true, ReachText(s.reach));
        for (const ProviderStatus& p : s.providers) {
          if (p.available && p.can_invite && p.backend == settings_.backend) action("Invite Friends", true, p.name);
        }
        action("Leave Lobby");
        info("Lobby:", s.info.name);
        info("Players:", std::to_string(s.members.size()) + " / " + std::to_string(max) + ": " + names);
        if (s.code.empty()) {
          info("Invite code:", "Not available", "This computer has no network address for a code.");
        } else {
          info("Invite code:", s.code, ReachText(s.reach));
        }
      } else {
        info("Status:", s.phase == Phase::kJoining ? "Connecting..."
                       : s.phase == Phase::kInGame ? "In the game"
                       : handed_ ? "Joining the host's game..."
                                 : "Waiting for the host to start");
        action("Leave Lobby");
        info("Lobby:", s.info.name);
        info("Players:", std::to_string(s.members.size()) + " / " + std::to_string(max) + ": " + names);
        info("Host:", s.info.host);
      }
      break;
    }
    case MenuPage::kBrowse: {
      action("Refresh");
      for (const LobbyEntry& e : s.lobbies) {
        std::string value = e.info.host + "  " + std::to_string(e.info.players) + "/" +
                            std::to_string(e.info.max_players) + "  Difficulty " +
                            std::to_string(int(e.info.difficulty) + 1) + (e.info.password ? "  Password" : "") +
                            (e.backend == net::Backend::kSteam ? "  Steam" : "  LAN");
        r.push_back({MenuRow::kAction, e.info.name, std::move(value), e.joinable, e.reason});
      }
      if (s.refreshing) {
        info("Searching...");
      } else if (s.lobbies.empty()) {
        info("No lobbies found", "", "Only lobbies a working service found are shown (LAN: public lobbies).");
      }
      action("Back");
      for (const ProviderStatus& p : s.providers) {
        if (!p.available) info(p.name, "Off", p.reason);
      }
      break;
    }
    case MenuPage::kJoinCode:
      text("Invite code:", code_, "Enter the invite code you were given (KUF-...).");
      text("Password:", password_.empty() ? "None" : Masked(password_), "Only if the lobby has one.");
      action("Join", !code_.empty());
      action("Back");
      break;
    case MenuPage::kPassword:
      info("Lobby:", pending_.info.name);
      text("Password:", password_.empty() ? "None" : Masked(password_), "This lobby has a password.");
      action("Join", !password_.empty());
      action("Back");
      break;
  }
  return r;
}

MenuView LobbyMenu::View() const {
  MenuView v;
  v.page = page_;
  v.rows = Rows();
  v.selected = selected_;
  v.top = top_;
  v.visible = visible_;
  v.editing = editing_;
  v.message = message_;
  static const char* titles[] = {"Multiplayer", "Create Lobby", "Lobby", "Browse Lobbies", "Join by Code", "Password"};
  v.title = titles[int(page_)];
  if (page_ == MenuPage::kCreate) v.title += "  (Difficulty: " + difficulty_name_ + ")";
  if (page_ == MenuPage::kLobby) {
    const Snapshot s = service_.State();
    if (!s.info.name.empty()) v.title = s.info.name;
  }
  if (v.rows.size() > visible_) v.title += "  " + std::to_string(selected_ + 1) + "/" + std::to_string(v.rows.size());
  v.help = editing_ ? "Type to edit \u00B7 Enter to confirm \u00B7 Esc to cancel"
                    : "@(Dpad_Small)Move  @(A)Select  @(B)Back";
  return v;
}

bool LobbyMenu::TextRowSelected() const {
  const auto rows = Rows();
  return selected_ < rows.size() && rows[selected_].kind == MenuRow::kText;
}

std::string* LobbyMenu::EditedText() {
  if (page_ == MenuPage::kCreate && selected_ == kCreateName) return &settings_.name;
  if (page_ == MenuPage::kCreate && selected_ == kCreatePassword) return &settings_.password;
  if (page_ == MenuPage::kJoinCode && selected_ == kCodeCode) return &code_;
  if (page_ == MenuPage::kJoinCode && selected_ == kCodePassword) return &password_;
  if (page_ == MenuPage::kPassword && selected_ == kPassPassword) return &password_;
  return nullptr;
}

size_t LobbyMenu::MaxText() const {
  if (page_ == MenuPage::kJoinCode && selected_ == kCodeCode) return kMaxCodeChars;
  if (page_ == MenuPage::kCreate && selected_ == kCreateName) return kMaxNameChars;
  return kMaxPasswordChars;
}

bool LobbyMenu::BeginEdit() {
  std::string* t = EditedText();
  if (!t) return false;
  const bool code = page_ == MenuPage::kJoinCode && selected_ == kCodeCode;
  field_.Begin(*t, MaxText(), code ? TextField::Filter::kCode : TextField::Filter::kText);
  editing_ = true;
  caret_on_ = true;
  return true;
}

void LobbyMenu::Commit() {
  if (!editing_) return;
  if (std::string* t = EditedText()) *t = field_.text();
  editing_ = false;
}

void LobbyMenu::Cancel() { editing_ = false; }  // the stored text was never changed

MenuEffect LobbyMenu::Text(const std::vector<input::TextEvent>& events) {
  if (!editing_) return {};
  using input::TextKey;
  MenuEffect effect;
  for (const input::TextEvent& e : events) {
    if (!editing_) break;  // Enter / Esc ended it: the rest is not this field's
    if (!e.is_key) {
      field_.Insert(e.text);
      continue;
    }
    switch (e.key) {
      case TextKey::kBackspace: field_.Backspace(); break;
      case TextKey::kDelete: field_.Delete(); break;
      case TextKey::kLeft: field_.Left(); break;
      case TextKey::kRight: field_.Right(); break;
      case TextKey::kHome: field_.Home(); break;
      case TextKey::kEnd: field_.End(); break;
      case TextKey::kSelectAll: field_.SelectAll(); break;
      case TextKey::kCopy: {  // the field's text; a password is never copied
        const auto rows = Rows();
        if (selected_ < rows.size() && rows[selected_].label != "Password:") effect = {Action::kCopy, field_.text()};
        break;
      }
      case TextKey::kEnter: Commit(); break;
      case TextKey::kEscape: Cancel(); break;
    }
    caret_on_ = true;
  }
  return effect;
}

void LobbyMenu::Change(size_t row, int dir) {
  if (page_ != MenuPage::kCreate) return;
  const Snapshot s = service_.State();
  const ProviderStatus* steam = Steam(s);
  switch (row) {
    case kCreatePlayers:
      settings_.max_players = uint8_t(std::clamp<int>(settings_.max_players + dir, net::kMinPlayers, net::kMaxPlayers));
      break;
    case kCreateVisibility: {
      // Friends Only needs a friends list (Steam).
      std::vector<net::Visibility> options = {net::Visibility::kPublic, net::Visibility::kPrivate};
      if (settings_.backend == net::Backend::kSteam) options.insert(options.begin() + 1, net::Visibility::kFriends);
      size_t i = std::find(options.begin(), options.end(), settings_.visibility) - options.begin();
      i = i >= options.size() ? 0 : (i + options.size() + size_t(dir > 0 ? 1 : options.size() - 1)) % options.size();
      settings_.visibility = options[i];
      break;
    }
    case kCreateBackend:
      if (steam && steam->available) {
        settings_.backend = settings_.backend == net::Backend::kSteam ? net::Backend::kDirect : net::Backend::kSteam;
        if (settings_.backend == net::Backend::kDirect && settings_.visibility == net::Visibility::kFriends) {
          settings_.visibility = net::Visibility::kPrivate;
        }
      }
      break;
    default:
      break;
  }
}

MenuEffect LobbyMenu::Activate(size_t row) {
  const auto rows = Rows();
  if (row >= rows.size() || !Selectable(rows[row])) return {};
  if (!rows[row].enabled) {
    message_ = rows[row].note;
    return {};
  }
  if (rows[row].kind == MenuRow::kText) {
    if (editing_) {
      Commit();  // A / Enter on the field being edited: done
    } else {
      BeginEdit();  // a click, Enter or A: edit it (all selected: typing replaces it)
    }
    return {};
  }
  if (rows[row].kind == MenuRow::kChoice) {
    Change(row, +1);
    return {};
  }
  std::string error;
  switch (page_) {
    case MenuPage::kHome:
      switch (row) {
        case kHomeSolo: return {Action::kSolo};
        case kHomeCreate: Go(MenuPage::kCreate); return {};
        case kHomeBrowse:
          service_.Refresh();
          Go(MenuPage::kBrowse);
          return {};
        case kHomeCode: Go(MenuPage::kJoinCode); return {};
        default: return {Action::kLeave};
      }
    case MenuPage::kCreate:
      if (rows[row].label == "Back") {
        Go(MenuPage::kHome, kHomeCreate);
        return {};
      }
      settings_.player = player_;
      if (settings_.name.empty()) settings_.name = player_ + "'s lobby";
      if (!service_.Create(settings_, &error)) {
        message_ = error;
        return {};
      }
      Go(MenuPage::kLobby);
      message_ = "Lobby created.";
      return {};
    case MenuPage::kLobby: {
      const std::string label = rows[row].label;
      if (label == "Leave Lobby") {
        service_.Leave();
        starting_ = handed_ = false;
        Go(MenuPage::kHome);
        message_ = "You left the lobby.";  // on Home only; the next page change clears it
        return {};
      }
      if (label == "Copy Invite Code") {
        message_ = "Invite code copied.";
        return {Action::kCopy, service_.State().code};
      }
      if (label == "Invite Friends") {
        if (!service_.OpenInviteDialog(&error)) message_ = error;
        return {};
      }
      if (label == "Start Game") {
        starting_ = true;
        step_start_ = std::chrono::steady_clock::now();
        message_ = "Starting the game's co-op session...";
        return {Action::kHostGame, "", settings_.visibility == net::Visibility::kPublic};
      }
      return {};
    }
    case MenuPage::kBrowse: {
      const Snapshot s = service_.State();
      if (rows[row].label == "Refresh") {
        service_.Refresh();
        return {};
      }
      if (rows[row].label == "Back" && row > s.lobbies.size()) {
        Go(MenuPage::kHome, kHomeBrowse);
        return {};
      }
      if (row >= 1 && row - 1 < s.lobbies.size()) {
        pending_ = s.lobbies[row - 1];
        password_.clear();
        if (pending_.info.password) {
          Go(MenuPage::kPassword, kPassPassword);
          return {};
        }
        if (!service_.Join(pending_, player_, "", &error)) {
          message_ = error;
          return {};
        }
        Go(MenuPage::kLobby);
        message_ = "Connecting...";
      }
      return {};
    }
    case MenuPage::kJoinCode:
      if (row == kCodeBack) {
        Go(MenuPage::kHome, kHomeCode);
        return {};
      }
      if (!service_.JoinCode(code_, player_, password_, &error)) {
        message_ = error;
        return {};
      }
      Go(MenuPage::kLobby);
      message_ = "Connecting...";
      return {};
    case MenuPage::kPassword:
      if (row == kPassBack) {
        Go(MenuPage::kBrowse);
        return {};
      }
      if (!service_.Join(pending_, player_, password_, &error)) {
        message_ = error;
        return {};
      }
      Go(MenuPage::kLobby);
      message_ = "Connecting...";
      return {};
  }
  return {};
}

MenuEffect LobbyMenu::Select(size_t row) {
  const auto rows = Rows();
  if (row >= rows.size() || !Selectable(rows[row])) return {};
  if (editing_) {
    Commit();  // a click ends the edit, keeping the text
    if (row == selected_) return {};  // on the field itself: just that
  }
  selected_ = row;
  Clamp();
  return Activate(row);
}

MenuEffect LobbyMenu::Input(MenuInput in) {
  const auto rows = Rows();
  if (editing_) {
    // Only the controller's buttons arrive here while a field is edited (the keyboard's keys go to Text, the
    // pointer's hover is not given to the menu): letters step, B cancels, A is done.
    switch (in) {
      case MenuInput::kBack: Cancel(); return {};
      case MenuInput::kSelect: Commit(); return {};
      case MenuInput::kUp: field_.Step(-1); return {};
      case MenuInput::kDown: field_.Step(+1); return {};
      case MenuInput::kRight: field_.Next(); return {};
      case MenuInput::kLeft: field_.Backspace(); return {};
      default: return {};
    }
  }
  auto step = [&](int dir) {
    size_t s = selected_;
    for (;;) {
      if (dir < 0 && s == 0) return;
      if (dir > 0 && s + 1 >= rows.size()) return;
      s = size_t(int(s) + dir);
      if (Selectable(rows[s])) break;
    }
    selected_ = s;
    Clamp();
  };
  switch (in) {
    case MenuInput::kUp: step(-1); return {};
    case MenuInput::kDown: step(+1); return {};
    case MenuInput::kPageUp:
      for (size_t i = 0; i < visible_; ++i) step(-1);
      return {};
    case MenuInput::kPageDown:
      for (size_t i = 0; i < visible_; ++i) step(+1);
      return {};
    case MenuInput::kLeft:
    case MenuInput::kRight:
      if (selected_ < rows.size() && rows[selected_].kind == MenuRow::kChoice && rows[selected_].enabled) {
        Change(selected_, in == MenuInput::kRight ? +1 : -1);
      }
      return {};
    case MenuInput::kSelect: return Activate(selected_);
    case MenuInput::kBack:
      switch (page_) {
        case MenuPage::kHome: return {Action::kLeave};
        case MenuPage::kCreate: Go(MenuPage::kHome, kHomeCreate); return {};
        case MenuPage::kBrowse: Go(MenuPage::kHome, kHomeBrowse); return {};
        case MenuPage::kJoinCode: Go(MenuPage::kHome, kHomeCode); return {};
        case MenuPage::kPassword: Go(MenuPage::kBrowse); return {};
        case MenuPage::kLobby:  // Back goes to Leave Lobby (leaving is a deliberate choice)
          for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].label == "Leave Lobby") selected_ = i;
          }
          Clamp();
          return {};
      }
      return {};
    default:
      return {};
  }
}

void LobbyMenu::GameStepFailed(const std::string& why) {
  starting_ = false;
  message_ = why;
}

MenuEffect LobbyMenu::Tick() {
  service_.Poll();
  const Snapshot s = service_.State();
  if (!s.error.empty() && s.error != seen_error_) {
    seen_error_ = s.error;
    message_ = s.error;
  }
  if (page_ == MenuPage::kLobby && s.phase == Phase::kIdle) {
    starting_ = handed_ = false;
    const std::string why = message_;
    Go(MenuPage::kHome);  // the lobby is gone (rejected, lost): the message says why
    message_ = why;
    return {};
  }
  if (editing_) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    caret_on_ = (ms / 530) % 2 == 0;  // the caret blinks
  }
  if (starting_) {
    if (s.session_ready) {
      std::string error;
      starting_ = false;
      if (service_.StartGame(&error)) {
        message_ = "The game has started.";
      } else {
        message_ = error;
      }
    } else if (std::chrono::steady_clock::now() - step_start_ > start_timeout) {
      starting_ = false;
      message_ = "The game's co-op session could not be created; the game was not started.";
    }
  }
  // The host starts its lobby as soon as the game's session exists, so the session and the start often arrive
  // together: a guest is handed over in either phase.
  if (!s.host && (s.phase == Phase::kInLobby || s.phase == Phase::kInGame) && s.session_ready && !handed_) {
    handed_ = true;
    message_ = "Joining the host's game...";
    return {Action::kJoinGame};
  }
  Clamp();
  return {};
}

}  // namespace kuf::coop
