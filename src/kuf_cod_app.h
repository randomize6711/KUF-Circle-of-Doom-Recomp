// kuf_cod - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <rex/runtime.h>
#include <rex/ui/window.h>

#include "input/input_factory.h"
#include "runtime/audio_meter.h"
#include "input/input_script.h"
#include "runtime/crash_report.h"
#include "runtime/display.h"
#include "runtime/frame_capture.h"
#include "runtime/frame_pacing.h"
#include "runtime/menu_pointer.h"
#include "runtime/pc_settings.h"
#include "runtime/telemetry.h"
#include "runtime/title_menu.h"
#include "runtime/version.h"

REXCVAR_DECLARE(std::string, content_root);  // SDK (patch 0005): where saves live

class KufCodApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<KufCodApp>(new KufCodApp(ctx, "kuf_cod",
        PPCImageConfig));
  }

  // The game folder is the user's original disc copy and is mounted read-only. The SDK's developer switch
  // --allow_game_relative_writes would make it writable; this port never allows that.
  //
  // Saves (M5B) go to --content_root, else the user data root, and never into the game folder.
  //
  // Input (M4): SDL controllers plus our keyboard/mouse driver (src/input, docs/INPUT.md).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    REXLOG_INFO("[kuf] version {} (commit {})", kuf::version::String(), kuf::version::Commit());
    if (REXCVAR_GET(allow_game_relative_writes)) {
      REXLOG_ERROR("--allow_game_relative_writes is not supported: the game folder must stay read-only");
      rex::FlushLogging();
      std::_Exit(2);
    }
    std::filesystem::path saves = REXCVAR_GET(content_root);
    if (saves.empty()) {
      saves = user_data_root();
    }
    if (Inside(saves, game_data_root())) {
      REXLOG_ERROR("saves ({}) would be inside the game folder ({}): choose another --content_root or "
                   "--user_data_root", saves.string(), game_data_root().string());
      rex::FlushLogging();
      std::_Exit(2);
    }
    // PC settings (M6B): pc_settings.toml next to the other user data, never in the saves or the game folder.
    kuf::pc::Init(user_data_root());
    kuf::display::Configure();  // M6C: before the graphics system starts (its render scale)
    kuf::pacing::Configure(user_data_root());  // M6D: before the graphics system starts (its vblank rate)
    config.input_factory = kuf::input::CreateInputSystem;
    config.audio_factory = REX_AUDIO_BACKEND(kuf::audio::MeteredSDLAudioSystem);
    std::string error;
    if (!kuf::input::script::Start(&error)) {
      REXLOG_ERROR("{}", error);
      rex::FlushLogging();
      std::_Exit(2);
    }
  }

  // Runs after the runtime (memory, GPU plugin) has installed its own fault handlers and before any
  // guest thread starts, so the crash handler is the last one consulted.
  void OnPostSetup() override {
    kuf::diag::InstallCrashHandler({user_data_root() / "crash-report.txt", runtime()->memory()});
    kuf::menu_pointer::Start(runtime()->memory());
    auto* graphics = runtime()->graphics_system();
    kuf::capture::Start(user_data_root(), graphics ? graphics->presenter() : nullptr);
    kuf::display::Start(window());
    kuf::display::SetPresenter(graphics ? graphics->presenter() : nullptr);
    kuf::app::SetQuitHandler([this] {  // the title list's Quit Game (M7): as closing the window
      app_context().CallInUIThread([this] {
        if (window()) window()->RequestClose();
      });
    });
    kuf::telemetry::Start(user_data_root(), [this] {
      // Same path as a user closing the window: OnClosing terminates the title and exits the process.
      app_context().CallInUIThread([this] {
        kuf::display::Stop();
        kuf::capture::Stop();
        kuf::input::script::Stop();
        kuf::telemetry::Stop();
        if (window()) {
          window()->RequestClose();
        }
      });
    });
  }

  // A user closing the window ends the process by hard exit after the title terminates, before OnShutdown;
  // write the run summary now (exit_reason stays "shutdown"). Stop() is idempotent.
  bool OnWindowCloseRequested() override {
    kuf::display::Stop();
    kuf::capture::Stop();
    kuf::input::script::Stop();
    kuf::telemetry::Stop();
    return true;
  }

  void OnShutdown() override {
    kuf::display::Stop();
    kuf::capture::Stop();
    kuf::input::script::Stop();
    kuf::telemetry::Stop();
  }

  static bool Inside(const std::filesystem::path& path, const std::filesystem::path& dir) {
    if (dir.empty()) {
      return false;
    }
    std::error_code ec;
    const auto p = std::filesystem::weakly_canonical(std::filesystem::absolute(path), ec);
    const auto d = std::filesystem::weakly_canonical(std::filesystem::absolute(dir), ec);
    return std::mismatch(p.begin(), p.end(), d.begin(), d.end()).second == d.end();
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
