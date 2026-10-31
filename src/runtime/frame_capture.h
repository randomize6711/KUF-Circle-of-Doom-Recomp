// Offscreen frame capture for graphics validation (M3).
//
// --kuf_capture_frames=1,30,300   capture after these presented-frame numbers (1-based)
// --kuf_capture_every=N           additionally capture every N-th frame (0 = off)
// Each capture reads back the latest guest output image through the presenter (the image the game
// presented, at guest resolution) and writes <user_data_root>/captures/frame_NNNNNN.png plus one JSON line
// in captures.jsonl: requested frame, seconds since start, width, height, and the label of a scripted capture.
// Captures are game content: they stay under build/runs and are never committed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::ui {
class Presenter;
}

namespace kuf::capture {

void Start(const std::filesystem::path& user_data_root, rex::ui::Presenter* presenter);
void Stop();

// Called by the present hook after each guest frame (frame numbers start at 1).
void OnFramePresented(uint64_t frame);

// Captures the next available frame now, recorded with `label` (the input script's `capture <label>`, for
// scenarios whose timing follows the game's state rather than fixed frame numbers).
void RequestLabeled(uint64_t frame, const std::string& label);

}  // namespace kuf::capture
