#include "runtime/audio_meter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "runtime/telemetry.h"

REXCVAR_DEFINE_BOOL(kuf_audio_meter, false, "KUF/Debug", "Log audio output levels per 100 ms of audio");
REXCVAR_DEFINE_BOOL(kuf_audio_pace, false, "KUF/Audio",
                    "Pace audio to real time like a device: for outputs that do not (the sandbox's silent ALSA "
                    "null sink). Real devices pace themselves; leave off there");

namespace kuf::audio {

using rex::X_STATUS;
namespace {

// The SDK's mixed frame: 6 channels x 256 samples, big-endian float, one channel after another, 48 kHz.
constexpr uint32_t kFrameFloats = 6 * 256;
constexpr double kFrameSeconds = 256.0 / 48000.0;
constexpr int kFramesPerWindow = 19;  // ~101 ms

// Pacing (silent runs): a real device takes the frames the SDL driver queued at its playback rate, and each
// frame it takes releases the client's semaphore, on which the SDK's audio worker waits (outside guest code)
// before it asks the game for the next frame. The ALSA null sink takes them at once. So the inner SDL driver
// gets a private semaphore instead, and a pacing thread passes each release on to the client's semaphore one
// frame time (256 samples at 48 kHz) after the previous one. The game sees a device-paced semaphore and
// SubmitFrame never blocks. (M4 to M6A slept in SubmitFrame instead, inside the game's audio callback; the
// game then presented 290 frames per 10 s on the title and menus instead of 300: M6A profiling.)
class MeteringDriver final : public rex::audio::AudioDriver {
 public:
  MeteringDriver(rex::memory::Memory* memory, rex::audio::AudioDriver* inner, bool meter,
                 rex::thread::Semaphore* client_semaphore, std::unique_ptr<rex::thread::Semaphore> device_semaphore)
      : AudioDriver(memory),
        inner_(inner),
        meter_(meter),
        start_(Clock::now()),
        client_semaphore_(client_semaphore),
        device_semaphore_(std::move(device_semaphore)) {
    if (device_semaphore_) {
      pacer_ = std::thread([this] { Pace(); });
    }
  }

  ~MeteringDriver() override { StopPacing(); }

  rex::audio::AudioDriver* inner() const { return inner_; }

  // After the inner driver is destroyed (no more device releases).
  void StopPacing() {
    if (pacer_.joinable()) {
      stop_ = true;
      device_semaphore_->Release(1, nullptr);
      pacer_.join();
    }
  }

  void SubmitFrame(uint32_t samples_ptr) override {
    if (meter_) {
      Meter(samples_ptr);
    }
    inner_->SubmitFrame(samples_ptr);
  }

 private:
  using Clock = std::chrono::steady_clock;

  void Pace() {
    auto next = Clock::now();
    bool warned = false;
    while (true) {
      rex::thread::Wait(device_semaphore_.get(), false);
      if (stop_) {
        return;
      }
      // Real time, resynchronised after a stall instead of bursting.
      next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kFrameSeconds));
      const auto now = Clock::now();
      if (next < now - std::chrono::milliseconds(200)) {
        next = now;
      }
      std::this_thread::sleep_until(next);
      if (!client_semaphore_->Release(1, nullptr) && !warned) {
        REXLOG_WARN("[kuf-audio] pacing: client semaphore full");
        warned = true;
      }
    }
  }

  void Meter(uint32_t samples_ptr) {
    const auto* raw = memory_->TranslateVirtual<const uint32_t*>(samples_ptr);
    for (uint32_t i = 0; i < kFrameFloats; ++i) {
      uint32_t bits = __builtin_bswap32(raw[i]);
      float v;
      std::memcpy(&v, &bits, sizeof(v));
      if (std::isfinite(v)) {
        peak_ = std::max(peak_, std::fabs(double(v)));
        sum_squares_ += double(v) * v;
        channel_squares_[i / 256] += double(v) * v;
      }
    }
    if (++frames_ == kFramesPerWindow) {
      const double rms = std::sqrt(sum_squares_ / (double(kFramesPerWindow) * kFrameFloats));
      auto db = [](double x) { return x > 1e-6 ? 20.0 * std::log10(x) : -120.0; };
      double ch[6];
      for (int c = 0; c < 6; ++c) {
        ch[c] = db(std::sqrt(channel_squares_[c] / (double(kFramesPerWindow) * 256)));
        channel_squares_[c] = 0;
      }
      REXLOG_INFO("[kuf-audio] t={:.2f}s video_frame={} peak={:.1f}dBFS rms={:.1f}dBFS channels_rms_dBFS=[{:.1f}, "
                  "{:.1f}, {:.1f}, {:.1f}, {:.1f}, {:.1f}]",
                  std::chrono::duration<double>(Clock::now() - start_).count(),
                  kuf::telemetry::FramesPresented(), db(peak_), db(rms), ch[0], ch[1], ch[2], ch[3], ch[4], ch[5]);
      frames_ = 0;
      peak_ = sum_squares_ = 0;
    }
  }

  rex::audio::AudioDriver* inner_;
  const bool meter_;
  Clock::time_point start_;
  int frames_ = 0;
  double peak_ = 0, sum_squares_ = 0;
  double channel_squares_[6] = {};
  rex::thread::Semaphore* client_semaphore_;
  std::unique_ptr<rex::thread::Semaphore> device_semaphore_;  // null unless pacing
  std::atomic<bool> stop_{false};
  std::thread pacer_;
};

}  // namespace

std::unique_ptr<rex::audio::AudioSystem> MeteredSDLAudioSystem::Create(
    rex::runtime::FunctionDispatcher* dispatcher) {
  return std::make_unique<MeteredSDLAudioSystem>(dispatcher);
}

rex::X_STATUS MeteredSDLAudioSystem::CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                                                  rex::audio::AudioDriver** out_driver) {
  const bool meter = REXCVAR_GET(kuf_audio_meter), pace = REXCVAR_GET(kuf_audio_pace);
  // The device side of the pacing: the inner driver's releases go to the pacing thread first.
  std::unique_ptr<rex::thread::Semaphore> device_semaphore =
      pace ? rex::thread::Semaphore::Create(0, 1 << 16) : nullptr;
  rex::audio::AudioDriver* inner = nullptr;
  const rex::X_STATUS status =
      SDLAudioSystem::CreateDriver(index, pace ? device_semaphore.get() : semaphore, &inner);
  if (status != X_STATUS_SUCCESS || (!meter && !pace)) {
    *out_driver = inner;
    return status;
  }
  REXLOG_INFO("[kuf-audio] audio client {}:{}{}", index, meter ? " level meter" : "",
              pace ? " paced to real time" : "");
  *out_driver = new MeteringDriver(memory(), inner, meter, semaphore, std::move(device_semaphore));
  return status;
}

void MeteredSDLAudioSystem::DestroyDriver(rex::audio::AudioDriver* driver) {
  if (auto* metering = dynamic_cast<MeteringDriver*>(driver)) {
    SDLAudioSystem::DestroyDriver(metering->inner());
    metering->StopPacing();
    delete metering;
    return;
  }
  SDLAudioSystem::DestroyDriver(driver);
}

}  // namespace kuf::audio
