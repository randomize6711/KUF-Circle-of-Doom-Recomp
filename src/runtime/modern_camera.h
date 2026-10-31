// What the camera did in the current game tick, for the presentation interpolation (M6D D3b-3, presentation.cpp).
// modern_camera.cpp fills it on the game thread while the camera runs; it describes the view handed to the
// renderer in this tick (sub_825246D0) and changes nothing.
#pragma once

#include <cstdint>

#include "runtime/camera_math.h"

namespace kuf::camera {

struct TickView {
  uint64_t serial = 0;       // + 1 per view handed to the renderer (sub_825246D0): a new one in this tick
  uint32_t camera = 0;       // the camera object (camera manager - 672)
  bool event = false;        // an event or scripted camera placed the view (sub_82524530)
  bool modern = false;       // the modern camera placed the view: aimed at `pivot`
  Vec3 pivot;                // the point the modern camera's view is aimed at (the game's target on the character)
  Vec3 attach;               // the game's own camera target before its smoothing: the followed character's attach
                             // point raised by `target_height`
  double target_height = 0;  // the camera key's target offset above the attach point (camera + 464)
  double turn = 0;           // the view's horizontal turn in this tick when the player's turn was taken whole
                             // (radians, + = right, not wrapped: it may exceed half a circle); NaN otherwise: the
                             // game's own camera, or the position smoothing moved the eye a fraction of the shorter
                             // way round the target (M6B SmoothAround), so the view turned less than half a circle
  bool snapped = false;      // the game moved its camera at once: its own turn taken (the right-stick centring,
                             // a camera change), or its position smoothing off
  bool pulled_in = false;    // the collision moved the eye towards the character at once (beyond the eased
                             // correction in use, or the game's own camera's correction grew)
};

// The latest view handed to the renderer (game thread).
const TickView& LatestTickView();

}  // namespace kuf::camera
