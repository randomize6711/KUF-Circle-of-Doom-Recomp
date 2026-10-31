#include "runtime/frame_capture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "runtime/png_writer.h"

REXCVAR_DEFINE_STRING(kuf_capture_frames, "", "KUF",
                      "Comma-separated presented-frame numbers to capture as PNG (offscreen validation)");
REXCVAR_DEFINE_INT32(kuf_capture_every, 0, "KUF", "Also capture every N-th presented frame (0 = off)");

namespace kuf::capture {
namespace {

using Clock = std::chrono::steady_clock;

rex::ui::Presenter* g_presenter = nullptr;
std::filesystem::path g_dir;
std::set<uint64_t> g_targets;
int g_every = 0;
Clock::time_point g_start;

std::mutex g_mutex;
std::condition_variable g_cv;
struct Request {
  uint64_t frame;
  std::string label;  // from the input script's `capture <label>`; empty for numbered captures
};
std::deque<Request> g_queue;
bool g_stop = false;
std::thread g_thread;
std::atomic<bool> g_enabled{false};
std::set<std::string> g_written;  // capture worker only

std::string LabelJson(const std::string& label) {
  return label.empty() ? std::string() : ", \"label\": \"" + label + "\"";
}

void CaptureOne(uint64_t requested, const std::string& label, std::ofstream& index) {
  rex::ui::RawImage image;
  const double t = std::chrono::duration<double>(Clock::now() - g_start).count();
  if (!g_presenter || !g_presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
    REXLOG_WARN("[kuf-capture] frame {}: no guest output available", requested);
    index << "{\"requested_frame\": " << requested << ", \"ok\": false, \"time_s\": " << t << LabelJson(label)
          << "}\n";
    return;
  }
  char name[96];
  std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(requested));
  if (!g_written.insert(name).second && !label.empty()) {
    // Several captures of one frame (M6D: a tick drawn again while the game is held): keep each one.
    std::snprintf(name, sizeof(name), "frame_%06llu_%s.png", static_cast<unsigned long long>(requested),
                  label.c_str());
  }
  const bool ok = png::WriteRgb(g_dir / name, image.width, image.height, image.data.data(), image.stride);
  index << "{\"requested_frame\": " << requested << ", \"ok\": " << (ok ? "true" : "false") << ", \"file\": \""
        << name << "\", \"width\": " << image.width << ", \"height\": " << image.height
        << ", \"time_s\": " << t << LabelJson(label) << "}\n";
  index.flush();
  REXLOG_INFO("[kuf-capture] frame {} -> {} ({}x{}){}{}", requested, name, image.width, image.height,
              label.empty() ? "" : " label ", label);
}

void Worker() {
  std::ofstream index(g_dir / "captures.jsonl", std::ios::app);
  std::unique_lock lock(g_mutex);
  for (;;) {
    g_cv.wait(lock, [] { return g_stop || !g_queue.empty(); });
    if (g_stop && g_queue.empty()) {
      return;
    }
    Request request = std::move(g_queue.front());
    g_queue.pop_front();
    lock.unlock();
    CaptureOne(request.frame, request.label, index);
    lock.lock();
  }
}

}  // namespace

void Start(const std::filesystem::path& user_data_root, rex::ui::Presenter* presenter) {
  g_presenter = presenter;
  g_start = Clock::now();
  g_every = std::max(0, REXCVAR_GET(kuf_capture_every));
  std::stringstream list(REXCVAR_GET(kuf_capture_frames));
  for (std::string item; std::getline(list, item, ',');) {
    if (item.empty()) {
      continue;
    }
    if (item.find_first_not_of("0123456789") != std::string::npos) {
      REXLOG_ERROR("[kuf-capture] --kuf_capture_frames: '{}' is not a frame number; frame captures disabled", item);
      g_targets.clear();
      break;
    }
    g_targets.insert(std::stoull(item));
  }
  if (!presenter) {
    if (!g_targets.empty() || g_every > 0) {
      REXLOG_WARN("[kuf-capture] capture requested but no presenter (GPU emulation off?)");
    }
    return;
  }
  g_dir = user_data_root / "captures";
  std::filesystem::create_directories(g_dir);
  g_enabled = true;
  g_thread = std::thread(Worker);
}

void Stop() {
  if (!g_enabled.exchange(false)) {
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

void RequestLabeled(uint64_t frame, const std::string& label) {
  if (!g_enabled.load(std::memory_order_relaxed)) {
    REXLOG_WARN("[kuf-capture] capture '{}' requested but capture is not available", label);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_queue.push_back({frame, label});
  }
  g_cv.notify_one();
}

void OnFramePresented(uint64_t frame) {
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  if (!g_targets.count(frame) && !(g_every > 0 && frame % g_every == 0)) {
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_queue.push_back({frame, {}});
  }
  g_cv.notify_one();
}

}  // namespace kuf::capture
