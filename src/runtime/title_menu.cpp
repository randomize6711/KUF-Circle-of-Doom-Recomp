// The title list's own rows (M7 feature 4, docs/SETTINGS.md "Title menu"): below CREDITS, "CHECK FOR UPDATES" and
// "QUIT GAME" (in capitals as the game's rows), drawn by the game as its own rows (the layout extended in memory:
// ui_layout.h AddTitleMainRows, cloned from CREDITS; the whole list moved up so that it still ends where it did).
//
// The title list (menu vtable 0x8203E658, input sub_826791D0(menu, event)): its seven row buttons at menu +64, the
// selected index at +112 (Up/Down skip unavailable rows, stop at 0 and 6), the highlight button at +92 and the row
// it is on at +96. A move places the highlight on the row (the highlight's SetRect, button vtable slot 5,
// sub_82625AC8), plays cue 4 and refreshes (menu vtable slot 3: SetFocus, sub_82622400, on the row at +96). The rows
// added here are not in that array (its next slot is the highlight): while one of them is selected the game's index
// stays on CREDITS, the highlight and the focus are on the added row, and its input (which the focused button
// forwards to this menu) is handled here.
//
//   Check for Updates  runs the installed updater's check (tools/updater/kuf_update.py through the install's
//                      kuf-update, found from $KUF_INSTALL_ROOT that the launcher sets, or --kuf_updater) on its own
//                      thread; the row shows "Checking...", then the result. When an update is available, Select
//                      installs it (the updater verifies the signature; the version in use keeps running) and the row
//                      asks for a restart. Outside an installed package: "UPDATES NOT SET UP".
//   Quit Game          asks with the game's own message box (UX fix 6): Yes quits as closing the window does
//                      (after any save in progress, with the PC settings written); No returns to the list.

#include "runtime/title_menu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "runtime/menu_pointer.h"
#include "runtime/pc_settings.h"
#include "runtime/prompt_text.h"
#include "runtime/save_groups.h"
#include "runtime/ui_layout.h"
#include "runtime/update_status.h"
#include "runtime/version.h"

REXCVAR_DEFINE_STRING(kuf_storage_prompt, "auto", "KUF/Settings",
                      "M7 UX fix 5: the console's storage device question after Press START: auto (answered Yes for "
                      "the player: the port saves in its own folder) or ask (shown, as the original)");
REXCVAR_DEFINE_BOOL(kuf_trace_title_events, false, "KUF/Debug",
                    "M7: log each event of the title menu's handler (its arguments and the event's first words)");
REXCVAR_DEFINE_BOOL(kuf_title_arrow, true, "KUF/Settings",
                    "M7 UI polish: with a controller, the selection arrow beside every title list row (the game's rows "
                    "hide it with their state bit 0x80)");
REXCVAR_DEFINE_BOOL(kuf_title_menu_rows, true, "KUF/Settings",
                    "Check for Updates and Quit Game below CREDITS in the title list (M7)");
REXCVAR_DEFINE_STRING(kuf_updater, "", "KUF/Settings",
                      "The updater command for Check for Updates (default: $KUF_UPDATER, which the launcher sets)");

REX_EXTERN(sub_82624570);         // control lookup (node, id)
REX_EXTERN(sub_82623850);         // control state flags (control, flag, on): 1 shown
REX_EXTERN(sub_82624CB8);         // set a control's text (control, string, 0)
REX_EXTERN(sub_8261B488);         // menu sound cue (menu, cue)
REX_EXTERN(sub_8218F410);         // the game's allocation
REX_EXTERN(sub_82625AC8);         // a button's SetRect (button, x, y, width, height)
REX_EXTERN(sub_82622400);         // the toolkit's SetFocus (control)
REX_EXTERN(sub_825D5560);         // the game's message post (message, data, 0, 0, 0)

namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);

enum Row { kUpdates, kQuit, kRowCount };
constexpr uint32_t kTextBase = 0xFF01;  // the rows' text numbers (the game's tables end at 1910; 0xFF00: PC Settings)
constexpr const char* kInitialLabels[kRowCount] = {"CHECK FOR UPDATES", "QUIT GAME"};
constexpr uint32_t kRows = 64, kSelected = 112, kHighlight = 92, kHighlightRow = 96, kLastRow = 6;
constexpr uint32_t kEventCode = 12;
constexpr uint32_t kCodeSelect = 0, kCodeBack = 1, kCodeUp = 6, kCodeDown = 7;
constexpr uint32_t kCueMove = 4, kCueSelect = 0xB;
constexpr uint32_t kTextSize = 96;

std::array<uint32_t, kRowCount> g_initial{};  // guest strings of the initial labels
uint32_t g_scratch = 0;                       // guest buffer for set-text (the control copies it)
bool g_extended = false;

// Game thread state of the title list.
uint32_t g_menu = 0;
int g_extra = -1;  // the added row selected (-1: the game's own rows)
// Quit Game (M7 UX fix 6): the game's message box (message 0x42205: text[1024], +1024 owner, +1028 kind (1: a
// question), +1032 / +1036 the answers the owner gets for Yes / No), as the game asks its storage question.
constexpr uint32_t kQuitYes = 0x7E51, kQuitNo = 0x7E52;  // not used by the game
constexpr uint32_t kBoxSize = 1056;
constexpr char kQuitQuestion[] = "Quit Game?\\nAre you sure you want to quit the game?";  // the game's "\n"
constexpr uint32_t kMessageBox = 0x42205;  // the game's message box message
uint32_t g_box = 0;          // the guest message data
bool g_quit_pending = false;  // Yes: quit once no save is in progress

// The updater (M7: tools/updater/kuf_update.py, run in its own thread) and the label each row should show.
enum class Update { kIdle, kChecking, kUpToDate, kAvailable, kDownloading, kInstalled, kFailed, kNotSetUp };
std::mutex g_mutex;
Update g_update = Update::kIdle;
kuf::updates::Reply g_reply;                  // the last check or install result (g_mutex)
std::optional<kuf::updates::Dialog> g_dialog_pending;  // to show when the title list has the focus (g_mutex)
std::optional<kuf::updates::Dialog> g_dialog_open;     // the one on screen (game thread)
pid_t g_update_pid = 0;                       // the running updater (g_mutex): Cancel sends it SIGTERM
constexpr uint32_t kUpdateYes = 0x7E53, kUpdateNo = 0x7E54;  // the update box's answers (not used by the game)
std::array<std::string, kRowCount> g_want;   // the label wanted (g_mutex)
std::array<std::string, kRowCount> g_shown;  // the label set (game thread)

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFn fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0,
              uint32_t r6 = 0, uint32_t r7 = 0) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  fn(ctx, base);
  return ctx.r3.u32;
}

uint32_t GuestString(PPCContext& ctx, uint8_t* base, const std::string& text, uint32_t size) {
  ctx.r3.u64 = size;
  sub_8218F410(ctx, base);
  const uint32_t at = ctx.r3.u32;
  if (at) {
    const size_t n = std::min<size_t>(text.size(), size - 1);
    std::memcpy(base + at, text.data(), n);
    base[at + n] = 0;
  }
  return at;
}

void SetWanted(Row row, std::string label) {
  std::lock_guard lock(g_mutex);
  g_want[row] = std::move(label);
}

std::string UpdaterCommand() {
  std::string cmd = REXCVAR_GET(kuf_updater);
  if (cmd.empty()) {
    if (const char* env = std::getenv("KUF_UPDATER")) cmd = env;
  }
  return cmd;
}

// Runs the updater (its command, then " --json " and the arguments) with its standard output read line by line
// (each JSON line to `on_line`); its process ID is kept while it runs (Cancel). Returns its exit status (-1: not run).
int RunUpdater(const std::string& cmd, const std::string& args,
               const std::function<void(const kuf::updates::Reply&)>& on_line, kuf::updates::Reply* result) {
  std::string quoted = "exec '";
  for (char c : cmd) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
  quoted += "' --json " + args + " 2>/dev/null";
  int fds[2];
  if (pipe(fds) != 0) return -1;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addclose(&actions, fds[1]);
  pid_t pid = 0;
  const char* argv[] = {"/bin/sh", "-c", quoted.c_str(), nullptr};
  const int spawned = posix_spawn(&pid, "/bin/sh", &actions, nullptr, const_cast<char* const*>(argv), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (spawned != 0) {
    close(fds[0]);
    return -1;
  }
  {
    std::lock_guard lock(g_mutex);
    g_update_pid = pid;
  }
  FILE* out = fdopen(fds[0], "r");
  char buf[8192];
  while (out && std::fgets(buf, sizeof(buf), out)) {
    if (const auto reply = kuf::updates::ParseLine(buf)) {
      if (reply->exit >= 0) {
        *result = *reply;
      } else if (on_line) {
        on_line(*reply);
      }
    }
  }
  if (out) fclose(out);
  int status = 0;
  waitpid(pid, &status, 0);
  {
    std::lock_guard lock(g_mutex);
    g_update_pid = 0;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void StartCheck() {
  const std::string cmd = UpdaterCommand();
  {
    std::lock_guard lock(g_mutex);
    if (g_update == Update::kChecking || g_update == Update::kDownloading) return;
    if (cmd.empty()) {
      g_update = Update::kNotSetUp;
      g_want[kUpdates] = "UPDATES NOT SET UP";
      g_dialog_pending = kuf::updates::Dialog::kNotSetUp;
      REXLOG_INFO("[kuf-title] update check: no updater (not an installed package)");
      return;
    }
    g_update = Update::kChecking;
    g_want[kUpdates] = "CHECKING FOR UPDATES...";
  }
  REXLOG_INFO("[kuf-title] update check started (version {})", kuf::version::String());
  std::thread([cmd] {
    kuf::updates::Reply r;
    const int code = RunUpdater(cmd, "check", nullptr, &r);
    std::lock_guard lock(g_mutex);
    g_reply = r;
    using kuf::updates::Dialog;
    if (r.exit == 0 && r.update) {
      g_update = Update::kAvailable;
      g_want[kUpdates] = "UPDATE " + r.latest + " AVAILABLE";
      g_dialog_pending = Dialog::kOffer;
    } else if (r.exit == 3) {
      g_update = Update::kUpToDate;
      g_want[kUpdates] = "UP TO DATE";
      g_dialog_pending = Dialog::kUpToDate;
    } else {
      g_update = Update::kFailed;
      g_want[kUpdates] = "UPDATE CHECK FAILED";
      if (r.error.empty()) r.error = code < 0 ? "The updater could not be started." : "The updater gave no result.";
      g_reply = r;
      g_dialog_pending = Dialog::kFailed;
    }
    REXLOG_INFO("[kuf-title] update check: exit {} (updater {}), installed '{}', latest '{}' {}{}", r.exit, code,
                r.current, r.latest, r.published, r.error.empty() ? "" : ", " + r.error);
  }).detach();
}

void StartInstall() {
  const std::string cmd = UpdaterCommand();
  std::string latest;
  {
    std::lock_guard lock(g_mutex);
    if (g_update != Update::kAvailable || cmd.empty()) return;
    g_update = Update::kDownloading;
    latest = g_reply.latest;
    g_want[kUpdates] = "DOWNLOADING...";
  }
  REXLOG_INFO("[kuf-title] installing update {}", latest);
  std::thread([cmd, latest] {
    kuf::updates::Reply r;
    const int code = RunUpdater(cmd, "update --yes", [](const kuf::updates::Reply& p) {
      std::lock_guard lock(g_mutex);
      g_want[kUpdates] = kuf::updates::ProgressLabel(p);
    }, &r);
    std::lock_guard lock(g_mutex);
    using kuf::updates::Dialog;
    r.latest = latest;
    g_reply = r;
    if (r.exit == 0) {
      g_update = Update::kInstalled;
      g_want[kUpdates] = "UPDATE " + latest + " INSTALLED";
      g_dialog_pending = std::getenv("KUF_RESTART_FILE") ? Dialog::kRestart : Dialog::kInstalledNoRestart;
    } else if (r.exit == 5) {
      g_update = Update::kAvailable;  // still available: Select offers it again
      g_want[kUpdates] = "UPDATE " + latest + " AVAILABLE";
      g_dialog_pending = Dialog::kCancelled;
    } else {
      g_update = Update::kFailed;
      g_want[kUpdates] = "UPDATE NOT INSTALLED";
      if (r.error.empty()) r.error = "The updater stopped without a result.";
      g_reply = r;
      g_dialog_pending = Dialog::kFailed;
    }
    REXLOG_INFO("[kuf-title] update install: exit {} (updater {}){}", r.exit, code,
                r.error.empty() ? "" : ", " + r.error);
  }).detach();
}

void CancelInstall() {
  std::lock_guard lock(g_mutex);
  if (g_update == Update::kDownloading && g_update_pid > 0) {
    kill(g_update_pid, SIGTERM);  // the updater stops before it switches anything (exit 5)
    REXLOG_INFO("[kuf-title] update download: cancelling");
  }
}

// The update box: the game's own message box (as Quit Game), its text from update_status.cpp with the prompts of
// the device in use; Yes / No come back as kUpdateYes / kUpdateNo (BeforePost).
void AskUpdate(PPCContext& ctx, uint8_t* base, uint32_t menu, kuf::updates::Dialog dialog) {
  kuf::updates::Reply reply;
  {
    std::lock_guard lock(g_mutex);
    reply = g_reply;
  }
  const std::string text =
      kuf::prompts::ForCurrentDevice(kuf::updates::DialogText(dialog, reply, kuf::version::String()));
  if (!g_box) g_box = GuestString(ctx, base, "", kBoxSize);
  if (!g_box) return;
  std::memset(base + g_box, 0, kBoxSize);
  std::memcpy(base + g_box, text.data(), std::min<size_t>(text.size(), 1023));
  Store32(base, g_box + 1024, menu);
  Store32(base, g_box + 1028, 1);
  Store32(base, g_box + 1032, kUpdateYes);
  Store32(base, g_box + 1036, kUpdateNo);
  g_dialog_open = dialog;
  Call(ctx, base, sub_825D5560, kMessageBox, g_box);
  REXLOG_INFO("[kuf-title] update dialog {}", int(dialog));
}

void RestartGame() {
  const char* marker = std::getenv("KUF_RESTART_FILE");
  if (!marker) return;
  if (FILE* f = std::fopen(marker, "w")) {
    std::fputs("restart after update\n", f);
    std::fclose(f);
    REXLOG_INFO("[kuf-title] restarting for the update");
    g_quit_pending = true;  // as Quit Game: once no save is in progress, PC settings written, then the launcher
  }
}

uint32_t Root(const uint8_t* base, uint32_t menu) {  // the title list's root node (from a row's scene record)
  const uint32_t row = Load32(base, menu + kRows);
  const uint32_t scene = row ? Load32(base, row + 48) : 0;
  return scene ? Load32(base, scene + 8) : 0;
}

uint32_t AddedButton(PPCContext& ctx, uint8_t* base, uint32_t menu, int row) {
  const uint32_t root = Root(base, menu);
  return root ? Call(ctx, base, sub_82624570, root, kuf::ui::kTitleMainRowButton + uint32_t(row)) : 0;
}

// The highlight and the focus on a row button (the game's own: CREDITS; or one of the added ones).
void HighlightOn(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t button) {
  const uint32_t highlight = Load32(base, menu + kHighlight);
  if (!button || !highlight) return;
  Store32(base, menu + kHighlightRow, button);
  Call(ctx, base, sub_82625AC8, highlight, Load32(base, highlight + 16), Load32(base, button + 20),
       Load32(base, highlight + 24), Load32(base, highlight + 28));
  Call(ctx, base, sub_82622400, button);
}

void Select(PPCContext& ctx, uint8_t* base, uint32_t menu, int row) {
  g_extra = row;
  if (row < 0) {
    HighlightOn(ctx, base, menu, Load32(base, menu + kRows + 4 * kLastRow));
  } else {
    HighlightOn(ctx, base, menu, AddedButton(ctx, base, menu, row));
  }
  Call(ctx, base, sub_8261B488, menu, kCueMove);
}

void AskQuit(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  if (!g_box) g_box = GuestString(ctx, base, "", kBoxSize);
  if (!g_box) return;
  std::memset(base + g_box, 0, kBoxSize);
  std::memcpy(base + g_box, kQuitQuestion, sizeof(kQuitQuestion));
  Store32(base, g_box + 1024, menu);
  Store32(base, g_box + 1028, 1);
  Store32(base, g_box + 1032, kQuitYes);
  Store32(base, g_box + 1036, kQuitNo);
  Call(ctx, base, sub_825D5560, kMessageBox, g_box);
  REXLOG_INFO("[kuf-title] Quit Game: asking");
}

void ApplyLabels(PPCContext& ctx, uint8_t* base) {
  if (!g_menu || !g_scratch) return;
  std::array<std::string, kRowCount> want;
  {
    std::lock_guard lock(g_mutex);
    want = g_want;
  }
  for (int row = 0; row < kRowCount; ++row) {
    if (want[row].empty() || want[row] == g_shown[row]) continue;
    const uint32_t button = AddedButton(ctx, base, g_menu, row);
    if (!button) return;  // the list is not built (any more)
    const std::string text = "@(offsety=-2)" + want[row];
    const size_t n = std::min<size_t>(text.size(), kTextSize - 1);
    std::memcpy(base + g_scratch, text.data(), n);
    base[g_scratch + n] = 0;
    Call(ctx, base, sub_82624CB8, button, g_scratch, 0);
    g_shown[row] = want[row];
  }
}

}  // namespace

namespace kuf::title_menu {

bool ExtendLayout(PPCContext& ctx, uint8_t* base, std::vector<uint8_t>* layout, std::string* error) {
  if (!REXCVAR_GET(kuf_title_menu_rows)) return false;
  auto extended = kuf::ui::AddTitleMainRows(*layout, {kTextBase + kUpdates, kTextBase + kQuit}, error);
  if (!extended) return false;
  if (!g_extended) {  // once; kept for the whole run
    for (int row = 0; row < kRowCount; ++row) {
      g_initial[row] = GuestString(ctx, base, std::string("@(offsety=-2)") + kInitialLabels[row], 64);
    }
    g_scratch = GuestString(ctx, base, "", kTextSize);
    g_extended = true;
  }
  *layout = std::move(*extended);
  return true;
}

uint32_t TextOverride(uint32_t number) {
  if (!g_extended || number < kTextBase || number >= kTextBase + kRowCount) return 0;
  return g_initial[number - kTextBase];
}

namespace {
constexpr uint32_t kStorageYes = 2003;         // the storage prompt's answers (only its two call sites use them)
constexpr uint32_t kMessageBoxScene = 0x02500000;
constexpr uint32_t kTitleListScene = 0x03900000;  // the title list (NEW GAME ... QUIT GAME)
std::atomic<uint32_t> g_title_object{0};          // the title menu object, from its event handler
constexpr uint32_t kFocusedControl = 0x82ABBB90;
std::atomic<int> g_answer_storage{0};  // ticks left to answer the storage prompt (0: none pending)
constexpr uint32_t kNoticeBox = 0x42208;   // a one-button notice (the storage warning after the question)
constexpr uint32_t kBoxAnswered = 0x42206;  // a message box answered (data: the box)
// sub_8267ABD8's two posts of the storage notice (text 1776; it also posts texts 1781 and 1872, left alone).
constexpr uint32_t kStorageNoticePosts[] = {0x8267AE40, 0x8267B0A4};

// The focused control's scene ID (its scene record +0), or 0.
uint32_t FocusedSceneId(const uint8_t* base) {
  const uint32_t focus = Load32(base, kFocusedControl);
  if (focus < 0x10000 || focus >= 0xF0000000) return 0;
  const uint32_t record = Load32(base, focus + 48);
  return record >= 0x10000 && record < 0xF0000000 ? Load32(base, record) : 0;
}
}  // namespace

bool BeforeMenuEvent(PPCContext& ctx, uint8_t* base) {
  if (ctx.r3.u32 >= 0x10000) g_title_object = ctx.r3.u32;  // the title menu (its list's rows at +64)
  if (REXCVAR_GET(kuf_trace_title_events)) {
    std::string words;
    for (const uint32_t r : {ctx.r4.u32, ctx.r5.u32}) {
      if (r < 0x10000 || r >= 0xF0000000) continue;
      words += fmt::format(" [{:08X}:", r);
      for (uint32_t i = 0; i < 6; ++i) words += fmt::format(" {:08X}", Load32(base, r + 4 * i));
      words += "]";
    }
    REXLOG_INFO("[kuf-title] event r3={:08X} r4={:08X} r5={:08X} r6={:08X}{}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, words);
  }
  return false;
}

bool BeforePost(PPCContext& ctx, uint8_t* base) {
  const uint32_t message = ctx.r3.u32, data = ctx.r4.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (message == kNoticeBox && REXCVAR_GET(kuf_storage_prompt) == "auto" &&
      std::find(std::begin(kStorageNoticePosts), std::end(kStorageNoticePosts), lr) != std::end(kStorageNoticePosts)) {
    // The title's warning that progress is lost if the storage device is removed (text 1776, after the storage
    // question and again whenever the title is entered): a console notice with no owner and no answer anyone waits
    // for (owner 0, kind 0). Not posted at all (M7 UI follow-up: confirming it a tick later still drew it for 1-2
    // ticks, the player's "popup flash" on Start); the game's message buffers are a ring (nothing to free).
    REXLOG_INFO("[kuf-title] storage notice: not shown (lr {:08X})", lr);
    return true;
  }
  if (message == kBoxAnswered && data >= 0x10000 && data < 0xF0000000) {
    // The box hands the answer to its owner (sub_826601E8: the answer at box +148, A -> +136, B -> +140; the owner at
    // +152). The Quit Game box: Yes quits (OnTick), No only closes it.
    const uint32_t answer = Load32(base, data + 148);
    if ((answer == kUpdateYes || answer == kUpdateNo) && g_dialog_open) {
      using kuf::updates::Dialog;
      const Dialog dialog = *g_dialog_open;
      g_dialog_open.reset();
      const bool yes = answer == kUpdateYes;
      REXLOG_INFO("[kuf-title] update dialog {}: {}", int(dialog), yes ? "Yes" : "No");
      if (yes && dialog == Dialog::kOffer) StartInstall();
      if (yes && dialog == Dialog::kCancelAsk) CancelInstall();
      if (yes && dialog == Dialog::kRestart) RestartGame();
      return false;
    }
    if (answer == kQuitYes) {
      REXLOG_INFO("[kuf-title] Quit Game: Yes");
      g_quit_pending = true;
    } else if (answer == kQuitNo) {
      REXLOG_INFO("[kuf-title] Quit Game: No");
    }
    return false;
  }
  if (message != kMessageBox || data < 0x10000 || data >= 0xF0000000) return false;
  if (REXCVAR_GET(kuf_trace_title_events)) {
    REXLOG_INFO("[kuf-title] message box posted (lr {:08X}): owner {:08X} kind {} answers {:08X} {:08X} {:08X} {:08X}",
                static_cast<uint32_t>(ctx.lr), Load32(base, data + 1024), Load32(base, data + 1028),
                Load32(base, data + 1032), Load32(base, data + 1036), Load32(base, data + 1040),
                Load32(base, data + 1044));
  }
  if (Load32(base, data + 1032) == kStorageYes && REXCVAR_GET(kuf_storage_prompt) == "auto") {
    g_answer_storage = 90;  // up to 3 s for the box to take the focus
    REXLOG_INFO("[kuf-title] storage prompt: answering Yes (the PC port saves in its own folder)");
  }
  return false;
}

void OnUiUpdate(PPCContext& ctx, uint8_t* base) {
  // The storage question (posted by the game: its Yes selects the save device, which the title and the saves need):
  // answered in the UI update of the tick its box took the focus, before that tick is drawn, with the box hidden, so
  // it is never seen (M7 UI follow-up: answered at the next tick's start it was drawn for a tick).
  if (g_answer_storage <= 0 || FocusedSceneId(base) != kMessageBoxScene) return;
  const uint32_t focus = Load32(base, kFocusedControl);
  const uint32_t record = Load32(base, focus + 48);
  const uint32_t root = record >= 0x10000 && record < 0xF0000000 ? Load32(base, record + 8) : 0;
  if (root >= 0x10000 && root < 0xF0000000) {
    ctx.r3.u64 = root;
    ctx.r4.u64 = 1;
    ctx.r5.u64 = 0;
    sub_82623850(ctx, base);  // the box hidden for its last tick
  }
  g_answer_storage = 0;
  kuf::menu_pointer::Dispatch(ctx, base, kCodeSelect);
  REXLOG_INFO("[kuf-title] storage prompt answered (before it was drawn)");
}

void OnTick(PPCContext& ctx, uint8_t* base) {
  if (g_extended) ApplyLabels(ctx, base);
  if (g_menu && !g_dialog_open && FocusedSceneId(base) == kTitleListScene) {  // an updater result to show
    std::optional<kuf::updates::Dialog> dialog;
    {
      std::lock_guard lock(g_mutex);
      dialog.swap(g_dialog_pending);
    }
    if (dialog) AskUpdate(ctx, base, g_menu, *dialog);
  }
  if (g_quit_pending && !kuf::saves::SaveInProgress()) {  // as closing the window; PC settings written first
    g_quit_pending = false;
    kuf::pc::SaveIfChanged();
    REXLOG_INFO("[kuf-title] Quit Game");
    if (!kuf::app::RequestQuit()) REXLOG_WARN("[kuf-title] no quit handler");
  }
  if (g_answer_storage > 0) {  // the box has the focus: Select (its A) answers Yes
    if (FocusedSceneId(base) == kMessageBoxScene) {
      g_answer_storage = 0;
      kuf::menu_pointer::Dispatch(ctx, base, kCodeSelect);
      REXLOG_INFO("[kuf-title] storage prompt answered");
    } else if (--g_answer_storage == 0) {
      REXLOG_WARN("[kuf-title] storage prompt: the box never took the focus (not answered)");
    }
  }
}

}  // namespace kuf::title_menu

namespace kuf::app {

namespace {
std::mutex g_quit_mutex;
std::function<void()> g_quit;
}  // namespace

void SetQuitHandler(std::function<void()> handler) {
  std::lock_guard lock(g_quit_mutex);
  g_quit = std::move(handler);
}

bool RequestQuit() {
  std::function<void()> quit;
  {
    std::lock_guard lock(g_quit_mutex);
    quit = g_quit;
  }
  if (!quit) return false;
  quit();
  return true;
}

}  // namespace kuf::app

namespace kuf::title_menu {

uint32_t Menu() { return g_menu; }

bool BeforeInput(PPCContext& ctx, uint8_t* base) {
  const uint32_t menu = ctx.r3.u32, event = ctx.r4.u32;
  if (!g_extended || menu < 0x10000 || event < 0x10000) {
    return false;
  }
  if (menu != g_menu) {  // a new title list: its labels as built
    g_menu = menu;
    g_extra = -1;
    g_shown = {};
    std::lock_guard lock(g_mutex);
    for (int row = 0; row < kRowCount; ++row) g_want[row] = kInitialLabels[row];
    if (g_update != Update::kIdle && g_update != Update::kChecking && g_update != Update::kDownloading &&
        g_update != Update::kInstalled) {
      g_update = Update::kIdle;  // a finished check is shown again only when asked again
    }
  }
  const uint32_t code = Load32(base, event + kEventCode);
  if (g_extra < 0) {
    if (code == kCodeDown && Load32(base, menu + kSelected) == kLastRow && AddedButton(ctx, base, menu, 0)) {
      Select(ctx, base, menu, 0);
      ctx.r3.u64 = 0;
      return true;
    }
    return false;
  }
  switch (code) {
    case kCodeUp:
      Select(ctx, base, menu, g_extra - 1);
      break;
    case kCodeDown:
      if (g_extra + 1 < kRowCount) Select(ctx, base, menu, g_extra + 1);
      break;
    case kCodeSelect:
      Call(ctx, base, sub_8261B488, menu, kCueSelect);
      if (g_extra == kUpdates) {
        Update state;
        {
          std::lock_guard lock(g_mutex);
          state = g_update;
        }
        using kuf::updates::Dialog;
        if (state == Update::kAvailable) {
          AskUpdate(ctx, base, menu, Dialog::kOffer);  // the release again (Install or Close)
        } else if (state == Update::kDownloading) {
          AskUpdate(ctx, base, menu, Dialog::kCancelAsk);
        } else if (state == Update::kInstalled) {
          AskUpdate(ctx, base, menu,
                    std::getenv("KUF_RESTART_FILE") ? Dialog::kRestart : Dialog::kInstalledNoRestart);
        } else if (state != Update::kChecking) {
          StartCheck();
        }
      } else if (g_extra == kQuit) {
        AskQuit(ctx, base, menu);  // the game's own Yes / No message box (UX fix 6)
      }
      ApplyLabels(ctx, base);
      break;
    case kCodeBack:
      // As from CREDITS: the game's own Back (the selection back on CREDITS first).
      Select(ctx, base, menu, -1);
      ctx.r3.u64 = menu;
      ctx.r4.u64 = event;
      return false;
    default:
      break;
  }
  ctx.r3.u64 = 0;
  return true;
}

}  // namespace kuf::title_menu

// The game type screen's co-op gate (M7 UI follow-up; KNOWN_ISSUES K62). sub_82669770 checks the Xbox LIVE
// multiplayer privilege (254, through the XDK's XUserCheckPrivilege wrapper) and then a sign-in bit; when either
// fails it posts the console's message box (text 1687 "You cannot play co-op because you do not have a Xbox LIVE Gold
// Membership ...", or 1730) and returns 0, and its callers grey the four online co-op rows (Host Private, Host
// Public, Quick Match, Co-op Lobby) and abort any co-op action. The port has no online co-op (docs/NETWORKING.md), so
// the gate answers "not available" without asking the console's account services and without the box: the rows stay
// unavailable (the same callers, and the screen's own poll, sub_82668698, which never posts), nothing pretends to be
// signed in to Xbox LIVE.
REXCVAR_DEFINE_BOOL(kuf_xbox_live_notice, false, "KUF/Settings",
                    "M7 UI follow-up: the console's co-op check with its Xbox LIVE Gold message box (false: co-op "
                    "rows unavailable without the message; online co-op is not implemented)");
REX_EXTERN(__imp__sub_82669770);
REX_HOOK_RAW(sub_82669770) {  // the co-op gate (no arguments; returns 1 if co-op may be used)
  if (REXCVAR_GET(kuf_xbox_live_notice)) {
    __imp__sub_82669770(ctx, base);
    return;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[kuf-title] co-op: not available in the PC port (the Xbox LIVE check and its message skipped)");
  }
  ctx.r3.u64 = 0;
}
