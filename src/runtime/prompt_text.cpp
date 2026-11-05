#include "runtime/prompt_text.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "input/game_input_hooks.h"
#include "input/kuf_input_driver.h"
#include "runtime/pc_settings.h"
#include "runtime/update_status.h"

REXCVAR_DEFINE_STRING(kuf_button_prompts, "auto", "KUF/Input",
                      "Button prompts: auto (the device used last), controller, keyboard (M7)");

REXCVAR_DEFINE_BOOL(kuf_trace_prompts, false, "KUF/Debug", "Log each text given keyboard prompts (M7)");
REXCVAR_DECLARE(bool, kuf_mouse_look);
REX_EXTERN(sub_8218F410);  // the game's allocation

namespace kuf::prompts {
namespace {

using kuf::input::PromptDevice;

constexpr int kNone = -1;
std::atomic<int> g_last{kNone};  // the last device used (PromptDevice), or none yet
std::atomic<int> g_logged{kNone};

std::mutex g_mutex;
std::unordered_map<std::string, uint32_t> g_copies;  // rewritten text -> its guest copy (kept: texts are reused)
constexpr size_t kMaxText = 4096;
constexpr size_t kMaxCopies = 4096;  // a bound on the copies (each distinct text once; ~2,000 texts per language)
std::optional<std::pair<std::string, std::string>> g_labels;  // the message box's answers renamed (g_mutex)

std::string Rewrite(std::string_view text, const uint8_t* base) {
  auto* keyboard = kuf::input::KufInputDriver::Instance();
  if (!keyboard) return std::string(text);
  const auto game = base ? kuf::input::LiveGameContext(base) : kuf::input::CurrentGameContext();
  // A button help bar (two or more icons) names the menu's buttons; a hint with one icon follows the game's context.
  const bool help_bar = kuf::input::CountButtonIcons(text) >= 2;
  const auto context = game == kuf::input::GameContext::kStartPrompt               ? kuf::input::Context::kStartPrompt
                       : game == kuf::input::GameContext::kGameplay && !help_bar ? kuf::input::Context::kGameplay
                                                                                   : kuf::input::Context::kMenu;
  const auto bindings = keyboard->CurrentBindings();
  const bool mouse_look = context == kuf::input::Context::kGameplay && REXCVAR_GET(kuf_mouse_look);
  return kuf::input::RewritePrompts(text, [&](std::string_view icon) {
    return kuf::input::IconKeys(icon, bindings, context, mouse_look);
  });
}

}  // namespace

void NoteDevice(PromptDevice device) {
  g_last.store(int(device), std::memory_order_relaxed);
  if (g_logged.exchange(int(device)) != int(device)) {
    REXLOG_INFO("[kuf-input] button prompts: {}", device == PromptDevice::kKeyboard ? "keyboard" : "controller");
  }
}

PromptDevice Current() {
  const std::string mode = REXCVAR_GET(kuf_button_prompts);  // the command line wins over PC Settings
  if (mode == "controller") return PromptDevice::kController;
  if (mode == "keyboard") return PromptDevice::kKeyboard;
  const int setting = kuf::pc::Current().ui.button_prompts;
  if (setting == kuf::pc::kPromptsController) return PromptDevice::kController;
  if (setting == kuf::pc::kPromptsKeyboard) return PromptDevice::kKeyboard;
  // Before any input: the keyboard and mouse (M7 UI follow-up: a connected controller made the first screens
  // controller ones, with the Xbox arrow, until the first key or click). The first controller input switches.
  const int last = g_last.load(std::memory_order_relaxed);
  return last != kNone ? PromptDevice(last) : PromptDevice::kKeyboard;
}

void SetAnswerLabels(std::string a, std::string b) {
  std::lock_guard lock(g_mutex);
  g_labels.emplace(std::move(a), std::move(b));
}

void ClearAnswerLabels() {
  std::lock_guard lock(g_mutex);
  g_labels.reset();
}

uint32_t OnTextLookup(PPCContext& ctx, uint8_t* base, uint32_t text) {
  if (text < 0x10000 || text >= 0xF0000000) return 0;
  const char* s = reinterpret_cast<const char*>(base + text);
  const size_t n = strnlen(s, kMaxText);
  if (n == kMaxText || !std::memchr(s, '@', n)) return 0;
  const std::string_view original(s, n);
  std::string relabeled(original);
  {
    std::lock_guard lock(g_mutex);
    if (g_labels) relabeled = kuf::updates::RelabelAnswers(original, g_labels->first, g_labels->second);
  }
  const bool keyboard = Current() == PromptDevice::kKeyboard;
  if (!keyboard && relabeled == original) return 0;
  std::string rewritten = keyboard ? Rewrite(kuf::input::ConciseHelpBar(relabeled), base) : relabeled;
  if (rewritten == original) return 0;
  std::lock_guard lock(g_mutex);
  if (const auto it = g_copies.find(rewritten); it != g_copies.end()) return it->second;
  if (g_copies.size() >= kMaxCopies) return 0;
  ctx.r3.u64 = rewritten.size() + 1;
  sub_8218F410(ctx, base);
  const uint32_t copy = ctx.r3.u32;
  if (!copy) return 0;
  std::memcpy(base + copy, rewritten.c_str(), rewritten.size() + 1);
  if (REXCVAR_GET(kuf_trace_prompts)) REXLOG_INFO("[kuf-input] keyboard prompt: {}", rewritten);
  g_copies.emplace(std::move(rewritten), copy);
  return copy;
}

std::string ForCurrentDevice(std::string_view text) {
  return Current() == PromptDevice::kKeyboard ? Rewrite(text, nullptr) : std::string(text);
}

}  // namespace kuf::prompts
