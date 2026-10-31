// Run telemetry: positive evidence that the game is progressing, for diagnostics and the smoke test.
//
// Counts presented frames (the game's D3D swap function) and records every file the game opens (its static
// CreateFileA), logs a progress line periodically, and writes `run-summary.json` into the user data
// directory at shutdown. With --kuf_exit_after_seconds=N, `request_exit` is called after N seconds (the app
// writes the summary and closes its window, which terminates the title the same way a user close does).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace kuf::telemetry {

void Start(const std::filesystem::path& user_data_root, std::function<void()> request_exit);

// Stops the progress thread and writes run-summary.json. Idempotent; the first call wins.
void Stop();

// Frames presented so far (any thread).
uint64_t FramesPresented();

// Presented frames that show a game tick for the first time (M6D; equal to FramesPresented() with the original
// pacing). Input scripts, captures and the game-context log count these.
uint64_t TickFramesPresented();

}  // namespace kuf::telemetry
