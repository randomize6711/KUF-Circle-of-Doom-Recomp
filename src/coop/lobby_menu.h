// The multiplayer menus as a model (docs/MULTIPLAYER.md "Menus"): pages of rows, the selection, scrolling, text entry
// and what the game has to do, independent of how they are drawn. The renderer (runtime/lobby_screen.cpp) draws a
// View in the game's own menu rows and turns an Action into the game's own steps; tests drive it directly.
//
//   Home       Continue Solo Game, Create Lobby, Browse Lobbies, Join by Code, Back
//   Create     Lobby Name, Max Players, Visibility, Password, Invitations (only with Steam available), Create Lobby,
//              Back (the title shows the game's own difficulty: it follows the character, KNOWN_ISSUES K61)
//   Lobby      the actions first (Start Game for the host, Copy Invite Code, Invite Friends only with an invitation
//              provider, Leave Lobby), then the players (n / max: names) and the invite code with its reach
//   Browse     Refresh, the lobbies working providers found (name, host, players, difficulty, password), Back
//   Join Code  Code, Password, Join, Back
//   Password   for a browsed lobby that has one
//
// Start Game asks the game for its own co-op session first (Action::kHostGame); the lobby counts as started only
// once that session exists (the service's session_ready), else the page shows the failure. A guest is handed to the
// game's own join (Action::kJoinGame) once the host's session has arrived.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "coop/lobby_service.h"
#include "coop/text_field.h"
#include "input/text_entry.h"

namespace kuf::coop {

enum class MenuPage { kHome, kCreate, kLobby, kBrowse, kJoinCode, kPassword };

struct MenuRow {
  enum Kind { kAction, kChoice, kText, kInfo } kind = kAction;
  std::string label;
  std::string value;     // a choice's or text's value (a password masked)
  bool enabled = true;   // greyed when false (still shown: the reason is in `note`)
  std::string note;      // why it is unavailable, or a description
};

struct MenuView {
  MenuPage page = MenuPage::kHome;
  std::string title;
  std::vector<MenuRow> rows;  // all of the page's rows
  size_t selected = 0;
  size_t top = 0;             // the first row shown (scrolling)
  size_t visible = 6;         // rows shown at a time
  bool editing = false;       // the selected text row takes typed characters
  std::string message;        // the last result or error
  std::string help;           // the button help line
};

// The menu's buttons (keyboard navigation, the controller, the pointer's steps). Text editing is not here: a field
// being edited takes the keyboard's text events (Text); Backspace is never a menu input.
enum class MenuInput { kUp, kDown, kLeft, kRight, kSelect, kBack, kPageUp, kPageDown };

// What the game has to do for the menu (the renderer's part).
enum class Action {
  kNone,
  kSolo,      // Continue Solo Game: the game's own single-player row
  kLeave,     // Back from Home: the game's previous screen
  kHostGame,  // Start Game: the game's own Host Public / Host Private row (creates its co-op session)
  kJoinGame,  // a guest: the game's own join (its session search finds the host's session)
  kCopy,      // copy `text` to the clipboard
};
struct MenuEffect {
  Action action = Action::kNone;
  std::string text;
  bool public_session = true;  // kHostGame: Host Public (true) or Host Private
};

class LobbyMenu {
 public:
  LobbyMenu(LobbyService& service, std::string player);
  void Open();  // shows Home (or the lobby if one is open)
  MenuEffect Input(MenuInput in);
  // The keyboard's text events while a field is edited (input/text_entry.h): typing, Backspace, Delete, the caret,
  // Ctrl+A, Ctrl+C (kCopy: never a password), pasted text, Enter (done) and Esc (cancel). Ignored when no field is
  // edited. The host's password on Create Lobby is shown as typed; a guest's is masked.
  MenuEffect Text(const std::vector<input::TextEvent>& events);
  bool Editing() const { return editing_; }
  MenuEffect Select(size_t row);          // the mouse: the row under the pointer, clicked
  MenuEffect Tick();                      // the service's progress (start, join, errors), once per tick
  MenuView View() const;
  MenuPage Page() const { return page_; }
  bool TextRowSelected() const;
  void SetVisible(size_t rows) {
    visible_ = rows ? rows : 1;
    Clamp();
  }
  // The game: its host / join step did not produce a session in time.
  void GameStepFailed(const std::string& why);
  // The game type screen's Difficulty (its index and the text it shows); the lobby is created with it.
  void SetGameDifficulty(uint8_t index, std::string name);
  std::chrono::milliseconds start_timeout = std::chrono::milliseconds(15000);

 private:
  std::vector<MenuRow> Rows() const;
  MenuEffect Activate(size_t row);
  void Change(size_t row, int dir);
  void Go(MenuPage p, size_t selected = 0);
  void Clamp();
  std::string* EditedText();
  size_t MaxText() const;

  LobbyService& service_;
  std::string player_;
  MenuPage page_ = MenuPage::kHome;
  size_t selected_ = 0, top_ = 0, visible_ = 6;  // rows shown at a time (the renderer sets it per page)
  bool BeginEdit();
  void Commit();
  void Cancel();
  bool editing_ = false;    // a text row is being edited (in field_; stored on Commit only)
  TextField field_;
  bool caret_on_ = true;
  std::string message_;
  // Create
  LobbySettings settings_;
  std::string difficulty_name_ = "as on the game type screen";
  // Join by code / password
  std::string code_, password_;
  LobbyEntry pending_;  // a browsed lobby waiting for its password
  // Start / join progress
  bool starting_ = false, joining_game_ = false, handed_ = false;
  std::chrono::steady_clock::time_point step_start_;
  std::string seen_error_;
};

std::string Masked(const std::string& s);

// What a row draws where (the renderer's columns): a plain action its label as the game's own button text; any
// other row its label in the fixed label column and its value in the value column (clipped to `value_chars`
// characters, '>' where cut). The label never depends on the value: typing never moves it.
struct RowText {
  std::string button, label, value;
};
RowText SplitRow(const MenuRow& row, size_t value_chars);

// The selected row's columns draw in the game's own selected-text font (the renderer gives selected text its look,
// and its animation, only to fonts without a colour of their own: the *_SEL presets); a field being typed into keeps
// its steady font so the text and caret stay readable. True: the selected font for this column.
bool SelectedLook(bool row_selected, bool editing, bool value_column);
constexpr size_t kMaxNameChars = 24, kMaxPasswordChars = 32, kMaxCodeChars = 40;
constexpr size_t kFieldChars = 28;  // shown of a field being edited (the row's width)

}  // namespace kuf::coop
