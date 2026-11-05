#include "runtime/profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "runtime/telemetry.h"

REXCVAR_DEFINE_BOOL(kuf_profile, false, "KUF/Debug",
                    "Record frame times, per-thread CPU and GPU busy into <user data>/profile/ (M6A)");
REXCVAR_DEFINE_BOOL(kuf_profile_samples, false, "KUF/Debug",
                    "With kuf_profile: also sample where CPU time goes (perf events; tools/profile_report.py)");
REXCVAR_DEFINE_INT32(kuf_profile_sample_hz, 1000, "KUF/Debug", "Samples per second of CPU time, per thread");
REXCVAR_DEFINE_INT32(kuf_profile_stack_words, 0, "KUF/Debug",
                     "M12: with --kuf_profile_samples, also record this many 8-byte words from the top of the sampled "
                     "thread's stack (samples_stack.bin), for finding callers by stack scanning (0: off, at most 64)");

namespace kuf::profile {
namespace {

using Clock = std::chrono::steady_clock;

struct FrameRecord {
  uint64_t begin_ns;
  uint64_t end_ns;
};

struct Sample {
  uint64_t ip;
  uint32_t tid;
  uint32_t frame;
};

constexpr size_t kMaxFrames = 1 << 20;   // 9.7 h at 30 fps
constexpr size_t kMaxSamples = 1 << 24;  // 4.6 h of one busy core at 1000 Hz; later samples are dropped

bool g_enabled = false;
std::filesystem::path g_dir;
std::vector<FrameRecord> g_frames;  // written by the render thread only, read after Stop
std::atomic<size_t> g_frame_count{0};

bool g_sampling = false;
std::vector<Sample> g_samples;  // filled by the worker thread only
size_t g_stack_words = 0;        // --kuf_profile_stack_words (fixed while sampling)
std::vector<uint64_t> g_stacks;  // g_stack_words per sample, in g_samples' order (zero where none was recorded)
size_t g_lost = 0;              // samples the kernel dropped because a ring buffer was full

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;
std::atomic<bool> g_stopped{false};

#if defined(__linux__)
std::string ReadSmallFile(const std::string& path) {
  std::ifstream in(path);
  std::string s;
  std::getline(in, s, '\0');
  return s;
}

// utime + stime of one thread in clock ticks, or -1.
long ThreadTicks(const std::string& stat) {
  // Fields after the parenthesised name: state is field 3, utime 14, stime 15.
  const size_t close = stat.rfind(')');
  if (close == std::string::npos) {
    return -1;
  }
  unsigned long utime = 0, stime = 0;
  if (std::sscanf(stat.c_str() + close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime,
                  &stime) != 2) {
    return -1;
  }
  return static_cast<long>(utime + stime);
}

// One software CPU-clock event per thread (perf_event_open on our own threads: allowed without privileges at
// perf_event_paranoid <= 2, user-space addresses only). The kernel writes a sample (address, thread) into
// the thread's ring buffer every 1/hz s of that thread's CPU time; the worker drains the rings. Unlike a
// SIGPROF timer this interrupts nothing and also covers threads that block signals (SDL's). (Timers on
// thread CPU clocks are not an option: on the 7.2 kernel of the development machine they get the process
// killed with SIGKILL, even in a minimal C program.)
struct Ring {
  int fd = -1;
  uint8_t* base = nullptr;
};
constexpr size_t kRingPages = 32;  // data pages per thread (power of two)
const size_t kPage = static_cast<size_t>(sysconf(_SC_PAGESIZE));
std::map<int, Ring> g_rings;

Ring OpenRing(int tid) {
  perf_event_attr attr{};
  attr.size = sizeof(attr);
  attr.type = PERF_TYPE_SOFTWARE;
  attr.config = PERF_COUNT_SW_TASK_CLOCK;
  attr.sample_period = 1000000000ull / static_cast<uint64_t>(std::max(1, REXCVAR_GET(kuf_profile_sample_hz)));
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  if (g_stack_words) {
    attr.sample_type |= PERF_SAMPLE_STACK_USER;
    attr.sample_stack_user = static_cast<uint32_t>(g_stack_words * 8);
  }
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  Ring ring;
  ring.fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0));
  if (ring.fd < 0) {
    return ring;
  }
  void* map = mmap(nullptr, (1 + kRingPages) * kPage, PROT_READ | PROT_WRITE, MAP_SHARED, ring.fd, 0);
  if (map == MAP_FAILED) {
    close(ring.fd);
    ring.fd = -1;
    return ring;
  }
  ring.base = static_cast<uint8_t*>(map);
  return ring;
}

void DrainRing(Ring& ring, uint32_t frame) {
  auto* meta = reinterpret_cast<perf_event_mmap_page*>(ring.base);
  const uint8_t* data = ring.base + kPage;
  const uint64_t size = kRingPages * kPage;
  const uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
  uint64_t tail = meta->data_tail;
  auto copy = [&](uint64_t pos, void* out, size_t n) {
    for (size_t k = 0; k < n; ++k) {
      static_cast<uint8_t*>(out)[k] = data[(pos + k) % size];
    }
  };
  while (tail + sizeof(perf_event_header) <= head) {
    perf_event_header header;
    copy(tail, &header, sizeof(header));
    if (header.size < sizeof(header)) {
      break;
    }
    if (header.type == PERF_RECORD_SAMPLE && header.size >= sizeof(header) + 16) {
      struct {
        uint64_t ip;
        uint32_t pid, tid;
      } body;
      copy(tail + sizeof(header), &body, sizeof(body));
      if (g_samples.size() < kMaxSamples) {
        g_samples.push_back({body.ip, body.tid, frame});
        if (g_stack_words) {
          // PERF_SAMPLE_STACK_USER: u64 size, size bytes of stack from the sampled stack pointer, u64 dyn_size.
          const uint64_t at = tail + sizeof(header) + sizeof(body);
          uint64_t size = 0;
          if (header.size >= sizeof(header) + sizeof(body) + 8) copy(at, &size, 8);
          uint64_t dyn = 0;
          if (size && header.size >= sizeof(header) + sizeof(body) + 16 + size) copy(at + 8 + size, &dyn, 8);
          const size_t first = g_stacks.size();
          g_stacks.resize(first + g_stack_words, 0);
          const uint64_t n = std::min<uint64_t>({size, dyn, g_stack_words * 8});
          if (n) copy(at + 8, g_stacks.data() + first, static_cast<size_t>(n));
        }
      }
    } else if (header.type == PERF_RECORD_LOST) {
      uint64_t lost[2];
      copy(tail + sizeof(header), lost, sizeof(lost));
      g_lost += lost[1];
    }
    tail += header.size;
  }
  __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

std::string FindGpuBusyFile() {
  for (const char* card : {"card0", "card1", "card2", "card3"}) {
    std::string path = std::string("/sys/class/drm/") + card + "/device/gpu_busy_percent";
    if (access(path.c_str(), R_OK) == 0) {
      return path;
    }
  }
  return {};
}

void Worker() {
  const double t0 = NowNs() / 1e9;
  const long hz = sysconf(_SC_CLK_TCK);
  const std::string gpu_path = FindGpuBusyFile();
  std::ofstream threads(g_dir / "threads.csv");
  threads << "t_s,tid,name,cpu_s\n";
  std::ofstream gpu(g_dir / "gpu.csv");
  gpu << "t_s,frame,busy_percent\n";
  if (gpu_path.empty()) {
    REXLOG_INFO("[kuf-profile] no amdgpu gpu_busy_percent in /sys; GPU busy not recorded");
  }
  std::map<int, long> last_ticks;
  std::unique_lock lock(g_mutex);
  for (int tick = 1;; ++tick) {
    if (g_cv.wait_for(lock, std::chrono::milliseconds(100), [] { return g_stop; })) {
      return;
    }
    const double t = NowNs() / 1e9 - t0;
    if (!gpu_path.empty()) {
      const std::string busy = ReadSmallFile(gpu_path);
      gpu << t << "," << kuf::telemetry::FramesPresented() << "," << std::atoi(busy.c_str()) << "\n";
    }
    if (g_sampling) {
      const uint32_t frame = static_cast<uint32_t>(kuf::telemetry::FramesPresented());
      if (DIR* dir = opendir("/proc/self/task")) {
        while (dirent* e = readdir(dir)) {
          const int tid = std::atoi(e->d_name);
          if (tid > 0 && !g_rings.count(tid)) {
            g_rings[tid] = OpenRing(tid);
          }
        }
        closedir(dir);
      }
      for (auto& [tid, ring] : g_rings) {
        if (ring.base) {
          DrainRing(ring, frame);
        }
      }
    }
    if (tick % 10) {
      continue;
    }
    if (DIR* dir = opendir("/proc/self/task")) {
      while (dirent* e = readdir(dir)) {
        const int tid = std::atoi(e->d_name);
        if (tid <= 0) {
          continue;
        }
        const std::string base = std::string("/proc/self/task/") + e->d_name;
        const long ticks = ThreadTicks(ReadSmallFile(base + "/stat"));
        if (ticks < 0) {
          continue;
        }
        long& last = last_ticks[tid];
        if (ticks != last) {
          std::string name = ReadSmallFile(base + "/comm");
          while (!name.empty() && (name.back() == '\n' || name.back() == ',')) {
            name.pop_back();
          }
          for (char& c : name) {
            if (c == ',') c = ' ';
          }
          threads << t << "," << tid << "," << name << "," << double(ticks - last) / hz << "\n";
          last = ticks;
        }
      }
      closedir(dir);
    }
  }
}
#endif

}  // namespace

uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void OnPresent(uint64_t begin_ns, uint64_t end_ns) {
  if (!g_enabled) {
    return;
  }
  const size_t i = g_frame_count.load(std::memory_order_relaxed);
  if (i < kMaxFrames) {
    g_frames[i] = {begin_ns, end_ns};
    g_frame_count.store(i + 1, std::memory_order_release);
  }
}

void Start(const std::filesystem::path& user_data_root) {
#if defined(__linux__)
  if (!REXCVAR_GET(kuf_profile)) {
    return;
  }
  g_dir = user_data_root / "profile";
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  g_frames.resize(kMaxFrames);
  g_enabled = true;
  if (REXCVAR_GET(kuf_profile_samples)) {
    g_samples.reserve(1 << 20);
    g_stack_words = static_cast<size_t>(std::clamp(REXCVAR_GET(kuf_profile_stack_words), 0, 64));
    g_sampling = true;  // the worker opens an event on each thread within 100 ms of it appearing
  }
  g_thread = std::thread(Worker);
  REXLOG_INFO("[kuf-profile] recording into {}{}", g_dir.string(),
              g_sampling ? " (with CPU samples)" : "");
#endif
}

void Stop() {
#if defined(__linux__)
  if (!g_enabled || g_stopped.exchange(true)) {
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
  size_t failed = 0;
  for (auto& [tid, ring] : g_rings) {
    if (ring.base) {
      DrainRing(ring, static_cast<uint32_t>(kuf::telemetry::FramesPresented()));
      munmap(ring.base, (1 + kRingPages) * kPage);
    } else {
      ++failed;
    }
    if (ring.fd >= 0) {
      close(ring.fd);
    }
  }
  {
    std::ofstream out(g_dir / "frames.csv");
    out << "frame,begin_us,present_us\n";
    const size_t n = g_frame_count.load(std::memory_order_acquire);
    const uint64_t t0 = n ? g_frames[0].begin_ns : 0;
    for (size_t i = 0; i < n; ++i) {
      out << i + 1 << "," << (g_frames[i].begin_ns - t0) / 1000 << ","
          << (g_frames[i].end_ns - g_frames[i].begin_ns) / 1000 << "\n";
    }
  }
  if (g_sampling) {
    std::ofstream out(g_dir / "samples.bin", std::ios::binary);
    out.write(reinterpret_cast<const char*>(g_samples.data()),
              static_cast<std::streamsize>(g_samples.size() * sizeof(Sample)));
    if (g_stack_words) {
      std::ofstream stacks(g_dir / "samples_stack.bin", std::ios::binary);
      const uint64_t words = g_stack_words;
      stacks.write(reinterpret_cast<const char*>(&words), sizeof(words));
      stacks.write(reinterpret_cast<const char*>(g_stacks.data()),
                   static_cast<std::streamsize>(g_stacks.size() * sizeof(uint64_t)));
    }
    std::ofstream maps(g_dir / "maps.txt");
    maps << ReadSmallFile("/proc/self/maps");
    std::ofstream names(g_dir / "thread_names.csv");
    names << "tid,name\n";
    if (DIR* dir = opendir("/proc/self/task")) {
      while (dirent* e = readdir(dir)) {
        if (std::atoi(e->d_name) > 0) {
          std::string name = ReadSmallFile(std::string("/proc/self/task/") + e->d_name + "/comm");
          while (!name.empty() && name.back() == '\n') name.pop_back();
          names << e->d_name << "," << name << "\n";
        }
      }
      closedir(dir);
    }
    REXLOG_INFO("[kuf-profile] {} CPU samples from {} threads ({} lost in full ring buffers; {} threads could "
                "not be sampled)", g_samples.size(), g_rings.size() - failed, g_lost, failed);
  }
  REXLOG_INFO("[kuf-profile] {} frames recorded", g_frame_count.load());
#endif
}

}  // namespace kuf::profile
