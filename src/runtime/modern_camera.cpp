// The modern gameplay camera (M6B): free horizontal and vertical look with direct mouse input and an adjustable
// controller response, on top of the game's own camera (docs/CAMERA.md). Settings: pc_settings.h (the PC
// Settings page of the game's Settings). With camera.modern_camera = false nothing here changes the game.
//
// The game camera (object X, vtable 0x82020FB0) is updated once per frame by sub_82524B60:
//   sub_82524A38(X)   reads both sticks with sub_82523D78 (radial dead zone 12800, scaled to 0..1) into
//                     X+2120.. (left) and X+2128/2132 (right; Y negated when the game's Camera option is
//                     Inverted, X+2892), then X+2068/2072 = right stick x/y
//   sub_825232B8(X)   the free camera ("FreeCam" fields of the current key, copied to X+2084..2100):
//                     horizontal mode X+180 (key+92): 1/2 = rate (X+2076 += stick * key speed), 3 = absolute;
//                     vertical mode X+184 (key+96): 3 = absolute: X+2080 eases to stick * limit (key+228/232,
//                     -0.05..0.18 rad for the action camera), so it returns to 0 when the stick is released:
//                     the original restriction and its "snap back"; 1/2 = rate
//   sub_8252C040(X)   key evaluation, smoothing (sub_8252B460), then sub_8252B8A0, which calls the virtual
//                     geometry step sub_825239F8(X, eye out, target out, eye in, target in): the eye orbits the
//                     target by X+2076 (and, with X+2056, that turn is kept in X+1680 and X+2076 cleared:
//                     continuous yaw); X+2080 raises the target by distance * sin(angle). After it the eye and
//                     target are smoothed (X+496/+508, 1/n of the way per frame), the virtual collision step
//                     (sub_82521710, when the key's Collision flag is set) moves the camera out of the scenery
//                     (keeping it parameter 248 above the ground, its height smoothed through a global), then
//                     the view is built.
// Event and scripted cameras take another branch of sub_82524B60 (no sub_82524A38 / sub_825232B8).
//
// The modern camera, for cameras with rate horizontal and absolute vertical modes (every character's gameplay
// cameras), when camera.modern_camera is on:
//   - the right stick is shaped here (own dead zone, power curve) instead of the game's dead zone;
//   - horizontal: the player's turns (stick speed * real elapsed time, mouse counts * sensitivity; no stick
//     emulation for the mouse, no acceleration, independent of the frame rate) are imposed exactly, then the
//     game's own camera motion (trailing behind the character, centring) runs as in the original;
//   - vertical: the view's angle above the horizon, changed the same way and kept when the input stops (no snap
//     back; optional gentle recentring to the game's default view); the game's X+2080 stays 0;
//   - after the game's geometry step, the eye orbits the pivot (the game's camera target) to that angle
//     (camera_math.h ApplyOrbit): below the pivot when looking up, down to just above the character's feet,
//     always aimed at the pivot. While the view is pitched the game's position smoothing is done on the game's
//     own eye before the orbit (so the orbit itself is never smoothed along a chord), and the collision's
//     height smoothing and ground clearance are relaxed; the game's collision step still applies.
// Other cameras (air attacks, transformations) keep the game's handling, with the mouse added as stick input.
// After an event camera the vertical angle starts again from the default view.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "input/game_input_hooks.h"
#include "input/kuf_input_driver.h"
#include "runtime/camera_math.h"
#include "runtime/display.h"
#include "runtime/frame_pacing.h"
#include "runtime/tick_schedule.h"
#include "runtime/modern_camera.h"
#include "runtime/pc_settings.h"

REXCVAR_DEFINE_BOOL(kuf_trace_camera_orbit, false, "KUF/Debug",
                    "Log the rendered camera every frame: eye, pivot, distance, aim off the pivot, height above the "
                    "character's feet, eye movement since the last frame (tests)");
REXCVAR_DEFINE_BOOL(kuf_trace_projection, false, "KUF/Debug",
                    "Log each new projection matrix the game builds: field of view, aspect, the matrix scales (tests)");
REXCVAR_DEFINE_BOOL(kuf_trace_modern_camera, false, "KUF/Debug",
                    "Log the modern camera's view five times a second (tests and tuning)");
REXCVAR_DEFINE_BOOL(kuf_camera_host_dt, false, "KUF/Debug",
                    "With the fixed tick: time the modern camera by the host clock between its updates, as before K43, "
                    "instead of one tick per update (tests: the state comparison must catch the difference)");


namespace {

using kuf::camera::Vec3;

// Camera object offsets (see above).
constexpr uint32_t kFreeCamEnabled = 173, kHorizontalMode = 180, kVerticalMode = 184;
constexpr uint32_t kPadSelect = 1044, kStickX = 2068, kStickY = 2072, kYaw = 2076, kPitch = 2080;
constexpr uint32_t kHorizontalLimited = 2084, kHorizontalMin = 2088, kHorizontalMax = 2092;
constexpr uint32_t kRightStickCaller = 0x82524A78;  // return address of sub_82524A38's right-stick read
constexpr uint32_t kPadA = 0x82A96054, kPadB = 0x82A96064;  // the two pad objects sub_82523D78 chooses from
constexpr uint32_t kPivotHeight = 464;   // the key's target offset (y) above the attach point: the character's feet
constexpr uint32_t kBasis = 16;            // camera: right, up, forward, position rows (4 floats each)
constexpr uint32_t kManagerOffset = 672;   // the camera manager object = camera + 672 (sub_82524B60)
constexpr uint32_t kManagerView = 176;     // the manager's copy of the camera rows (read by the game and the renderer)
constexpr uint32_t kManagerViewIndex = 164;  // the renderer object it hands the view to (-1: none)
constexpr uint32_t kManagerEvent = 893;      // set: an event camera places the view (sub_82524530)
// sub_8252B8A0's position smoothing, after the geometry step: the eye and target it hands to the collision
// (+496, +508) move 1/n of the way to the new ones per frame (n = +548, +552; off when +520 is set or n <= 0),
// unless they are within the constant at 0x8200099C of them.
constexpr uint32_t kSmoothEye = 496, kSmoothTarget = 508, kSmoothOff = 520;
constexpr uint32_t kSmoothEyeFrames = 548, kSmoothTargetFrames = 552;
constexpr uint32_t kSmoothSnapDistance = 0x8200099C;
constexpr double kGroundClearance = 30.0;     // world units the eye stays above the character's feet level
constexpr uint32_t kSmoothedHeight = 0x82A805CC;      // the collision's smoothed camera height (global)
constexpr double kHeightSmoothing = 0.3, kHeightSmoothingThreshold = 1000.0;  // its rate and pass-through limit
constexpr double kHeightFollowSeconds = 0.5;         // after vertical input, the height is not smoothed this long
constexpr uint32_t kGroundClearanceParameter = 248;  // the game's camera collision: eye above the ground below it
constexpr double kModernGroundClearance = 30.0;      // ... for the modern camera
constexpr uint32_t kGroundRayLength = 0x825218B4, kGroundRayClearance = 0x82521924;  // its reads for the ground ray
constexpr double kDefaultPivotHeight = 150.0; // if the key's target offset is unusable
constexpr double kMinOrbitDistance = 80.0;    // the orbit never comes closer to the pivot
constexpr double kDistanceEaseSeconds = 0.15; // a new distance setting is eased in with this time constant
constexpr double kObstructionReleaseSeconds = 0.3;  // after an obstruction the distance eases back out
constexpr double kClearCorrection = 0.5;            // world units: a smaller collision correction is none
// The collision step's feedback into the game's own camera (sub_82521710 with X+2056 set): the eye position it
// found (a global, 0x82ABAA70: the eye given to it, or where the scenery stopped it) pulls the game's baked eye
// (X+1680 x, X+1688 z) by the factor at 0x820009A8 (0.5) per frame.
constexpr uint32_t kCollisionEye = 0x82ABAA70, kCollisionPull = 0x820009A8;
constexpr uint32_t kBakeMode = 2056, kBakedX = 1680, kBakedZ = 1688;
constexpr uint32_t kGameplayFovCaller = 0x8252476C;  // return address of sub_825246D0's call of sub_821EA080
constexpr uint32_t kFovSlotIndex = 0x82AE760C, kFovSlot = 191;  // the view's per-frame field of view slot
constexpr uint32_t kRenderFovSlotIndex = 0x82AE7610;  // the render side's index into the same slots
constexpr uint32_t kRebuildFovCaller = 0x821EA028;     // sub_821E9FB0: stores the slot's own value again
constexpr uint32_t kEventFovCaller = 0x82524674;       // sub_82524530 (event cameras)
constexpr uint32_t kRenderProjectionCaller = 0x821E9F74;  // sub_821E9ED0: the render side's projection
constexpr int kRenderTraceFrames = 8;  // --kuf_trace_projection: render projections logged after an angle change
constexpr uint32_t kProjectionLogFrames = 30;  // --kuf_trace_projection logs each distinct projection again this often
constexpr double kGameTurn = 0.05;  // radians beyond our own correction: the game turned its camera itself
constexpr double kAbsorbed = 0.003; // radians: the game's camera has taken in our turn
constexpr int kQuietFrames = 10;    // after that many such frames the game's own camera motion runs again

struct State {
  bool active = false;      // this frame's free camera is ours (set by the integrator, used by the geometry)
  uint32_t camera = 0;
  bool integrated = false;  // the integrator ran since the last geometry step (else an event camera ran)
  bool after_event = false;
  bool have_pitch = false;
  double pitch = 0;         // the view's angle above the horizon we want (radians, + = up)
  double pitch_input = 0;   // vertical input not yet applied (radians)
  double dt = 0;            // this frame's elapsed time (seconds)
  double offset = 0;        // the angle added to the game's default view this frame (radians; trace)
  double turn = 0;          // horizontal turn input not yet applied (radians, + = to the right)
  bool have_heading = false;
  double heading = 0;       // the view direction we want (azimuth of eye -> target, radians)
  double last_correction = 0;  // the turn asked of the game last frame (radians)
  int quiet_frames = 0;        // frames without a turn, with the game's camera where we want it
  double idle = 0;          // seconds without vertical input
  std::chrono::steady_clock::time_point last;
  bool have_last = false;
  // Trace (--kuf_trace_modern_camera): the view of the last geometry step, and a frame counter.
  Vec3 view_eye, view_target, follow_target, trace_target;
  Vec3 orbit_last;
  double orbit_last_step = 0, orbit_last_distance = 0, orbit_last_requested_step = 0;
  Vec3 orbit_last_requested;
  double final_distance = 1e9;  // camera to pivot after the game's collision, last frame
  bool final_up = false;        // the view looked up then
  bool orbit_have_last = false;
  bool view_ours = false;
  uint32_t trace_frames = 0;
  // This frame's view: the game's own (unpitched) eye/target and ours, from the geometry step.
  bool pitched = false;
  Vec3 game_eye, game_target, eye, target;
  double delta = 0;  // view angle added (radians)
  // While the view is pitched the game's position smoothing is done here, on the game's own eye (see the
  // geometry hook): its smoothed eye, which the camera object then no longer holds.
  bool smoothing = false;
  Vec3 smooth_eye;
  // The camera distance setting as a factor on the game's own distance (eased to changes).
  bool have_scale = false;
  double scale = 1.0;
  double obstruction = 1.0;  // the fraction of the way to our eye the collision's global was (trace)
  bool have_allowed = false;
  Vec3 correction;           // the collision's correction in use (eased back when it shrinks)
  Vec3 last_dir;             // pivot -> the eye the collision was given, last frame
  double allowed = 1.0;      // distance in use / requested (trace)
  bool rendering = false;    // inside sub_825246D0 for our view
  int collision_hit = 0;     // the collision step's result this frame (trace)
  // --kuf_trace_projection: the projections logged (each distinct one once; again once the gameplay view runs).
  // The projection builder runs on the game thread and the render thread: the trace's set is shared (M6C fix: a
  // race between the two crashed a traced 21:9 run).
  std::mutex projections_mutex;
  std::set<std::array<double, 4>> projections;
  bool rendered_before = false, gameplay_started = false;
  uint32_t projection_frames = 0;
  double traced_fov = 0, traced_key_fov = 0;
  // Which call last wrote each field of view slot (the game side's writes, numbered), and the render side's
  // projections logged after the gameplay view's angle changes (with Modern Camera Off too).
  uint32_t slot_write[2] = {0, 0}, slot_caller[2] = {0, 0}, fov_writes = 0;
  float slot_value[2] = {0, 0}, gameplay_fov = 0;
  int render_trace_left = 0, fov_changes_traced = 0;
  // The last render-side projection drawn (logged with an angle change: with the fixed tick, M6D, the render side
  // has usually drawn the frame written before the change already when the change is written).
  std::mutex last_render_mutex;
  std::string last_render;
  // The rendered camera (basis and position as the game built it from our view), while valid.
  bool render_valid = false;
  float render[16] = {};
  // For the presentation interpolation (modern_camera.h): this tick's view, and what happened since the last one.
  kuf::camera::TickView tick_view;
  double tick_turn = 0;         // the player's turns applied since the last view (radians)
  bool tick_smoothed = false;   // the position smoothing moved the eye around the target since the last view
  bool snapped_pending = false; // the game turned its own camera since the last view
  bool pulled_pending = false;  // the collision pulled the camera in at once since the last view
  double game_correction = 0;   // the game's own camera: its collision's correction last frame (world units)
};
State g;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t bits = Load32(base, a);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Store32(base, a, bits);
}
Vec3 LoadVec(const uint8_t* base, uint32_t a) { return {LoadF(base, a), LoadF(base, a + 4), LoadF(base, a + 8)}; }
void StoreVec(uint8_t* base, uint32_t a, const Vec3& v) {
  StoreF(base, a, float(v.x));
  StoreF(base, a + 4, float(v.y));
  StoreF(base, a + 8, float(v.z));
}
int16_t LoadS16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, base + a, 2);
  return int16_t(__builtin_bswap16(v));
}

// The time this camera update stands for. With the fixed tick (M6D) the camera updates once per game tick, so that is
// one tick: the host clock between updates jitters with the host's load (5-111 ms measured with four runs at once)
// and made the camera, and through camera-relative movement the player, differ between runs (K43). The original
// pacing (one tick per presented frame) keeps the host clock, as approved in M6B.
double ElapsedSeconds() {
  const auto now = std::chrono::steady_clock::now();
  double dt = 1.0 / 30;
  if (kuf::pacing::FixedTick() && !REXCVAR_GET(kuf_camera_host_dt)) {
    dt = kuf::pacing::TickSchedule::kTickNs / 1e9;
  } else if (g.have_last) {
    dt = std::chrono::duration<double>(now - g.last).count();
    if (dt > 0.25) dt = 1.0 / 30;  // the game was paused or stalled: one ordinary frame
  }
  g.last = now;
  g.have_last = true;
  return std::max(dt, 0.0);
}

void MouseLook(bool modern, double* dx, double* dy) {
  *dx = *dy = 0;
  if (auto* driver = kuf::input::KufInputDriver::Instance()) {
    driver->SetDirectMouseLook(modern);
    if (modern) {
      driver->TakeMouseLook(dx, dy);
    }
  }
}

}  // namespace

// A place for a debugger breakpoint once the gameplay camera runs (investigations with tools/menu_test.py --gdb).
extern "C" __attribute__((noinline, used)) void kuf_camera_gdb_anchor(uint32_t camera) { asm volatile("" ::"r"(camera)); }

REX_EXTERN(__imp__sub_82523D78);
REX_EXTERN(sub_826B6EF0);  // the pad's stick values: (pad, -1, stick) -> pointer to {int16 x, int16 y}
REX_HOOK_RAW(sub_82523D78) {  // read one stick for the camera: (camera, stick, &x, &y)
  const uint32_t camera = ctx.r3.u32, stick = ctx.r4.u32, out_x = ctx.r5.u32, out_y = ctx.r6.u32;
  if (stick != 1 || uint32_t(ctx.lr) != kRightStickCaller || !kuf::pc::Current().camera.modern) {
    __imp__sub_82523D78(ctx, base);
    return;
  }
  const auto settings = kuf::pc::Current().camera;
  ctx.r3.u64 = base[camera + kPadSelect] ? kPadA : kPadB;
  ctx.r4.u64 = 0xFFFFFFFFu;
  ctx.r5.u64 = stick;
  sub_826B6EF0(ctx, base);
  const uint32_t values = ctx.r3.u32;
  const auto shaped = kuf::camera::ShapeStick(LoadS16(base, values), LoadS16(base, values + 2),
                                              settings.stick_deadzone / 100.0);
  StoreF(base, out_x, float(shaped.x));
  StoreF(base, out_y, float(shaped.y));
}

REX_EXTERN(__imp__sub_825232B8);
REX_HOOK_RAW(sub_825232B8) {  // the free camera's per-frame update: (camera)
  const uint32_t camera = ctx.r3.u32;
  const auto settings = kuf::pc::Current().camera;
  double mouse_dx, mouse_dy;
  MouseLook(settings.modern, &mouse_dx, &mouse_dy);
  g.active = false;
  if (!settings.modern) {
    __imp__sub_825232B8(ctx, base);
    return;
  }
  const double dt = ElapsedSeconds();
  if (g.after_event) {
    g.have_pitch = false;  // a cut scene or scripted camera ran: start again from the game's view
    g.have_heading = false;
    if (REXCVAR_GET(kuf_trace_modern_camera)) {
      REXLOG_INFO("[kuf-cam] event camera ended: the view starts again from the game's");
    }
    g.after_event = false;
  }
  g.integrated = true;
  static int frames = 0;
  if (++frames == 300) {
    kuf_camera_gdb_anchor(camera);
  }
  const double invert = settings.invert_mouse_y ? -1.0 : 1.0;
  const double mouse_yaw = mouse_dx * kuf::camera::MouseRadiansPerCount(settings.mouse_horizontal);
  const double mouse_pitch = -mouse_dy * invert * kuf::camera::MouseRadiansPerCount(settings.mouse_vertical);
  const double speed = kuf::camera::StickRadiansPerSecond(settings.stick_sensitivity);
  const double stick_x = LoadF(base, camera + kStickX), stick_y = LoadF(base, camera + kStickY);
  const uint32_t horizontal = Load32(base, camera + kHorizontalMode), vertical = Load32(base, camera + kVerticalMode);
  const bool free_camera = base[camera + kFreeCamEnabled] != 0;

  if (!free_camera || !(horizontal == 1 || horizontal == 2) || vertical != 3) {
    // Not one of the gameplay cameras: the game's own handling, the mouse added as stick input.
    if (dt > 0 && speed > 0) {
      StoreF(base, camera + kStickX, float(std::clamp(stick_x + mouse_yaw / (speed * dt), -1.0, 1.0)));
      StoreF(base, camera + kStickY,
             float(std::clamp(stick_y + mouse_pitch / (speed * kuf::camera::kStickPitchRatio * dt), -1.0, 1.0)));
    }
    __imp__sub_825232B8(ctx, base);
    return;
  }

  // Horizontal: the view direction we want, turned by the stick (per second of real time) and the mouse (per
  // count). The geometry step makes the game's camera point there (see sub_825239F8 below).
  g.turn += stick_x * speed * dt + mouse_yaw;

  // Vertical: our own angle, kept when the input stops; the geometry step limits it.
  const double pitch_input = stick_y * speed * kuf::camera::kStickPitchRatio * dt + mouse_pitch;
  g.pitch_input += kuf::camera::LimitUpward(pitch_input, g.final_up, g.final_distance, dt);
  g.idle = std::abs(pitch_input) > 1e-6 ? 0 : g.idle + dt;
  g.dt = dt;
  StoreF(base, camera + kPitch, 0.0f);
  g.active = true;
  g.camera = camera;
}

REX_EXTERN(__imp__sub_825239F8);
REX_HOOK_RAW(sub_825239F8) {  // the free camera's geometry: (camera, eye out, target out, eye in, target in)
  const uint32_t camera = ctx.r3.u32, eye_out = ctx.r4.u32, target_out = ctx.r5.u32;
  const bool ours = g.active && camera == g.camera;
  if (!g.integrated) {
    if (!g.after_event && REXCVAR_GET(kuf_trace_modern_camera)) {
      REXLOG_INFO("[kuf-cam] event camera: the game's own view");
    }
    g.after_event = true;  // an event camera is running (the gameplay path did not run this frame)
  }
  g.integrated = false;
  g.active = false;
  g.pitched = false;
  if (ours) {
    // The game turns its camera by X+2076 here, but its smoothing pulls the next frame's camera most of the way
    // back (only about 40 % of a turn stays, over several frames). So the turn asked for is the whole remaining
    // difference between the direction the game is about to show and ours: what the game shows is always our
    // direction, at once, and its own state converges behind it.
    const Vec3 eye = LoadVec(base, ctx.r6.u32), target = LoadVec(base, ctx.r7.u32);
    const double shown = std::atan2(target.x - eye.x, target.z - eye.z);
    // The game's pull-back after our correction is never larger than that correction. A bigger difference is
    // the game turning its own camera (the right-stick click "centre the camera behind you", camera changes):
    // then its direction (and its default height) is taken, as the original camera would show it.
    const double deviation = std::remainder(g.heading - shown, 2 * M_PI);
    if (g.have_heading && std::abs(deviation) > std::abs(g.last_correction) + kGameTurn) {
      g.snapped_pending = true;
      g.have_heading = false;
      g.have_pitch = false;
      if (REXCVAR_GET(kuf_trace_modern_camera)) {
        REXLOG_INFO("[kuf-cam] the game turned its camera by {:.1f} deg: taken", deviation * -180.0 / M_PI);
      }
    }
    // Once the game has taken in the player's last turn and no new turn comes, the camera is the game's own
    // again: it trails behind the moving character as the original does (the character's routes depend on it).
    if (!g.have_heading || g.quiet_frames >= kQuietFrames) {
      g.heading = shown;
      g.have_heading = true;
    }
    const bool turning = g.turn != 0;
    g.tick_turn += g.turn;
    g.heading = std::remainder(g.heading + g.turn, 2 * M_PI);
    g.turn = 0;
    const double correction = std::remainder(g.heading - shown, 2 * M_PI);
    g.quiet_frames = !turning && std::abs(correction) < kAbsorbed ? g.quiet_frames + 1 : 0;

    StoreF(base, camera + kYaw, float(correction));
    g.last_correction = correction;
  } else {
    g.final_up = false;
    g.last_correction = 0;
    g.have_heading = false;  // another camera: take the game's direction again when ours resumes
    g.have_pitch = false;
    g.pitch_input = 0;
    g.turn = 0;
    g.have_allowed = false;
  }
  __imp__sub_825239F8(ctx, base);
  g.view_eye = LoadVec(base, eye_out);  // for the trace: the view this frame (ours replaces it below)
  g.view_target = LoadVec(base, target_out);
  g.follow_target = g.view_target;  // the game's own target (it follows the player character)
  g.view_ours = ours;
  if (!ours) {
    g.smoothing = false;
    return;
  }
  const auto settings = kuf::pc::Current().camera;
  g.game_target = LoadVec(base, target_out);
  g.game_eye = LoadVec(base, eye_out);
  // The player's camera distance (PC Settings, percent of the game's own): eased to a new setting in a fraction
  // of a second, so a change previews at once without a jump.
  const double wanted_scale = settings.distance / 100.0;
  if (!g.have_scale) {
    g.scale = wanted_scale;
    g.have_scale = true;
  }
  g.scale += (wanted_scale - g.scale) * (1.0 - std::exp(-g.dt / kDistanceEaseSeconds));
  if (std::abs(wanted_scale - g.scale) < 1e-4) {
    g.scale = wanted_scale;
  }
  // The game smooths the camera's position after this step (sub_8252B8A0: eye and target 1/n of the way per
  // frame, in straight lines). With the orbit that cut a chord: on a fast vertical move the eye lagged behind
  // the orbit and passed close to the character; the same for a fast turn (at a high sensitivity a 150-degree
  // turn in one frame swung the camera through the character). So the same smoothing is done here: the target
  // as the game does it, and the eye's offset from the target by the same 1/n per frame, but around the target
  // (direction, horizontal distance and height) instead of straight across. For motion without a turn that is
  // the game's smoothing exactly; the camera still trails the character and lags a turn as much as before. The
  // orbit is placed around the smoothed eye and target, and the game's smoothing is given the result as its
  // state, so it leaves it as it is.
  {
    const bool off = base[camera + kSmoothOff] != 0;
    const double snap = LoadF(base, kSmoothSnapDistance);
    const auto frames_at = [&](uint32_t at) { return off ? 0 : int(Load32(base, camera + at)); };
    const Vec3 last_target = LoadVec(base, camera + kSmoothTarget);
    if (!g.smoothing) {
      g.smooth_eye = LoadVec(base, camera + kSmoothEye);
    }
    g.smoothing = true;
    Vec3 target = g.game_target;
    const int target_frames = frames_at(kSmoothTargetFrames);
    const Vec3 dt{g.game_target.x - last_target.x, g.game_target.y - last_target.y, g.game_target.z - last_target.z};
    if (target_frames > 0 && std::hypot(dt.x, dt.y, dt.z) > snap) {
      target = {last_target.x + dt.x / target_frames, last_target.y + dt.y / target_frames,
                last_target.z + dt.z / target_frames};
    }
    // Both relative to the new smoothed target: without a turn this is the game's eye + (new - eye) / n exactly.
    const Vec3 from{g.smooth_eye.x - target.x, g.smooth_eye.y - target.y, g.smooth_eye.z - target.z};
    const Vec3 to{g.game_eye.x - target.x, g.game_eye.y - target.y, g.game_eye.z - target.z};
    Vec3 offset = to;
    const int eye_frames = frames_at(kSmoothEyeFrames);
    const Vec3 de{g.game_eye.x - g.smooth_eye.x, g.game_eye.y - g.smooth_eye.y, g.game_eye.z - g.smooth_eye.z};
    if (eye_frames > 0 && std::hypot(de.x, de.y, de.z) > snap) {
      offset = kuf::camera::SmoothAround(from, to, 1.0 / eye_frames);
      g.tick_smoothed = true;  // the eye turned part of the shorter way (presentation: no turn reported)
    }
    g.smooth_eye = {target.x + offset.x, target.y + offset.y, target.z + offset.z};
    g.game_eye = g.smooth_eye;
    g.game_target = target;
  }
  // The view's angle: ours, held absolute (the game's default view moves a little as its camera settles).
  // Until the player moves the view vertically it is the game's own (its cameras frame scenes differently);
  // after that it is held, and recentring hands it back to the game once it is home.
  const double base_pitch = kuf::camera::ViewPitch(g.game_eye, g.game_target);
  if (g.pitch_input != 0) {
    if (!g.have_pitch) {
      g.pitch = base_pitch;
    }
    g.have_pitch = true;
  }
  if (!g.have_pitch) {
    g.pitch = base_pitch;
  }
  g.pitch += g.pitch_input;
  g.pitch_input = 0;
  if (settings.auto_recenter && g.have_pitch) {
    g.pitch = base_pitch + kuf::camera::Recenter(g.pitch - base_pitch, g.idle, g.dt);
    if (std::abs(g.pitch - base_pitch) < 1e-3) {
      g.have_pitch = false;
    }
  }
  // How far below the pivot the eye may orbit: the pivot's height above the character's feet (the key's target
  // offset), less a clearance, so a low orbit stays above the ground the character stands on.
  double pivot_height = LoadF(base, camera + kPivotHeight);
  if (!(pivot_height > 20 && pivot_height < 1000)) {
    pivot_height = kDefaultPivotHeight;
  }
  const Vec3 scaled_eye{g.game_target.x + (g.game_eye.x - g.game_target.x) * g.scale,
                        g.game_target.y + (g.game_eye.y - g.game_target.y) * g.scale,
                        g.game_target.z + (g.game_eye.z - g.game_target.z) * g.scale};
  const auto view = kuf::camera::ApplyOrbit(scaled_eye, g.game_target, g.pitch - base_pitch,
                                            kuf::camera::Radians(settings.vertical_range),
                                            pivot_height - kGroundClearance, kMinOrbitDistance);
  g.offset = view.offset;
  g.pitch = base_pitch + view.offset;  // within the limits: no dead zone when turning back
  if (std::abs(view.offset) < 1e-9) {
    g.pitch = base_pitch;
  }
  const Vec3 eye = view.eye, pivot = view.target;
  g.eye = eye;
  g.target = pivot;
  g.delta = kuf::camera::ViewPitch(eye, pivot) - kuf::camera::ViewPitch(g.game_eye, g.game_target);
  g.pitched = true;
  StoreVec(base, eye_out, eye);
  StoreVec(base, target_out, view.target);
  if (g.smoothing) {
    StoreVec(base, camera + kSmoothEye, eye);
    StoreVec(base, camera + kSmoothTarget, view.target);
  }
  g.view_eye = eye;
  g.view_target = pivot;
}

namespace {

// Five times a second with --kuf_trace_modern_camera: the rendered view's angles and how far the game's collision
// moved the camera in (distance ratio < 1). Tests read these lines (tests/camera/*.toml, log_value).
// Every frame with --kuf_trace_camera_orbit: the camera the renderer gets (after the game's collision), against
// the pivot (the game's camera target on the character).
void TraceOrbit(const uint8_t* base, uint32_t camera) {
  if (!REXCVAR_GET(kuf_trace_camera_orbit)) {
    return;
  }
  Vec3 eye, forward;
  if (g.render_valid) {
    forward = {g.render[8], g.render[9], g.render[10]};
    eye = {g.render[12], g.render[13], g.render[14]};
  } else {
    forward = LoadVec(base, camera + kBasis + 32);
    eye = LoadVec(base, camera + kBasis + 48);
  }
  const Vec3& pivot = g.game_target;
  const Vec3 to{pivot.x - eye.x, pivot.y - eye.y, pivot.z - eye.z};
  const double distance = std::hypot(to.x, to.y, to.z);
  const double flen = std::hypot(forward.x, forward.y, forward.z);
  double off = 0;
  if (distance > 1e-3 && flen > 1e-6) {
    const double c = (to.x * forward.x + to.y * forward.y + to.z * forward.z) / (distance * flen);
    off = std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / M_PI;
  }
  const double feet = pivot.y - LoadF(base, camera + kPivotHeight);
  const double step = g.orbit_have_last ? std::hypot(eye.x - g.orbit_last.x, eye.y - g.orbit_last.y, eye.z - g.orbit_last.z) : 0;
  const double jerk = g.orbit_have_last ? std::abs(step - g.orbit_last_step) : 0;
  const double distance_change = g.orbit_have_last ? distance - g.orbit_last_distance : 0;
  // The camera as placed (before the scenery's collision): its movement this frame and the change of that.
  const double requested_step = g.orbit_have_last ? std::hypot(g.view_eye.x - g.orbit_last_requested.x,
                                                               g.view_eye.y - g.orbit_last_requested.y,
                                                               g.view_eye.z - g.orbit_last_requested.z)
                                                  : 0;
  const double requested_jerk = g.orbit_have_last ? std::abs(requested_step - g.orbit_last_requested_step) : 0;
  g.orbit_last_requested = g.view_eye;
  g.orbit_last_requested_step = requested_step;
  g.orbit_last = eye;
  g.orbit_last_step = step;
  g.orbit_last_distance = distance;
  g.orbit_have_last = true;
  REXLOG_INFO("[kuf-cam] orbit: pitch {:.2f} deg, distance {:.1f}, aim off pivot {:.2f} deg, eye above feet {:.1f}, "
              "step {:.2f}, jerk {:.2f}, distance change {:.2f}, off orbit {:.2f}, eye ({:.1f},{:.1f},{:.1f}), pivot "
              "({:.1f},{:.1f},{:.1f}), {}, requested pitch {:.2f} distance {:.1f}, scenery {}, requested step {:.2f} "
              "change {:.2f}",
              std::asin(std::clamp(forward.y / std::max(flen, 1e-9), -1.0, 1.0)) * 180.0 / M_PI, distance, off,
              eye.y - feet, step, jerk, distance_change,
              std::hypot(eye.x - g.view_eye.x, eye.y - g.view_eye.y, eye.z - g.view_eye.z), eye.x, eye.y, eye.z,
              pivot.x, pivot.y, pivot.z, g.view_ours ? "modern" : "game",
              kuf::camera::ViewPitch(g.view_eye, g.view_target) * 180.0 / M_PI,
              std::hypot(g.view_eye.x - pivot.x, g.view_eye.y - pivot.y, g.view_eye.z - pivot.z), g.collision_hit,
              requested_step, requested_jerk);
}

void Trace(const uint8_t* base, uint32_t camera) {
  if (!REXCVAR_GET(kuf_trace_modern_camera) || ++g.trace_frames % 6 != 0) {
    return;
  }
  const Vec3 position = LoadVec(base, camera + kBasis + 48);
  const auto distance = [](const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); };
  const double wanted = distance(g.view_eye, g.view_target);
  const double ratio = wanted > 1e-3 ? distance(position, g.view_target) / wanted : 1.0;
  const double yaw = std::atan2(g.view_target.x - g.view_eye.x, g.view_target.z - g.view_eye.z) * 180.0 / M_PI;
  REXLOG_INFO("[kuf-cam] view: pitch {:.2f} deg, yaw {:.2f} deg, distance ratio {:.3f}, {}, offset {:.4f}, "
              "distance {:.1f}",
              kuf::camera::ViewPitch(g.view_eye, g.view_target) * 180.0 / M_PI, yaw, ratio,
              g.view_ours ? "modern" : "game", g.offset, distance(position, g.view_target));
  // The camera follows the player character: when its target has moved, the direction it moved in, against
  // the direction the camera looks (the character walks where the player sees "forward").
  const Vec3& now = g.follow_target;
  const double moved = std::hypot(now.x - g.trace_target.x, now.z - g.trace_target.z);
  if (moved > 20 && moved < 2000) {
    const double heading = std::atan2(now.x - g.trace_target.x, now.z - g.trace_target.z) * 180.0 / M_PI;
    REXLOG_INFO("[kuf-cam] target moved {:.0f}: heading {:.2f} deg, relative to the view {:.2f} deg", moved, heading,
                std::remainder(heading - yaw, 360.0));
  }
  g.trace_target = now;
}

}  // namespace

// After the camera update: the game has built the camera rows (camera+16..) from our view, with its collision.
// The game itself reads them (through the manager's copy) for camera selection, the player character, saves and
// more, all made for the original orientation; so they get the game's own orientation back, and only the view
// handed to the renderer (sub_825246D0) is ours. (The horizontal direction is the game's own: the player
// character moves relative to the camera the player sees.)
REX_EXTERN(__imp__sub_8252C040);
REX_HOOK_RAW(sub_8252C040) {  // the camera's per-frame update: (camera) -> camera rows
  const uint32_t camera = ctx.r3.u32;
  __imp__sub_8252C040(ctx, base);
  const uint32_t result = ctx.r3.u32;
  Trace(base, camera);
  if (!g.pitched || camera != g.camera) {
    g.render_valid = false;
    TraceOrbit(base, camera);
    return;
  }
  g.pitched = false;
  for (int i = 0; i < 16; ++i) {
    g.render[i] = LoadF(base, camera + kBasis + 4 * i);
  }
  g.render_valid = true;
  TraceOrbit(base, camera);
  kuf::camera::Basis pitched{LoadVec(base, camera + kBasis), LoadVec(base, camera + kBasis + 16),
                             LoadVec(base, camera + kBasis + 32), LoadVec(base, camera + kBasis + 48)};
  auto game = kuf::camera::UnpitchBasis(pitched, g.delta);
  // Position: the game's own eye, moved towards its target as far as the collision moved ours.
  const auto length = [](const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); };
  const double ours = length(g.eye, g.target);
  const double ratio = ours > 1e-3 ? std::clamp(length(pitched.position, g.target) / ours, 0.0, 1.0) : 1.0;
  // Where the game's collision left the camera: when the ground pushes a low orbit in, looking further up stops
  // gently before the camera reaches the character (next frame's input, LimitUpward).
  g.final_distance = length(pitched.position, g.target);
  g.final_up = kuf::camera::ViewPitch(g.eye, g.target) > 0;
  game.position = {g.game_target.x + (g.game_eye.x - g.game_target.x) * ratio,
                   g.game_target.y + (g.game_eye.y - g.game_target.y) * ratio,
                   g.game_target.z + (g.game_eye.z - g.game_target.z) * ratio};
  StoreVec(base, camera + kBasis, game.right);
  StoreVec(base, camera + kBasis + 16, game.up);
  StoreVec(base, camera + kBasis + 32, game.forward);
  StoreVec(base, camera + kBasis + 48, game.position);
  ctx.r3.u64 = result;
}

// The renderer's view (sub_825246D0 builds it from the manager's copy and hands it to the GPU side): ours,
// for this call only.
REX_EXTERN(__imp__sub_825246D0);
REX_HOOK_RAW(sub_825246D0) {  // (camera manager)
  const uint32_t manager = ctx.r3.u32;
  if (Load32(base, manager + kManagerViewIndex) != 0xFFFFFFFFu) {
    auto& v = g.tick_view;
    ++v.serial;
    v.camera = manager - kManagerOffset;
    v.event = base[manager + kManagerEvent] != 0;
    v.modern = !v.event && g.render_valid && manager == g.camera + kManagerOffset;
    v.pivot = g.target;
    v.attach = g.follow_target;
    v.target_height = LoadF(base, v.camera + kPivotHeight);
    v.turn = v.modern && !g.tick_smoothed ? g.tick_turn : NAN;
    v.snapped = g.snapped_pending || base[v.camera + kSmoothOff] != 0;
    v.pulled_in = g.pulled_pending;
  }
  g.tick_turn = 0;
  g.tick_smoothed = false;
  g.snapped_pending = false;
  g.pulled_pending = false;
  if (++g.projection_frames % kProjectionLogFrames == 0) {
    std::lock_guard lock(g.projections_mutex);
    g.projections.clear();  // the projection trace: each distinct one again (tests check windows of the log)
  }
  if (!g.render_valid || manager != g.camera + kManagerOffset) {
    __imp__sub_825246D0(ctx, base);
    return;
  }
  float saved[16];
  for (int i = 0; i < 16; ++i) {
    saved[i] = LoadF(base, manager + kManagerView + 4 * i);
    StoreF(base, manager + kManagerView + 4 * i, g.render[i]);
  }
  if (!g.rendered_before) {
    g.rendered_before = true;
    g.gameplay_started = true;
  }

  g.rendering = true;
  __imp__sub_825246D0(ctx, base);
  g.rendering = false;
  for (int i = 0; i < 16; ++i) {
    StoreF(base, manager + kManagerView + 4 * i, saved[i]);
  }
}

// Field of view (PC Settings). sub_825246D0 hands the camera's vertical angle (X+528: the key's own, smoothed
// between keys) to sub_821EA080(view, f1), which stores it in the view's per-frame slot (view + 4 * (index +
// 191), index = 0x82AE760C) for the projection (sub_826FC108: a D3DX-style perspective matrix from the vertical
// angle and the viewport's aspect) and the culling, and derives a draw-distance term from it. The slot is set
// to the player's angle after the call, for the modern gameplay camera only (the call from sub_825246D0, not the
// event cameras' sub_82524530); the draw-distance term stays the game's. HUD and menus use an orthographic
// projection that does not read the slot.
// --kuf_trace_projection: which game-side write each render-side projection reads (the slots are written with
// index 0x82AE760C and read by the render side with its own index, 0x82AE7610); logged for a few render frames
// after the gameplay view's angle changes.
void TraceFovSlotWrite(uint8_t* base, uint32_t view, uint32_t caller) {
  if (!REXCVAR_GET(kuf_trace_projection) || caller == kRebuildFovCaller) {
    return;
  }
  const uint32_t index = Load32(base, kFovSlotIndex), slot = index & 1;
  g.slot_write[slot] = ++g.fov_writes;
  g.slot_caller[slot] = caller;
  g.slot_value[slot] = LoadF(base, view + 4 * (index + kFovSlot));
  if (caller == kGameplayFovCaller && g.slot_value[slot] != g.gameplay_fov) {
    if (g.gameplay_fov != 0 && g.fov_changes_traced < 4) {
      ++g.fov_changes_traced;
      g.render_trace_left = kRenderTraceFrames;
      REXLOG_INFO("[kuf-cam] projection: gameplay view write #{} (slot {}): fov {:.4f} rad (was {:.4f})", g.fov_writes,
                  slot, g.slot_value[slot], g.gameplay_fov);
      std::lock_guard lock(g.last_render_mutex);
      if (!g.last_render.empty()) {
        REXLOG_INFO("[kuf-cam] {}, drawn before the change", g.last_render);
      }
    }
    g.gameplay_fov = g.slot_value[slot];
  }
}

REX_EXTERN(__imp__sub_821EA080);
REX_HOOK_RAW(sub_821EA080) {  // (view, f1 = vertical field of view)
  const uint32_t view = ctx.r3.u32, caller = uint32_t(ctx.lr);
  const double key = ctx.f1.f64;
  __imp__sub_821EA080(ctx, base);
  const auto settings = kuf::pc::Current().camera;
  if (!g.rendering || caller != kGameplayFovCaller || !settings.modern) {
    TraceFovSlotWrite(base, view, caller);
    return;
  }
  const double fov = kuf::camera::VerticalFovFor(key, settings.field_of_view);
  if (fov != key) {
    StoreF(base, view + 4 * (Load32(base, kFovSlotIndex) + kFovSlot), float(fov));
  }
  TraceFovSlotWrite(base, view, caller);
  if (REXCVAR_GET(kuf_trace_modern_camera) && (std::abs(fov - g.traced_fov) > 1e-6 || key != g.traced_key_fov)) {
    g.traced_fov = fov;
    g.traced_key_fov = key;
    REXLOG_INFO("[kuf-cam] field of view: key {:.4f} rad, used {:.4f} rad ({:.2f} degrees vertical, {:.2f} "
                "horizontal at 16:9), setting {}",
                key, fov, fov * 180.0 / M_PI, kuf::camera::HorizontalFov(fov, 16.0 / 9.0) * 180.0 / M_PI,
                settings.field_of_view);
  }
}

// The projection matrix builder (--kuf_trace_projection): each distinct set of inputs once a second of gameplay
// (once before), with the resulting scales, so
// tests see the projection the renderer gets (gameplay, and that HUD/menu frames do not change).
REX_EXTERN(__imp__sub_826FC108);
REX_HOOK_RAW(sub_826FC108) {  // (matrix out, f1 = vertical fov, f2 = aspect, f3 = near, f4 = far)
  const uint32_t out = ctx.r3.u32;
  kuf::display::BeforePerspective(ctx, base);  // the 3D views' aspect for the output (M6C)
  const double fov = ctx.f1.f64, aspect = ctx.f2.f64, near_plane = ctx.f3.f64, far_plane = ctx.f4.f64;
  __imp__sub_826FC108(ctx, base);
  if (!REXCVAR_GET(kuf_trace_projection)) {
    return;
  }
  if (uint32_t(ctx.lr) == kRenderProjectionCaller) {
    const uint32_t slot = Load32(base, kRenderFovSlotIndex) & 1, writer = g.slot_caller[slot];
    std::string line = fmt::format(
        "projection: render, fov {:.4f} rad, slot {} from write #{} ({}, {:.4f})", fov, slot, g.slot_write[slot],
        writer == kGameplayFovCaller ? "gameplay view" : writer == kEventFovCaller ? "event camera" : "other",
        g.slot_value[slot]);
    if (g.render_trace_left > 0) {
      --g.render_trace_left;
      REXLOG_INFO("[kuf-cam] {}", line);
    }
    std::lock_guard lock(g.last_render_mutex);
    g.last_render = std::move(line);
  }
  std::lock_guard lock(g.projections_mutex);
  if (g.gameplay_started) {  // each distinct projection once before gameplay, and again once in gameplay
    g.gameplay_started = false;
    g.projections.clear();
    REXLOG_INFO("[kuf-cam] projection: gameplay view");
  }
  if (!g.projections.insert({fov, aspect, near_plane, far_plane}).second) {
    return;
  }
  REXLOG_INFO("[kuf-cam] projection: fov {:.4f} rad aspect {:.4f} near {:.1f} far {:.1f}: x scale {:.4f} y scale "
              "{:.4f} (expected {:.4f} / {:.4f}), horizontal {:.2f} degrees",
              fov, aspect, near_plane, far_plane, LoadF(base, out), LoadF(base, out + 20),
              1.0 / std::tan(fov / 2) / aspect, 1.0 / std::tan(fov / 2),
              kuf::camera::HorizontalFov(fov, aspect) * 180.0 / M_PI);
}

// The game's camera collision (sub_82521710, the camera's virtual collision step) keeps the eye at least game
// parameter 248 above the ground found by a ray cast down from the eye (and casts up for ceilings with a
// fraction of it). Only that function reads parameter 248. That clearance suits the console camera, which never
// looks up; for the modern camera's low orbit the ground ray's clearance is lowered, so the eye can go below
// the character's upper body wherever the ground allows. The ceiling ray keeps the game's value (it keeps the
// camera off canopies and ceilings). (sub_826C6910: (parameters, id) -> f1.)
REX_EXTERN(__imp__sub_826C6910);
REX_HOOK_RAW(sub_826C6910) {
  const uint32_t id = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_826C6910(ctx, base);
  if (id == kGroundClearanceParameter) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("[kuf-cam] the game's camera ground clearance (parameter 248): {:.1f}", ctx.f1.f64);
    }
    const bool ground_ray = caller == kGroundRayLength || caller == kGroundRayClearance;
    if (ground_ray && g.pitched && kuf::pc::Current().camera.modern) {
      ctx.f1.f64 = std::min(ctx.f1.f64, kModernGroundClearance);
    }
  }
}

// The collision step ends by smoothing the camera's height through a global (0x82A805CC): a new height within 1000
// units of the smoothed one moves it only 30 % of the way per frame. With a free vertical orbit that lag pulls the
// eye off the orbit (and on fast moves the eye would cut a chord past the character). While the player moves the
// view vertically (and briefly after), the collision's unsmoothed height is used; otherwise (e.g. walking over
// uneven ground) the game's smoothing stays. The unsmoothed height is recovered exactly: either it passed
// through (a change over 1000) or the smoothed value moved 30 % of the way to it.
REX_EXTERN(__imp__sub_82521710);
REX_HOOK_RAW(sub_82521710) {  // camera collision: (camera, eye out, target out, eye in, target in) -> hit
  const uint32_t camera = ctx.r3.u32, eye_out = ctx.r4.u32;
  const bool modern = g.pitched && kuf::pc::Current().camera.modern;
  const bool bypass = modern && g.idle < kHeightFollowSeconds;
  const float before = LoadF(base, kSmoothedHeight);
  const Vec3 eye_in = LoadVec(base, ctx.r6.u32), target_in = LoadVec(base, ctx.r7.u32);
  const float baked_x = LoadF(base, camera + kBakedX), baked_z = LoadF(base, camera + kBakedZ);
  __imp__sub_82521710(ctx, base);
  const uint32_t hit = ctx.r3.u32;
  g.collision_hit = int(hit & 0xFF);
  // The collision pulls the game's own (baked) eye towards the eye it was given, or towards where the scenery
  // stopped it. With the orbit (pitched, or at another distance) the eye it is given is not the game's: the
  // game's eye was pulled towards ours, its distance drifted with the view angle (707 -> about 605 units held 12
  // degrees up) and a longer camera distance would feed on itself. So the pull is made in the game's terms:
  // the fraction of the way the collision let our eye go, applied to the game's own eye. Without scenery in the
  // way that is no pull, as for the game's own camera; with it, the game's camera is pulled in as far as ours.
  if (modern && base[camera + kBakeMode] != 0) {
    const Vec3 d{eye_in.x - target_in.x, eye_in.y - target_in.y, eye_in.z - target_in.z};
    const double length2 = d.x * d.x + d.y * d.y + d.z * d.z;
    const Vec3 offset{eye_in.x - g.game_eye.x, eye_in.y - g.game_eye.y, eye_in.z - g.game_eye.z};
    if (length2 > 1e-6 && std::hypot(offset.x, offset.y, offset.z) > 1e-3) {
      const Vec3 found = LoadVec(base, kCollisionEye);
      const double fraction = std::clamp(((found.x - target_in.x) * d.x + (found.y - target_in.y) * d.y +
                                          (found.z - target_in.z) * d.z) / length2, 0.0, 1.0);
      const double pull = LoadF(base, kCollisionPull);
      const double x = g.game_target.x + (g.game_eye.x - g.game_target.x) * fraction;
      const double z = g.game_target.z + (g.game_eye.z - g.game_target.z) * fraction;
      StoreF(base, camera + kBakedX, float(baked_x + pull * (x - baked_x)));
      StoreF(base, camera + kBakedZ, float(baked_z + pull * (z - baked_z)));
      g.obstruction = fraction;
    } else {
      g.obstruction = 1.0;
    }
  }
  // The height smoothing (see above): undone during vertical input.
  if (bypass) {
    const float after = LoadF(base, kSmoothedHeight);
    const double change = double(after) - before;
    // Over the threshold the height passed through unsmoothed; below 1e-6 nothing changed.
    if (std::abs(change) <= kHeightSmoothingThreshold && std::abs(change) >= 1e-6) {
      const float height = float(before + change / kHeightSmoothing);
      StoreF(base, kSmoothedHeight, height);
      StoreF(base, eye_out + 4, height);
    }
  }
  // Obstruction: the game's collision moves the eye out of the scenery in the same frame (in towards the
  // character, and up from the ground ray), and lets it go back as soon as the way is clear: with a far camera a
  // tree passing behind the character, or a rise in the ground behind it, made the camera jump in and out (the
  // view angle by 4 degrees in a frame). The camera still takes a larger correction at once (it never shows the
  // inside of the scenery: any correction the one in use does not cover is taken at once), but a smaller one only
  // gradually: the correction (eye out - eye in) eases towards the new one with a time constant of
  // kObstructionReleaseSeconds.
  if (!modern) {
    // The game's own camera takes the collision's correction at once both ways: a growing one is a pull-in.
    const Vec3 out = LoadVec(base, eye_out);
    const double size = std::hypot(out.x - eye_in.x, out.y - eye_in.y, out.z - eye_in.z);
    if (size > g.game_correction + kClearCorrection) g.pulled_pending = true;
    g.game_correction = size;
  }
  if (modern) {
    Vec3 out = LoadVec(base, eye_out);
    const Vec3 now{out.x - eye_in.x, out.y - eye_in.y, out.z - eye_in.z};
    const double size = std::hypot(now.x, now.y, now.z);
    // The correction is kept relative to the camera's arm (pivot -> eye): when the view turns, it turns with it.
    const Vec3 dir{eye_in.x - target_in.x, eye_in.y - target_in.y, eye_in.z - target_in.z};
    g.correction = kuf::camera::RotateWith(g.correction, g.last_dir, dir);
    g.last_dir = dir;
    if (!g.have_allowed) {
      g.correction = now;
    } else {
      // Ease towards the new correction, then add whatever of the new one the eased one does not cover (how far
      // it falls short in the new one's direction): what the scenery requires now is applied at once, what it
      // no longer requires is let go gradually. (Below kClearCorrection the way is clear.)
      const double k = 1.0 - std::exp(-g.dt / kObstructionReleaseSeconds);
      g.correction = {g.correction.x + (now.x - g.correction.x) * k, g.correction.y + (now.y - g.correction.y) * k,
                      g.correction.z + (now.z - g.correction.z) * k};
      if (size > kClearCorrection) {
        const double along = (g.correction.x * now.x + g.correction.y * now.y + g.correction.z * now.z) / size;
        const double missing = std::max(0.0, size - along) / size;
        g.correction = {g.correction.x + now.x * missing, g.correction.y + now.y * missing,
                        g.correction.z + now.z * missing};
        if (missing * size > kClearCorrection) g.pulled_pending = true;  // taken at once (presentation)
      }
    }
    g.have_allowed = true;
    out = {eye_in.x + g.correction.x, eye_in.y + g.correction.y, eye_in.z + g.correction.z};
    StoreVec(base, eye_out, out);
    const double requested = std::hypot(eye_in.x - target_in.x, eye_in.y - target_in.y, eye_in.z - target_in.z);
    const double allowed = requested > 1e-3 ? std::hypot(eye_in.x + now.x - target_in.x, eye_in.y + now.y -
                                              target_in.y, eye_in.z + now.z - target_in.z) / requested : 1.0;
    g.allowed = requested > 1e-3 ? std::hypot(out.x - target_in.x, out.y - target_in.y, out.z - target_in.z) /
                                   requested : 1.0;
    if (REXCVAR_GET(kuf_trace_camera_orbit)) {
      REXLOG_INFO("[kuf-cam] collision: hit {}, eye in ({:.1f},{:.1f},{:.1f}) out ({:.1f},{:.1f},{:.1f}), "
                  "allowed {:.3f}, used {:.3f}",
                  hit, eye_in.x, eye_in.y, eye_in.z, out.x, out.y, out.z, allowed, g.allowed);
    }
  }
}

namespace kuf::camera {
const TickView& LatestTickView() { return g.tick_view; }
}  // namespace kuf::camera
