// The graphical setup of Kingdom Under Fire: Circle of Doom (installer/README.md): a wizard in GTK 3 with the
// project's own theme (assets/theme.css, the X360 font, original artwork; no animations). Steps: welcome, the player's game files,
// the installation folder, the shortcut options, the installation (a worker thread; core/install.cpp), the end. Run
// again over an installation (or with --uninstall FOLDER) it offers repair and removal.
//
//   kuf-setup [--payload DIR] [--assets DIR] [--uninstall FOLDER] [--screenshot DIR]
//
// --payload: the game package to install (default: ../share/kuf-setup/payload next to the program, as in the
// AppImage); --assets: theme, font, images, the game-file list; --screenshot: draws each page into DIR/*.png and
// exits (a check of the look without a person; under GDK_BACKEND=broadway it needs no screen).

#include <gtk/gtk.h>

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "core/game_data.h"
#include "core/install.h"
#include "core/steam.h"
#include "core/util.h"

namespace setup = kuf::setup;
namespace fs = std::filesystem;

namespace {

enum Page { kWelcome, kGame, kLocation, kOptions, kInstalling, kFinish, kMaintenance, kPageCount };
constexpr const char* kPageIds[] = {"welcome", "game", "location", "options", "installing", "finish", "maintenance"};
constexpr const char* kStepNames[] = {"Welcome", "Game Files", "Location", "Options", "Install", "Finish"};

struct App {
  // Paths
  fs::path exe, assets, payload, signers;
  setup::GameManifest manifest;
  std::string package_version;
  std::vector<std::string> compat;  // what this system lacks for the game (empty: compatible)
  GtkWidget* welcome_warning = nullptr;
  // Widgets
  GtkWidget* window = nullptr;
  GtkWidget* stack = nullptr;
  GtkWidget* back = nullptr;
  GtkWidget* next = nullptr;
  GtkWidget* steps[6] = {};
  // Game files
  GtkWidget* game_entry = nullptr;
  GtkWidget* game_status = nullptr;
  GtkWidget* game_details = nullptr;
  bool game_ok = false;
  fs::path game_dir;
  // Location
  GtkWidget* root_entry = nullptr;
  GtkWidget* root_status = nullptr;
  setup::Target target = setup::Target::kInvalid;
  // Options
  GtkWidget* opt_menu = nullptr;
  GtkWidget* opt_desktop = nullptr;
  GtkWidget* opt_steam = nullptr;
  GtkWidget* steam_note = nullptr;
  GtkWidget* steam_profile = nullptr;
  std::vector<setup::steam::Profile> profiles;
  // Installing
  GtkWidget* stage_label = nullptr;
  GtkWidget* progress = nullptr;
  GtkWidget* detail_label = nullptr;
  std::atomic<bool> cancel{false};
  bool running = false;
  // Finish
  GtkWidget* finish_title = nullptr;
  GtkWidget* finish_text = nullptr;
  GtkWidget* finish_notes = nullptr;
  GtkWidget* launch = nullptr;
  bool installed_ok = false;
  // Maintenance
  fs::path existing;
  GtkWidget* maint_text = nullptr;
  GtkWidget* rm_shortcuts = nullptr;
  GtkWidget* rm_steam = nullptr;
  GtkWidget* rm_data = nullptr;
  GtkWidget* repair_button = nullptr;
  GtkWidget* uninstall_button = nullptr;
  bool uninstall_mode = false;  // the install page shows an uninstallation
  Page page = kWelcome;
};

App* g_app = nullptr;

// ---- small widget helpers -----------------------------------------------------------------------------------------

GtkWidget* Label(const std::string& text, const char* css_class, bool wrap = true, float xalign = 0) {
  GtkWidget* l = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(l), text.c_str());
  gtk_label_set_xalign(GTK_LABEL(l), xalign);
  gtk_label_set_line_wrap(GTK_LABEL(l), wrap);
  gtk_label_set_max_width_chars(GTK_LABEL(l), 80);
  if (css_class) gtk_style_context_add_class(gtk_widget_get_style_context(l), css_class);
  return l;
}

void AddClass(GtkWidget* w, const char* c) { gtk_style_context_add_class(gtk_widget_get_style_context(w), c); }
void RemoveClass(GtkWidget* w, const char* c) { gtk_style_context_remove_class(gtk_widget_get_style_context(w), c); }

std::string Esc(const std::string& s) {
  gchar* e = g_markup_escape_text(s.c_str(), -1);
  std::string out = e;
  g_free(e);
  return out;
}

// An image from the assets at `size` logical pixels, sharp on high-DPI screens.
GtkWidget* Picture(const fs::path& file, int width, int height) {
  GtkWidget* image = gtk_image_new();
  const int scale = 2;  // drawn at twice the size, shown at the logical size
  GError* err = nullptr;
  GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(file.c_str(), width * scale, height * scale, TRUE, &err);
  if (pb) {
    cairo_surface_t* s = gdk_cairo_surface_create_from_pixbuf(pb, scale, nullptr);
    gtk_image_set_from_surface(GTK_IMAGE(image), s);
    cairo_surface_destroy(s);
    g_object_unref(pb);
  } else if (err) {
    g_error_free(err);
  }
  return image;
}

GtkWidget* Divider(App* a, GtkAlign align = GTK_ALIGN_CENTER) {
  GtkWidget* d = Picture(a->assets / "divider.png", 420, 21);
  gtk_widget_set_halign(d, align);
  return d;
}

GtkWidget* PageBox(const std::string& title) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_margin_start(box, 56);
  gtk_widget_set_margin_end(box, 56);
  gtk_widget_set_margin_top(box, 10);
  if (!title.empty()) {
    gtk_box_pack_start(GTK_BOX(box), Label(Esc(title), "page-title", false), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), Divider(g_app, GTK_ALIGN_START), FALSE, FALSE, 0);
  }
  return box;
}

GtkWidget* Panel(GtkWidget* child) {
  GtkWidget* frame = gtk_event_box_new();
  AddClass(frame, "panel");
  gtk_widget_set_margin_start(child, 18);  // an event box has no CSS padding: the child's margins instead
  gtk_widget_set_margin_end(child, 18);
  gtk_widget_set_margin_top(child, 14);
  gtk_widget_set_margin_bottom(child, 14);
  gtk_container_add(GTK_CONTAINER(frame), child);
  return frame;
}

void Message(App* a, GtkMessageType type, const std::string& title, const std::string& text) {
  GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(a->window), GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s",
                                        title.c_str());
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text.c_str());
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}

// ---- navigation ---------------------------------------------------------------------------------------------------

void UpdateButtons(App* a);

void ShowPage(App* a, Page p) {
  a->page = p;
  gtk_stack_set_visible_child_name(GTK_STACK(a->stack), kPageIds[p]);
  const int step = p == kMaintenance ? -1 : int(p);
  for (int i = 0; i < 6; ++i) {
    RemoveClass(a->steps[i], "current");
    RemoveClass(a->steps[i], "done");
    if (i == step) AddClass(a->steps[i], "current");
    if (step >= 0 && i < step) AddClass(a->steps[i], "done");
  }
  UpdateButtons(a);
}

void UpdateButtons(App* a) {
  const Page p = a->page;
  gtk_widget_set_visible(a->back, p == kGame || p == kLocation || p == kOptions);
  gtk_widget_set_visible(a->next, p != kMaintenance);
  RemoveClass(a->next, "primary");
  bool enabled = true;
  const char* text = "Continue";
  switch (p) {
    case kWelcome: enabled = a->compat.empty(); break;
    case kGame: enabled = a->game_ok; break;
    case kLocation: enabled = a->target == setup::Target::kNew || a->target == setup::Target::kExisting; break;
    case kOptions: {
      text = a->target == setup::Target::kExisting ? "Repair" : "Install";
      AddClass(a->next, "primary");
      const bool steam = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->opt_steam));
      if (steam && a->profiles.size() > 1) {
        enabled = gtk_combo_box_get_active(GTK_COMBO_BOX(a->steam_profile)) >= 0;  // the player chooses: no guess
      }
      break;
    }
    case kInstalling: text = "Cancel"; enabled = a->running && !a->uninstall_mode; break;
    case kFinish: text = "Finish"; AddClass(a->next, "primary"); break;
    default: break;
  }
  gtk_button_set_label(GTK_BUTTON(a->next), text);
  gtk_widget_set_sensitive(a->next, enabled);
  if (enabled) gtk_widget_grab_default(a->next);
}

// ---- game files ---------------------------------------------------------------------------------------------------

void CheckGame(App* a) {
  const std::string text = gtk_entry_get_text(GTK_ENTRY(a->game_entry));
  a->game_ok = false;
  if (text.empty()) {
    gtk_label_set_markup(GTK_LABEL(a->game_status), "Choose the folder that contains <b>default.xex</b>.");
    gtk_label_set_text(GTK_LABEL(a->game_details), "");
    UpdateButtons(a);
    return;
  }
  a->game_dir = setup::FindGameRoot(text);
  const setup::GameCheck c = setup::CheckGameFolder(a->game_dir, a->manifest);
  std::string status, details;
  if (!c.folder_ok) {
    status = "<span foreground='#ff8a6a'>✗ This folder cannot be read.</span>";
  } else if (!c.executable_found) {
    status = "<span foreground='#ff8a6a'>✗ No game here:</span> default.xex was not found in this folder.";
  } else if (!c.executable_known) {
    status = "<span foreground='#ff8a6a'>✗ Unsupported game version.</span> This default.xex is not the supported "
             "release (the original Xbox 360 game, base version). Title updates and other versions are not supported.";
  } else if (!c.missing.empty() || !c.wrong_size.empty()) {
    status = "<span foreground='#ff8a6a'>✗ Game files are missing or damaged</span> (" +
             std::to_string(c.missing.size()) + " missing, " + std::to_string(c.wrong_size.size()) +
             " with the wrong size).";
    int shown = 0;
    for (const auto& m : c.missing) {
      if (shown++ < 6) details += "Missing: " + m + "\n";
    }
    for (const auto& m : c.wrong_size) {
      if (shown++ < 6) details += "Wrong size: " + m + "\n";
    }
    if (shown > 6) details += "… and " + std::to_string(shown - 6) + " more";
  } else {
    a->game_ok = true;
    status = "<span foreground='#9fd38a'>✓ Compatible game files found.</span>";
    details = std::to_string(a->manifest.files.size()) + " files, " + setup::HumanBytes(c.bytes) + " — " +
              a->manifest.name + ".\nEach file is checked against the supported version while it is copied; your "
              "folder is only read, never changed.";
  }
  gtk_label_set_markup(GTK_LABEL(a->game_status), status.c_str());
  gtk_label_set_text(GTK_LABEL(a->game_details), details.c_str());
  UpdateButtons(a);
}

void ChooseFolder(App* a, GtkWidget* entry, const char* title) {
  GtkWidget* d = gtk_file_chooser_dialog_new(title, GTK_WINDOW(a->window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                             "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, nullptr);
  const std::string current = gtk_entry_get_text(GTK_ENTRY(entry));
  const fs::path start = current.empty() ? setup::HomeDir() : setup::ExistingAncestor(current);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), start.c_str());
  if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
    gchar* f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
    if (f) {
      gtk_entry_set_text(GTK_ENTRY(entry), f);
      g_free(f);
    }
  }
  gtk_widget_destroy(d);
}

// ---- location -----------------------------------------------------------------------------------------------------

uint64_t PackageBytes(App* a) {
  uint64_t n = 0;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(a->payload, ec)) {
    if (e.is_regular_file(ec)) n += e.file_size(ec);
  }
  return n;
}

void CheckLocation(App* a) {
  const std::string text = gtk_entry_get_text(GTK_ENTRY(a->root_entry));
  fs::path root = text;
  if (!text.empty() && text[0] == '~') root = setup::HomeDir() / fs::path(text.substr(1)).relative_path();
  a->target = setup::CheckTarget(root);
  const uint64_t needed = PackageBytes(a) + a->manifest.total + (uint64_t(64) << 20);
  const uint64_t free = setup::FreeBytes(root);
  std::string status;
  switch (a->target) {
    case setup::Target::kNew:
      if (free < needed) {
        a->target = setup::Target::kNotWritable;
        status = "<span foreground='#ff8a6a'>✗ Not enough free space</span> on this drive: " +
                 setup::HumanBytes(needed) + " needed, " + setup::HumanBytes(free) + " free.";
      } else {
        status = "<span foreground='#9fd38a'>✓ The game will be installed here.</span>  Space needed: " +
                 setup::HumanBytes(needed) + " (" + setup::HumanBytes(free) + " free).";
      }
      break;
    case setup::Target::kExisting: {
      const auto rec = setup::ReadRecord(root);
      status = "<span foreground='#f0c060'>⚠ The game is already installed here</span> (version " +
               Esc(rec ? rec->version : "?") + "). Continuing repairs it: the program and any damaged game files are "
               "replaced; your saves and settings are kept.";
      break;
    }
    case setup::Target::kNotEmpty:
      status = "<span foreground='#ff8a6a'>✗ This folder already contains other files.</span> Choose an empty folder, "
               "or a new one inside it (for example …/Kingdom Under Fire - Circle of Doom).";
      break;
    case setup::Target::kNotWritable:
      status = "<span foreground='#ff8a6a'>✗ You cannot write to this folder.</span> Choose a folder in your home "
               "folder (no administrator rights are needed or used).";
      break;
    default:
      status = "<span foreground='#ff8a6a'>✗ This folder cannot be used</span> (choose a full path, not your home "
               "folder itself).";
  }
  gtk_label_set_markup(GTK_LABEL(a->root_status), status.c_str());
  UpdateButtons(a);
}

fs::path RootPath(App* a) {
  const std::string text = gtk_entry_get_text(GTK_ENTRY(a->root_entry));
  if (!text.empty() && text[0] == '~') return setup::HomeDir() / fs::path(text.substr(1)).relative_path();
  return text;
}

// ---- options ------------------------------------------------------------------------------------------------------

void SteamToggled(GtkToggleButton* b, App* a) {
  const bool on = gtk_toggle_button_get_active(b);
  gtk_widget_set_visible(a->steam_profile, false);
  if (!on) {
    gtk_label_set_text(GTK_LABEL(a->steam_note), "");
    UpdateButtons(a);
    return;
  }
  a->profiles.clear();
  bool flatpak = false;
  for (const auto& install : setup::steam::FindInstalls(setup::HomeDir())) {
    for (auto& p : setup::steam::FindProfiles(install)) {
      flatpak |= install.flatpak;
      a->profiles.push_back(std::move(p));
    }
  }
  std::string note;
  if (a->profiles.empty()) {
    note = "Steam was not found for this user (or has never been signed in). Start Steam once, close it, and try "
           "again — or add the game to Steam yourself later.";
    gtk_toggle_button_set_active(b, FALSE);
  } else if (a->profiles.size() == 1) {
    note = "The game will be added to the Steam library of " + a->profiles[0].name + ". Steam must be closed while "
           "the setup changes its shortcuts.";
  } else {
    note = "Several Steam profiles were found. Choose the one to add the game to:";
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(a->steam_profile));
    for (const auto& p : a->profiles) {
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->steam_profile),
                                     (p.name + "  (" + p.account_id + (p.install.flatpak ? ", Flatpak" : "") + ")").c_str());
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->steam_profile), -1);
    gtk_widget_set_visible(a->steam_profile, true);
  }
  if (flatpak) note += " (Steam installed as a Flatpak may need permission to see the installation folder.)";
  gtk_label_set_text(GTK_LABEL(a->steam_note), note.c_str());
  UpdateButtons(a);
}

// ---- installing ---------------------------------------------------------------------------------------------------

struct ProgressMsg {
  setup::Stage stage;
  double fraction;
  std::string detail;
};

gboolean OnProgress(gpointer data) {
  std::unique_ptr<ProgressMsg> m(static_cast<ProgressMsg*>(data));
  App* a = g_app;
  const double overall = (double(m->stage) + std::clamp(m->fraction, 0.0, 1.0)) / double(setup::kStageCount);
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), overall);
  gtk_label_set_text(GTK_LABEL(a->stage_label), setup::StageName(m->stage));
  gtk_label_set_text(GTK_LABEL(a->detail_label), m->detail.c_str());
  return G_SOURCE_REMOVE;
}

struct DoneMsg {
  setup::Result result;
  bool uninstall;
};

gboolean OnDone(gpointer data) {
  std::unique_ptr<DoneMsg> m(static_cast<DoneMsg*>(data));
  App* a = g_app;
  a->running = false;
  a->installed_ok = m->result.ok && !m->uninstall;
  std::string title, text, notes;
  if (m->result.ok) {
    title = m->uninstall ? "The game was removed" : "Installation complete";
    text = m->uninstall ? "Kingdom Under Fire: Circle of Doom was uninstalled."
                        : "Kingdom Under Fire: Circle of Doom is installed in\n" + RootPath(a).string() +
                              "\n\nStart it from your application menu, your desktop or Steam (as you chose), or "
                              "with Launch Game below.";
  } else {
    title = m->uninstall ? "The game could not be removed" : "The installation did not complete";
    text = m->result.error + (m->uninstall ? "" : "\n\nNothing was installed; your game files were not changed.");
  }
  for (const auto& n : m->result.notes) notes += "• " + n + "\n";
  gtk_label_set_text(GTK_LABEL(a->finish_title), title.c_str());
  gtk_label_set_text(GTK_LABEL(a->finish_text), text.c_str());
  gtk_label_set_text(GTK_LABEL(a->finish_notes), notes.c_str());
  gtk_widget_set_visible(a->launch, a->installed_ok);
  ShowPage(a, kFinish);
  return G_SOURCE_REMOVE;
}

void Report(setup::Stage s, double f, const std::string& d) {
  static std::atomic<int64_t> last{0};
  const int64_t now = g_get_monotonic_time();
  if (f < 1 && f > 0 && now - last.load() < 50000) return;  // at most 20 updates per second
  last = now;
  g_idle_add(OnProgress, new ProgressMsg{s, f, d});
}

void StartInstall(App* a) {
  // Steam must be closed while its shortcuts file changes: asked before anything starts.
  const bool steam = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->opt_steam)) && !a->profiles.empty();
  bool with_steam = steam;
  while (with_steam && setup::steam::SteamRunning()) {
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(a->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          "Steam is running");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
                                             "Close Steam completely (Steam > Exit), then choose Retry. Or install "
                                             "without the Steam shortcut.");
    gtk_dialog_add_buttons(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL, "Without Steam", 2, "Retry", 1, nullptr);
    const int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_CANCEL || r == GTK_RESPONSE_DELETE_EVENT) return;
    if (r == 2) with_steam = false;
  }
  setup::Plan plan;
  plan.root = RootPath(a);
  plan.package = a->payload;
  plan.game_source = a->game_dir;
  plan.manifest = a->manifest;
  plan.setup_binary = a->exe;
  plan.setup_assets = a->assets;
  plan.icon = a->assets / "icon-256.png";
  plan.signers = a->signers;
  plan.menu = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->opt_menu));
  plan.desktop = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->opt_desktop));
  if (with_steam) {
    const int i = a->profiles.size() == 1 ? 0 : gtk_combo_box_get_active(GTK_COMBO_BOX(a->steam_profile));
    if (i >= 0) plan.steam_profile = a->profiles[size_t(i)];
  }
  a->cancel = false;
  a->running = true;
  a->uninstall_mode = false;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), 0);
  ShowPage(a, kInstalling);
  std::thread([a, plan] {
    setup::Result r = setup::Install(plan, Report, a->cancel);
    g_idle_add(OnDone, new DoneMsg{std::move(r), false});
  }).detach();
}

void StartUninstall(App* a) {
  setup::RemoveOptions o;
  o.shortcuts = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->rm_shortcuts));
  o.steam = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->rm_steam));
  o.user_data = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->rm_data));
  if (o.user_data) {  // a separate, explicit confirmation
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(a->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          "Delete your saves and settings?");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(d), "Your saved games and settings in %s will be deleted permanently.",
        (setup::XdgDataHome() / "kuf-cod-recomp" / "data").c_str());
    gtk_dialog_add_buttons(GTK_DIALOG(d), "Keep them", GTK_RESPONSE_CANCEL, "Delete them", GTK_RESPONSE_ACCEPT, nullptr);
    const int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r != GTK_RESPONSE_ACCEPT) return;
  }
  while (o.steam && setup::steam::SteamRunning()) {
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(a->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          "Steam is running");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
                                             "Close Steam completely to remove the game's Steam shortcut, then choose "
                                             "Retry. Or keep the Steam shortcut.");
    gtk_dialog_add_buttons(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL, "Keep it", 2, "Retry", 1, nullptr);
    const int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_CANCEL || r == GTK_RESPONSE_DELETE_EVENT) return;
    if (r == 2) o.steam = false;
  }
  a->running = true;
  a->uninstall_mode = true;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), 0);
  ShowPage(a, kInstalling);
  const fs::path root = a->existing;
  std::thread([root, o] {
    setup::Result r = setup::Uninstall(root, o, Report);
    g_idle_add(OnDone, new DoneMsg{std::move(r), true});
  }).detach();
}

// ---- buttons ------------------------------------------------------------------------------------------------------

void OnNext(GtkButton*, App* a) {
  switch (a->page) {
    case kWelcome: ShowPage(a, kGame); break;
    case kGame: ShowPage(a, kLocation); CheckLocation(a); break;
    case kLocation: ShowPage(a, kOptions); break;
    case kOptions: StartInstall(a); break;
    case kInstalling:
      a->cancel = true;
      gtk_label_set_text(GTK_LABEL(a->stage_label), "Cancelling…");
      break;
    case kFinish: gtk_window_close(GTK_WINDOW(a->window)); break;
    default: break;
  }
}

void OnBack(GtkButton*, App* a) {
  if (a->page == kGame) ShowPage(a, kWelcome);
  if (a->page == kLocation) ShowPage(a, kGame);
  if (a->page == kOptions) ShowPage(a, kLocation);
}

void OnLaunch(GtkButton*, App* a) {
  const std::string launcher = (RootPath(a) / "kuf-cod").string();
  gchar* argv[] = {const_cast<gchar*>(launcher.c_str()), nullptr};
  GError* err = nullptr;
  if (!g_spawn_async(RootPath(a).c_str(), argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &err)) {
    Message(a, GTK_MESSAGE_ERROR, "The game could not be started", err ? err->message : "");
    if (err) g_error_free(err);
    return;
  }
  gtk_window_close(GTK_WINDOW(a->window));
}

gboolean OnClose(GtkWidget*, GdkEvent*, App* a) {
  if (!a->running) return FALSE;
  GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(a->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                        "Stop the installation?");
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "What was copied so far is removed again.");
  const int r = gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
  if (r == GTK_RESPONSE_YES) a->cancel = true;
  return TRUE;  // closes when the worker has finished (OnDone shows the result)
}

// ---- building the window ------------------------------------------------------------------------------------------

GtkWidget* BuildWelcome(App* a) {
  GtkWidget* box = PageBox("");
  gtk_box_set_spacing(GTK_BOX(box), 8);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(box), Label("KINGDOM UNDER FIRE", "title-small", false, 0.5), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), Label("CIRCLE OF DOOM", "title-big", false, 0.5), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), Label("PC Edition — Setup", "edition", false, 0.5), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), Divider(a), FALSE, FALSE, 2);
  if (!a->compat.empty()) {  // checked before anything else: an installation that cannot start is not offered
    std::string w = "<span foreground='#ff8a6a'><b>This system cannot run this build of the game:</b></span>";
    for (const auto& p : a->compat) w += "\n• " + Esc(p);
    w += "\nUpdate your Linux distribution (or use a build made for it). Nothing was installed.";
    a->welcome_warning = Label(w, "body");
    gtk_box_pack_start(GTK_BOX(box), Panel(a->welcome_warning), FALSE, FALSE, 2);
  }
  GtkWidget* text = Label(
      "This setup installs the native Linux version of Kingdom Under Fire: Circle of Doom, rebuilt from the original "
      "Xbox 360 game. You need your own copy of the original game files (the folder with <b>default.xex</b>): they are "
      "checked, then copied into the installation; your original folder is never changed.",
      "body", true, 0.5);
  gtk_label_set_justify(GTK_LABEL(text), GTK_JUSTIFY_CENTER);
  gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 2);
  GtkWidget* fine = Label("An unofficial fan project, not affiliated with or endorsed by the game's publishers.  "
                          "Version " + Esc(a->package_version) + ".",
                          "muted", true, 0.5);
  gtk_label_set_justify(GTK_LABEL(fine), GTK_JUSTIFY_CENTER);
  gtk_box_pack_start(GTK_BOX(box), fine, FALSE, FALSE, 0);
  return box;
}

GtkWidget* BuildGame(App* a) {
  GtkWidget* box = PageBox("Your Original Game Files");
  gtk_box_pack_start(GTK_BOX(box),
                     Label("Select the folder with your legally obtained game files: the folder that contains "
                           "<b>default.xex</b> (for example a copy of your game disc's files).",
                           "body"),
                     FALSE, FALSE, 0);
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  a->game_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(a->game_entry), "/path/to/your game files");
  gtk_box_pack_start(GTK_BOX(row), a->game_entry, TRUE, TRUE, 0);
  GtkWidget* browse = gtk_button_new_with_mnemonic("_Browse…");
  AddClass(browse, "small");
  gtk_box_pack_start(GTK_BOX(row), browse, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
  GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  a->game_status = Label("Choose the folder that contains <b>default.xex</b>.", "body");
  a->game_details = Label("", "muted");
  gtk_box_pack_start(GTK_BOX(inner), a->game_status, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(inner), a->game_details, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), Panel(inner), FALSE, FALSE, 4);
  g_signal_connect_swapped(a->game_entry, "changed", G_CALLBACK(+[](App* app) { CheckGame(app); }), a);
  g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton*, App* app) {
                     ChooseFolder(app, app->game_entry, "Select your game files (the folder with default.xex)");
                   }),
                   a);
  return box;
}

GtkWidget* BuildLocation(App* a) {
  GtkWidget* box = PageBox("Installation Folder");
  gtk_box_pack_start(GTK_BOX(box),
                     Label("Where should the game be installed? It is installed for your user only; no "
                           "administrator rights are needed.",
                           "body"),
                     FALSE, FALSE, 0);
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  a->root_entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(a->root_entry), setup::DefaultRoot().c_str());
  gtk_box_pack_start(GTK_BOX(row), a->root_entry, TRUE, TRUE, 0);
  GtkWidget* browse = gtk_button_new_with_mnemonic("_Change…");
  AddClass(browse, "small");
  gtk_box_pack_start(GTK_BOX(row), browse, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
  a->root_status = Label("", "body");
  gtk_box_pack_start(GTK_BOX(box), Panel(a->root_status), FALSE, FALSE, 4);
  gtk_box_pack_start(GTK_BOX(box),
                     Label("Your saves and settings are kept separately in " +
                               Esc((setup::XdgDataHome() / "kuf-cod-recomp" / "data").string()) +
                               ", so they survive a reinstall.",
                           "muted"),
                     FALSE, FALSE, 0);
  g_signal_connect_swapped(a->root_entry, "changed", G_CALLBACK(+[](App* app) { CheckLocation(app); }), a);
  g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton*, App* app) {
                     ChooseFolder(app, app->root_entry, "Choose the installation folder");
                   }),
                   a);
  return box;
}

GtkWidget* BuildOptions(App* a) {
  GtkWidget* box = PageBox("Shortcuts");
  GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  a->opt_menu = gtk_check_button_new_with_mnemonic("Create _application-menu shortcut");
  a->opt_desktop = gtk_check_button_new_with_mnemonic("Create _desktop shortcut");
  a->opt_steam = gtk_check_button_new_with_mnemonic("Add to _Steam as a non-Steam game");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->opt_menu), TRUE);
  gtk_box_pack_start(GTK_BOX(inner), a->opt_menu, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(inner), a->opt_desktop, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(inner), a->opt_steam, FALSE, FALSE, 0);
  a->steam_note = Label("", "muted");
  gtk_widget_set_margin_start(a->steam_note, 38);
  a->steam_profile = gtk_combo_box_text_new();
  gtk_widget_set_margin_start(a->steam_profile, 38);
  gtk_widget_set_halign(a->steam_profile, GTK_ALIGN_START);
  gtk_widget_set_no_show_all(a->steam_profile, TRUE);
  gtk_box_pack_start(GTK_BOX(inner), a->steam_note, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(inner), a->steam_profile, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), Panel(inner), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box),
                     Label("The game itself is installed either way; shortcuts can be removed again with the "
                           "uninstaller.",
                           "muted"),
                     FALSE, FALSE, 0);
  g_signal_connect(a->opt_steam, "toggled", G_CALLBACK(SteamToggled), a);
  g_signal_connect_swapped(a->steam_profile, "changed", G_CALLBACK(+[](App* app) { UpdateButtons(app); }), a);
  return box;
}

GtkWidget* BuildInstalling(App* a) {
  GtkWidget* box = PageBox("Installing");
  gtk_widget_set_valign(box, GTK_ALIGN_FILL);
  GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  a->stage_label = Label("Preparing", "page-title");
  RemoveClass(a->stage_label, "page-title");
  AddClass(a->stage_label, "edition");
  a->progress = gtk_progress_bar_new();
  a->detail_label = Label("", "muted", false);
  gtk_label_set_ellipsize(GTK_LABEL(a->detail_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(inner), a->stage_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(inner), a->progress, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(inner), a->detail_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), Panel(inner), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box),
                     Label("Copying and checking about 4 GB of game files takes a few minutes. If anything fails, "
                           "what was copied is removed again.",
                           "muted"),
                     FALSE, FALSE, 0);
  return box;
}

GtkWidget* BuildFinish(App* a) {
  GtkWidget* box = PageBox("");
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  a->finish_title = Label("", "page-title", false, 0.5);
  gtk_box_pack_start(GTK_BOX(box), a->finish_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), Divider(a), FALSE, FALSE, 0);
  a->finish_text = Label("", "body", true, 0.5);
  gtk_label_set_justify(GTK_LABEL(a->finish_text), GTK_JUSTIFY_CENTER);
  gtk_box_pack_start(GTK_BOX(box), a->finish_text, FALSE, FALSE, 0);
  a->finish_notes = Label("", "muted", true, 0);
  gtk_box_pack_start(GTK_BOX(box), a->finish_notes, FALSE, FALSE, 0);
  a->launch = gtk_button_new_with_mnemonic("_Launch Game");
  AddClass(a->launch, "primary");
  gtk_widget_set_halign(a->launch, GTK_ALIGN_CENTER);
  gtk_widget_set_no_show_all(a->launch, TRUE);
  gtk_box_pack_start(GTK_BOX(box), a->launch, FALSE, FALSE, 8);
  g_signal_connect(a->launch, "clicked", G_CALLBACK(OnLaunch), a);
  return box;
}

GtkWidget* BuildMaintenance(App* a) {
  GtkWidget* box = PageBox("The Game Is Installed");
  a->maint_text = Label("", "body");
  gtk_box_pack_start(GTK_BOX(box), a->maint_text, FALSE, FALSE, 0);
  GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_pack_start(GTK_BOX(inner), Label("<b>Uninstall</b> removes the installed game and:", "body"), FALSE, FALSE, 0);
  a->rm_shortcuts = gtk_check_button_new_with_mnemonic("the setup's _menu and desktop shortcuts");
  a->rm_steam = gtk_check_button_new_with_mnemonic("the setup's _Steam shortcut");
  a->rm_data = gtk_check_button_new_with_mnemonic("my saves and _settings too (asks again)");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->rm_shortcuts), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->rm_steam), TRUE);
  for (GtkWidget* w : {a->rm_shortcuts, a->rm_steam, a->rm_data}) gtk_box_pack_start(GTK_BOX(inner), w, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(inner),
                     Label("Your original game files are never touched. Files you added to the installation folder "
                           "are kept.",
                           "muted"),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), Panel(inner), FALSE, FALSE, 0);
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  a->repair_button = gtk_button_new_with_mnemonic("_Repair or Reinstall…");
  a->uninstall_button = gtk_button_new_with_mnemonic("_Uninstall");
  gtk_box_pack_start(GTK_BOX(row), a->repair_button, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(row), a->uninstall_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 4);
  g_signal_connect(a->uninstall_button, "clicked", G_CALLBACK(+[](GtkButton*, App* app) { StartUninstall(app); }), a);
  g_signal_connect(a->repair_button, "clicked", G_CALLBACK(+[](GtkButton*, App* app) {
                     gtk_entry_set_text(GTK_ENTRY(app->root_entry), app->existing.c_str());
                     ShowPage(app, kGame);
                   }),
                   a);
  return box;
}

void BuildWindow(App* a) {
  a->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  AddClass(a->window, "kuf");
  gtk_window_set_title(GTK_WINDOW(a->window), "Kingdom Under Fire: Circle of Doom — Setup");
  GtkWidget* header = gtk_header_bar_new();  // themed (theme.css), the same on every desktop
  gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Circle of Doom — Setup");  // the taskbar has the full name
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
  gtk_window_set_titlebar(GTK_WINDOW(a->window), header);
  gtk_window_set_default_size(GTK_WINDOW(a->window), 1040, 720);
  gtk_widget_set_size_request(a->window, 860, 640);
  gtk_window_set_icon_from_file(GTK_WINDOW(a->window), (a->assets / "icon-256.png").c_str(), nullptr);
  g_signal_connect(a->window, "delete-event", G_CALLBACK(OnClose), a);
  g_signal_connect(a->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

  GtkWidget* frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  AddClass(frame, "frame");
  gtk_container_add(GTK_CONTAINER(a->window), frame);

  // The step list along the top.
  GtkWidget* steps = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign(steps, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(steps, 16);
  for (int i = 0; i < 6; ++i) {
    a->steps[i] = Label(kStepNames[i], "step", false, 0.5);
    gtk_box_pack_start(GTK_BOX(steps), a->steps[i], FALSE, FALSE, 0);
    if (i < 5) gtk_box_pack_start(GTK_BOX(steps), Label("·", "step", false, 0.5), FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(frame), steps, FALSE, FALSE, 0);

  a->stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(a->stack), GTK_STACK_TRANSITION_TYPE_NONE);  // no animations
  GtkWidget* (*builders[])(App*) = {BuildWelcome, BuildGame, BuildLocation, BuildOptions,
                                    BuildInstalling, BuildFinish, BuildMaintenance};
  for (int i = 0; i < kPageCount; ++i) {
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);  // small windows scroll, nothing overlaps
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), builders[i](a));
    gtk_stack_add_named(GTK_STACK(a->stack), scroller, kPageIds[i]);
  }
  gtk_box_pack_start(GTK_BOX(frame), a->stack, TRUE, TRUE, 8);

  GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(buttons, 40);
  gtk_widget_set_margin_end(buttons, 40);
  gtk_widget_set_margin_bottom(buttons, 26);
  a->back = gtk_button_new_with_mnemonic("_Back");
  a->next = gtk_button_new_with_mnemonic("_Continue");
  gtk_widget_set_can_default(a->next, TRUE);
  gtk_box_pack_start(GTK_BOX(buttons), a->back, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(buttons), a->next, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(frame), buttons, FALSE, FALSE, 0);
  g_signal_connect(a->next, "clicked", G_CALLBACK(OnNext), a);
  g_signal_connect(a->back, "clicked", G_CALLBACK(OnBack), a);
  gtk_widget_set_no_show_all(a->back, TRUE);

}

// ---- start --------------------------------------------------------------------------------------------------------

fs::path SelfExe() {
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec);
}

bool LoadAssets(App* a, const fs::path& assets_arg, const fs::path& payload_arg) {
  const fs::path dir = a->exe.parent_path();
  for (const fs::path& c : {assets_arg, fs::path(setup::Env("KUF_SETUP_ASSETS")), dir / "assets",
                            dir / "../share/kuf-setup/assets"}) {
    std::error_code ec;
    if (!c.empty() && fs::exists(c / "theme.css", ec)) {
      a->assets = fs::canonical(c, ec);
      break;
    }
  }
  if (a->assets.empty()) return false;
  for (const fs::path& c : {payload_arg, dir / "../share/kuf-setup/payload/package"}) {
    std::error_code ec;
    if (!c.empty() && fs::exists(c / "MANIFEST.sha256", ec)) {
      a->payload = fs::canonical(c, ec);
      break;
    }
  }
  if (!a->payload.empty()) {
    std::string v;
    setup::ReadFile(a->payload / "VERSION", &v);
    while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
    a->package_version = v;
    std::error_code ec;
    const fs::path signers = a->payload.parent_path() / "allowed_signers";
    if (fs::exists(signers, ec)) a->signers = signers;
    std::string requires_text;
    if (setup::ReadFile(a->payload.parent_path() / "requires.txt", &requires_text)) {
      a->compat = setup::CompatibilityProblems(setup::ParseRequirements(requires_text), setup::HostGlibc(),
                                               setup::HostGlibcxx());
      if (!setup::Env("KUF_SETUP_PRETEND_OLD_SYSTEM").empty()) {  // tests of this page
        a->compat = setup::CompatibilityProblems(setup::ParseRequirements(requires_text), "2.39", "3.4.33");
      }
    }
  }
  std::string list;
  if (!setup::ReadFile(a->assets / "game_v1.tsv", &list)) return false;
  a->manifest = setup::ParseGameManifest(list, "Xbox 360, base version");
  if (a->manifest.files.empty()) return false;
  // The bundled font (X360 by Redge, "100% Free" at dafont.com) for this program only.
  FcConfigAppFontAddFile(FcConfigGetCurrent(), reinterpret_cast<const FcChar8*>((a->assets / "fonts/X360.ttf").c_str()));
  GtkCssProvider* css = gtk_css_provider_new();
  GError* err = nullptr;
  if (!gtk_css_provider_load_from_path(css, (a->assets / "theme.css").c_str(), &err)) {
    g_printerr("theme: %s\n", err ? err->message : "?");
    if (err) g_error_free(err);
  }
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  return true;
}

// Draws each page into DIR/<page>.png (with sample contents) and quits.
void Screenshots(App* a, const fs::path& dir) {
  fs::create_directories(dir);
  auto pump = [] {
    for (int i = 0; i < 40; ++i) {
      while (gtk_events_pending()) gtk_main_iteration();
      g_usleep(10000);
    }
  };
  auto shot = [&](const char* name) {
    pump();
    const int w = gtk_widget_get_allocated_width(a->window), h = gtk_widget_get_allocated_height(a->window);
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(s);
    gtk_widget_draw(a->window, cr);
    cairo_surface_write_to_png(s, (dir / (std::string(name) + ".png")).c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(s);
  };
  ShowPage(a, kWelcome);
  shot("1-welcome");
  ShowPage(a, kGame);
  gtk_entry_set_text(GTK_ENTRY(a->game_entry), setup::Env("KUF_SETUP_SHOT_GAME").c_str());
  shot("2-game");
  ShowPage(a, kLocation);
  CheckLocation(a);
  shot("3-location");
  ShowPage(a, kOptions);
  shot("4-options");
  ShowPage(a, kInstalling);
  g_idle_add(OnProgress, new ProgressMsg{setup::kGameData, 0.42, "Data/Sound/SoundRes/71_BGM.xwb"});
  shot("5-installing");
  setup::Result r;
  r.ok = true;
  r.notes = {"Application-menu shortcut created.", "Steam: The Steam shortcut was added."};
  g_idle_add(OnDone, new DoneMsg{r, false});
  pump();
  shot("6-finish");
  a->existing = setup::DefaultRoot();
  gtk_label_set_text(GTK_LABEL(a->maint_text), ("Kingdom Under Fire: Circle of Doom (version 0.7.0) is installed in " +
                                                a->existing.string() + ".").c_str());
  ShowPage(a, kMaintenance);
  shot("7-maintenance");
}

}  // namespace

int main(int argc, char** argv) {
  fs::path assets_arg, payload_arg, uninstall_arg, shot_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--assets" && i + 1 < argc) assets_arg = argv[++i];
    else if (a == "--payload" && i + 1 < argc) payload_arg = argv[++i];
    else if (a == "--uninstall" && i + 1 < argc) uninstall_arg = argv[++i];
    else if (a == "--screenshot" && i + 1 < argc) shot_dir = argv[++i];
  }
  gtk_init(&argc, &argv);
  App app;
  g_app = &app;
  app.exe = SelfExe();
  if (!LoadAssets(&app, assets_arg, payload_arg)) {
    GtkWidget* d = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                          "The setup's files are missing or damaged.");
    gtk_dialog_run(GTK_DIALOG(d));
    return 1;
  }
  BuildWindow(&app);
  gtk_widget_show_all(app.window);

  // Run over an installation (or --uninstall FOLDER): repair or removal. Without a game package (the copy kept in the
  // installation for uninstalling) only removal is offered.
  fs::path existing = uninstall_arg;
  if (existing.empty() && setup::ReadRecord(setup::DefaultRoot())) existing = setup::DefaultRoot();
  if (!existing.empty() && setup::ReadRecord(existing)) {
    app.existing = existing;
    const auto rec = setup::ReadRecord(existing);
    gtk_label_set_text(GTK_LABEL(app.maint_text),
                       ("Kingdom Under Fire: Circle of Doom (version " + rec->version + ") is installed in " +
                        existing.string() + ".")
                           .c_str());
    gtk_widget_set_visible(app.repair_button, !app.payload.empty());
    ShowPage(&app, kMaintenance);
  } else if (!uninstall_arg.empty()) {
    Message(&app, GTK_MESSAGE_ERROR, "No installation found", "There is no installation of the game in " +
                                                                  uninstall_arg.string() + ".");
    return 1;
  } else if (app.payload.empty() && shot_dir.empty()) {
    Message(&app, GTK_MESSAGE_ERROR, "The setup's game package is missing",
            "This copy of the setup cannot install the game (it has no game package).");
    return 1;
  } else {
    ShowPage(&app, kWelcome);
  }
  if (!shot_dir.empty()) {
    Screenshots(&app, shot_dir);
    return 0;
  }
  gtk_main();
  return 0;
}
