#include "runtime/telemetry.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "input/input_script.h"
#include "runtime/frame_capture.h"
#include "runtime/frame_pacing.h"
#include "runtime/import_audit.h"
#include "runtime/profiler.h"

REXCVAR_DEFINE_INT32(kuf_exit_after_seconds, 0, "KUF",
                     "Request a normal application quit after this many seconds (0 = never). For smoke tests.");
REXCVAR_DEFINE_INT32(kuf_progress_interval_seconds, 10, "KUF", "Interval of [kuf-progress] log lines.");

// v1 addresses (docs/ENGINE_ANALYSIS.md):
REX_EXTERN(__imp__sub_8216E1B0);  // D3D present path; the game's only VdSwap call site

namespace kuf::telemetry {
namespace {

using Clock = std::chrono::steady_clock;

struct FileRecord {
  uint32_t opens = 0;
  uint32_t failures = 0;
  double first_open_s = 0;
};

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_tick_frames{0};  // presented frames that show a game tick for the first time (M6D)
std::mutex g_files_mutex;
std::map<std::string, FileRecord> g_files;
Clock::time_point g_start;
std::filesystem::path g_summary_path;

std::mutex g_thread_mutex;
std::condition_variable g_thread_cv;
bool g_stop = false;
std::thread g_thread;
std::vector<uint64_t> g_frame_timeline;     // frames presented per progress interval
std::vector<double> g_guest_tick_rate;      // guest timebase ticks per wall-clock second, per interval
std::vector<double> g_cpu_percent;          // process CPU time / wall time, per interval (100 = one core)
std::vector<uint64_t> g_rss_mb;             // resident set size at the end of each interval (0 = unknown)
std::vector<uint64_t> g_sim_tick_timeline;  // game ticks completed per interval (M6D)
std::vector<uint64_t> g_game_counter_timeline;  // the game's own tick counter's increase per interval
std::string g_exit_reason = "shutdown";
std::function<void()> g_request_exit;
std::atomic<bool> g_stopped{false};

double SecondsSinceStart() {
  return std::chrono::duration<double>(Clock::now() - g_start).count();
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

double ProcessCpuSeconds() {
#if defined(__linux__)
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#else
  return 0;
#endif
}

uint64_t ProcessRssMb() {
#if defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  uint64_t size_pages = 0, resident_pages = 0;
  if (statm >> size_pages >> resident_pages) {
    return resident_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) / (1024 * 1024);
  }
#endif
  return 0;
}

void Worker() {
  const int interval = std::max(1, REXCVAR_GET(kuf_progress_interval_seconds));
  const int exit_after = REXCVAR_GET(kuf_exit_after_seconds);
  uint64_t last_frames = 0;
  uint64_t last_sim_ticks = kuf::pacing::TicksCompleted();
  uint32_t last_counter = kuf::pacing::GameTickCounter();
  uint64_t last_ticks = rex::chrono::Clock::QueryGuestTickCount();
  double last_cpu = ProcessCpuSeconds();
  auto last_wall = Clock::now();
  std::unique_lock lock(g_thread_mutex);
  for (int elapsed = interval;; elapsed += interval) {
    if (g_thread_cv.wait_until(lock, g_start + std::chrono::seconds(elapsed), [] { return g_stop; })) {
      return;
    }
    uint64_t frames = g_frames.load();
    size_t files;
    {
      std::lock_guard files_lock(g_files_mutex);
      files = g_files.size();
    }
    const auto wall = Clock::now();
    const double wall_s = std::chrono::duration<double>(wall - last_wall).count();
    const uint64_t ticks = rex::chrono::Clock::QueryGuestTickCount();
    const double cpu = ProcessCpuSeconds();
    const double tick_rate = (ticks - last_ticks) / wall_s;
    const double cpu_percent = 100.0 * (cpu - last_cpu) / wall_s;
    last_wall = wall;
    last_ticks = ticks;
    last_cpu = cpu;
    g_frame_timeline.push_back(frames - last_frames);
    g_guest_tick_rate.push_back(tick_rate);
    g_cpu_percent.push_back(cpu_percent);
    const uint64_t sim_ticks = kuf::pacing::TicksCompleted();
    const uint32_t counter = kuf::pacing::GameTickCounter();
    g_sim_tick_timeline.push_back(sim_ticks - last_sim_ticks);
    g_game_counter_timeline.push_back(uint32_t(counter - last_counter));
    const uint64_t rss_mb = ProcessRssMb();
    g_rss_mb.push_back(rss_mb);
    REXLOG_INFO("[kuf-progress] t={}s frames={} (+{}) files_opened={} guest_ticks/s={:.0f} cpu={:.0f}% rss={}MB",
                elapsed, frames, frames - last_frames, files, tick_rate, cpu_percent, rss_mb);
    REXLOG_INFO("[kuf-pacing] t={}s game ticks +{} ({:.2f}/s), game counter +{}, frames +{} ({:.2f}/s)", elapsed,
                sim_ticks - last_sim_ticks, (sim_ticks - last_sim_ticks) / wall_s, uint32_t(counter - last_counter),
                frames - last_frames, (frames - last_frames) / wall_s);
    last_frames = frames;
    last_sim_ticks = sim_ticks;
    last_counter = counter;
    if (exit_after > 0 && elapsed >= exit_after && g_request_exit) {
      g_exit_reason = "kuf_exit_after_seconds";
      REXLOG_INFO("[kuf-progress] requesting exit after {}s", elapsed);
      g_request_exit();
      return;
    }
  }
}

}  // namespace

uint64_t FramesPresented() { return g_frames.load(std::memory_order_relaxed); }
uint64_t TickFramesPresented() { return g_tick_frames.load(std::memory_order_relaxed); }

void Start(const std::filesystem::path& user_data_root, std::function<void()> request_exit) {
  g_start = Clock::now();
  g_summary_path = user_data_root / "run-summary.json";
  g_request_exit = std::move(request_exit);
  g_thread = std::thread(Worker);
  kuf::profile::Start(user_data_root);
}

void Stop() {
  if (g_stopped.exchange(true)) {
    return;
  }
  {
    std::lock_guard lock(g_thread_mutex);
    g_stop = true;
  }
  g_thread_cv.notify_all();
  if (g_thread.joinable() && g_thread.get_id() != std::this_thread::get_id()) {
    g_thread.join();
  } else if (g_thread.joinable()) {
    g_thread.detach();
  }
  kuf::profile::Stop();
  std::ofstream out(g_summary_path);
  out << "{\n";
  out << "  \"duration_s\": " << SecondsSinceStart() << ",\n";
  out << "  \"exit_reason\": \"" << g_exit_reason << "\",\n";
  out << "  \"frames_presented\": " << g_frames.load() << ",\n";
  out << "  \"tick_frames_presented\": " << g_tick_frames.load() << ",\n";
  kuf::pacing::WriteJson(out);
  out << "  \"frame_timeline\": [";
  for (size_t i = 0; i < g_frame_timeline.size(); ++i) {
    out << (i ? ", " : "") << g_frame_timeline[i];
  }
  out << "],\n  \"guest_tick_rate\": [";
  for (size_t i = 0; i < g_guest_tick_rate.size(); ++i) {
    out << (i ? ", " : "") << static_cast<uint64_t>(g_guest_tick_rate[i]);
  }
  out << "],\n  \"cpu_percent\": [";
  for (size_t i = 0; i < g_cpu_percent.size(); ++i) {
    out << (i ? ", " : "") << static_cast<int>(g_cpu_percent[i]);
  }
  out << "],\n  \"sim_tick_timeline\": [";
  for (size_t i = 0; i < g_sim_tick_timeline.size(); ++i) {
    out << (i ? ", " : "") << g_sim_tick_timeline[i];
  }
  out << "],\n  \"game_counter_timeline\": [";
  for (size_t i = 0; i < g_game_counter_timeline.size(); ++i) {
    out << (i ? ", " : "") << g_game_counter_timeline[i];
  }
  out << "],\n  \"rss_mb\": [";
  for (size_t i = 0; i < g_rss_mb.size(); ++i) {
    out << (i ? ", " : "") << g_rss_mb[i];
  }
  out << "],\n";
  kuf::audit::WriteJson(out);
  out << "  \"files\": [\n";
  std::lock_guard files_lock(g_files_mutex);
  size_t i = 0;
  for (const auto& [path, rec] : g_files) {
    out << "    {\"path\": \"" << JsonEscape(path) << "\", \"opens\": " << rec.opens
        << ", \"failures\": " << rec.failures << ", \"first_open_s\": " << rec.first_open_s << "}"
        << (++i < g_files.size() ? "," : "") << "\n";
  }
  out << "  ]\n}\n";
}

}  // namespace kuf::telemetry

REX_HOOK_RAW(sub_8216E1B0) {
  const uint64_t begin_ns = kuf::profile::NowNs();
  __imp__sub_8216E1B0(ctx, base);
  kuf::profile::OnPresent(begin_ns, kuf::profile::NowNs());
  kuf::telemetry::g_frames.fetch_add(1, std::memory_order_relaxed);
  // Captures and input scripts count frames that show a game tick for the first time: with the original pacing
  // every frame; with decoupled pacing (M6D) a tick drawn again does not advance them, so scripts keep their timing.
  if (kuf::pacing::OnPresent()) {
    const uint64_t frame = kuf::telemetry::g_tick_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    kuf::capture::OnFramePresented(frame);
    kuf::input::script::OnFramePresented(frame);
  }
}

// File-open telemetry: the game opens files through two static copies of CreateFileA (path in r3,
// returns a handle or INVALID_HANDLE_VALUE). sub_826F38B8 dispatches NtCreateFile through XAPI's I/O
// function table, so call-site hooks on the NtCreateFile thunk cannot see it.
namespace {
void RecordOpen(const uint8_t* base, uint32_t path_address, uint32_t handle) {
  std::string path;
  if (path_address) {
    const char* p = reinterpret_cast<const char*>(base + path_address);
    path.assign(p, strnlen(p, 260));
  }
  const bool failed = handle == 0xFFFFFFFFu;
  std::lock_guard lock(kuf::telemetry::g_files_mutex);
  auto [it, inserted] = kuf::telemetry::g_files.try_emplace(path);
  if (inserted) {
    it->second.first_open_s = kuf::telemetry::SecondsSinceStart();
    REXLOG_INFO("[kuf-file] open {} -> {}", path, failed ? "FAILED" : "ok");
  }
  it->second.opens++;
  if (failed) {
    it->second.failures++;
  }
}
}  // namespace

REX_EXTERN(__imp__sub_826F38B8);  // CreateFileA (main copy, I/O function table)
REX_EXTERN(__imp__sub_823E7168);  // CreateFileA (second copy, direct NtCreateFile)

REX_HOOK_RAW(sub_826F38B8) {
  const uint32_t path = ctx.r3.u32;
  __imp__sub_826F38B8(ctx, base);
  RecordOpen(base, path, ctx.r3.u32);
}

REX_HOOK_RAW(sub_823E7168) {
  const uint32_t path = ctx.r3.u32;
  __imp__sub_823E7168(ctx, base);
  RecordOpen(base, path, ctx.r3.u32);
}
