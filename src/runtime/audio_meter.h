// Audio output decorator (M4), both parts off by default (then the SDK's SDL audio system is unchanged):
//   --kuf_audio_meter  measures every mixed frame the game submits (peak and RMS, never stored) and logs
//                      them per 100 ms of audio with the current video frame;
//   --kuf_audio_pace   paces the audio to real time the way a device does (it delays the "frame played"
//                      releases the game waits on; the game's own audio callback never blocks). A real output
//                      device does that itself; the sandbox's silent ALSA null sink does not, and the game's
//                      audio then runs far faster than real time (KNOWN_ISSUES K14). tools/run_sandboxed.sh
//                      turns it on for silent runs.
#pragma once

#include <memory>

#include <rex/audio/sdl/sdl_audio_system.h>

namespace kuf::audio {

class MeteredSDLAudioSystem final : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;
  static std::unique_ptr<rex::audio::AudioSystem> Create(rex::runtime::FunctionDispatcher* dispatcher);

  rex::X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                             rex::audio::AudioDriver** out_driver) override;
  void DestroyDriver(rex::audio::AudioDriver* driver) override;
};

}  // namespace kuf::audio
