// Performance profile of a run (M6A), off unless --kuf_profile=true.
//
// Records, into <user data>/profile/:
//   frames.csv    per presented frame: when the present call began and how long it took (steady clock, us)
//   threads.csv   per thread and second: CPU time used (from /proc/self/task), with the thread's name
//   gpu.csv       per 100 ms: the GPU's busy percentage (amdgpu sysfs, when present; the whole GPU, so it
//                 includes other programs drawing at the same time)
//   samples.bin   with --kuf_profile_samples=true: statistical samples of where CPU time goes: a perf
//                 software CPU-clock event per thread writes the user-space instruction address every
//                 1/kuf_profile_sample_hz s of that thread's CPU time; each sample also has the thread id and
//                 the frame count (to 100 ms). Time in the kernel is not sampled. maps.txt
//                 (/proc/self/maps at the end) lets tools/profile_report.py name the function, recompiled
//                 guest code (sub_XXXXXXXX), runtime or driver library.
// Nothing here changes what the game does. Sampling costs some CPU in the kernel and the profiler thread,
// so it is a separate switch.
#pragma once

#include <cstdint>
#include <filesystem>

namespace kuf::profile {

void Start(const std::filesystem::path& user_data_root);
void Stop();

// From the present hook (render thread): when the present call began and ended (steady clock, ns).
void OnPresent(uint64_t begin_ns, uint64_t end_ns);

uint64_t NowNs();

}  // namespace kuf::profile
