// Presentation interpolation (M6D D3b-3, D4a and D4b, docs/HIGH_FPS.md sections 15-17): the camera, the player and the
// world's objects drawn between the game's 30 Hz ticks, in the render-only frames of D3b-1 (render_frames.h) and in
// the tick's own frame.
//
// Each tick's scene build records what the tick handed to the renderer: the camera (the view object's matrix and
// field of view for the game slot, view + 28 + slot * 64 and view + (slot + 191) * 4) and the player character's world
// matrix (its renderer object + 28 + slot * 64). A frame built t seconds after tick N started shows the state between
// ticks N - 1 and N at alpha = t / (1/30 s) (at most 1: never beyond tick N), written into the same slot fields before
// its scene build. The scene build (culling, shadows, lists) and the render thread's draw (view matrix, projection,
// skinning, the character's attached parts) read them from there, so the whole frame agrees. Nothing the game
// simulates is changed: the camera and the character keep their own state, and the next tick writes the other slot.
// Across a discontinuity (no consecutive ticks, loading, an event camera starting or ending, the game snapping its
// camera, a collision pull-in, another player object, a teleport) the frames show tick N as the game wrote it.
//
// World objects (M6D D4a, section 16): every other object in the tick's draw list with a world matrix of its own
// (enemies and other characters, dropped items, moving props) is interpolated the same way, from its slot matrix at
// both ticks, when it is the same object at both (index, release generation, address and class) and no cut rule
// applies (a teleport, an instant turn, a matrix that is not rigid). Characters' parts follow their parent. The
// game's own matrix is put back at the slot's next tick where the game did not write it.
//
// Skeletal poses (M6D D4b, section 17): the scene build evaluates each skinned model's pose into a palette (model
// space, from its animation layers, which advance one frame per tick). In the frames between ticks the model gets a
// palette of its own (the game's allocator, the slot's frame memory) holding the pose between the two ticks' palettes,
// each bone interpolated in its parent's space (rotation slerp), when its root transform's rules keep it and the
// layers show no switch without a crossfade; the models attached to its bones are placed from it, and its shadow uses
// it. The physics step's parts that hang from the model's bones read the tick's own palette (the pointer set back for
// the call). The tick's palette pointer is put back when the next tick into the slot starts.
//
// On with render-only frames (the frame rate above 30) unless interpolation is off (D3b-1's frames, each a copy of its
// tick); at 30 fps (no render-only frames) nothing here runs. Both can change while the game runs (D5, Reset).
#pragma once

#include <cstdint>
#include <ostream>

namespace kuf::presentation {

// What is interpolated (each part only with `camera_player`, the master switch; the frame-rate settings or their
// command-line flags, frame_pacing.cpp).
struct Interpolation {
  bool camera_player = true;  // the camera and the player character (D3b-3)
  bool objects = true;        // the world's objects (D4a)
  bool bones = true;          // skeletal poses and their physics-driven parts (D4b, D4b-3)
  bool effects = true;        // the effect shaders' clock (D4b-4)
  bool operator==(const Interpolation&) const = default;
};

// Game thread: at start, and when the frame rate or what is interpolated changes (after Reset).
void Configure(bool render_only_frames, const Interpolation& what);
bool Enabled();

// Game thread, with the render thread idle (both slots drawn): every value written into either slot for a presented
// frame (cameras, the player, world objects, palette pointers) gets the game's own back, and the history is dropped
// (the next tick starts as the first: no frame interpolated across the change). Before Configure changes anything.
void Reset(uint8_t* base);

// Game thread, first at the tick's own scene build (before anything reads the slot): world object matrices this file
// stored into the slot two ticks ago and the game has not written since get the game's own value back.
void OnTickSlotBuild(uint8_t* base, uint32_t renderer);

// Game thread, at the tick's own scene build (before sub_821F49E8; the render thread is not using the game slot):
// records tick `tick` (`world`: it marked its slot as a world frame) and writes its frame's presentation at `alpha`.
// Returns the game time the frame shows (ticks: tick - 1 + alpha, or tick), NaN when interpolation is off.
double OnTickFrame(uint8_t* base, uint32_t renderer, uint64_t tick, bool world, double alpha);

// Game thread, before a render-only build's scene build (both slot indices on the tick's slot, the render thread
// idle): writes the presentation at `alpha`. Returns the game time the frame shows, NaN when interpolation is off.
double OnBuildFrame(uint8_t* base, uint32_t renderer, double alpha);

// Game thread, at the end of the tick's own scene build (sub_821F4D00 done): the tick's skeletal poses are all
// evaluated (D4b).
void OnTickSceneDone();

// Game thread, when a tick into `slot` starts (its frames all drawn, before any of the game's code of the tick): the
// skinned models' palette pointers this file set in the slot get the tick's own back (D4b).
void OnTickStart(uint8_t* base, uint32_t slot);

// Tests (the freeze's alpha sweep): everything drawn at alpha 1 whatever the frame's alpha except the world objects'
// root transforms (SetObjectsOnly), the skeletal poses (SetBonesOnly) or the effect clock (SetEffectsOnly), so only
// they move between the sweep's frames.
void SetObjectsOnly(bool on);
void SetBonesOnly(bool on);
void SetEffectsOnly(bool on);

// Game thread, right after OnTickFrame or OnBuildFrame returned a game time: how far behind the tick's own the frame's
// effect clock is (ticks, 0 to 1: 1 - the frame's alpha; 0 with the effect clock off or in the objects-only and
// bones-only sweeps). The render thread draws the frame's effects at that moment (render_frames.cpp, D4b-4).
double EffectLag();

// For run-summary.json (one "presentation" member, with a trailing comma and newline).
void WriteJson(std::ostream& out);

}  // namespace kuf::presentation
