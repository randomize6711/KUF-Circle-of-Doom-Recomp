// Display (M6C; display.h). The rendering side:
//
// 3D views. The view object's projection (sub_821E9ED0 on the render side, sub_821E9FB0 on the game side) takes
// its aspect from view+780 if the game set one, else from its viewport (+728 / +732, 1280x720 for the main
// views), stores it at +744 and builds the matrix with sub_826FC108 (f1 vertical angle, f2 aspect); the culling
// takes its scales from that matrix (+748, +752), and a third caller (0x82216C8C) builds a matrix from +744.
// For a 16:9 projection of these callers the angle and aspect are replaced by the output's (display_math.h
// WidenProjection) and +744 holds the new aspect, so drawing, culling and that third matrix agree.
//
// HUD and menus. The game's 2D renderer (sub_8222C360, called once per 2D layer) turns each queued quad (screen
// rectangles in 1280x720 pixels: menu controls, text, HUD parts, full-screen fades) into four vertices in clip space
// (x = (px / width - 0.5) * 2, y likewise; the vertex shader passes them through). A mid-asm hook where its two
// conversion paths meet (0x8222C9B4, config/v1/codegen.toml: the quad's vertices at r25 - 32, 24 bytes apart, x and
// y the first two floats) scales them about the centre into the 16:9 box (UiBoxScale). A quad that spans the whole
// width (or height) is a full-screen layer (fades, dimming, overlays) and is left to fill the output.
//
// Movies. The title loop and the attract movie are drawn by the game's movie player over the whole frame, not by the
// 2D renderer; while one plays the output is treated as 16:9 (the presenter pillarboxes the frame, 2D quads stay as
// they are), so the movie keeps its shape.
//
// The frame stays 1280x720 (rendered at RenderScaleFor's multiple of it) and the presenter stretches it to the
// output (present_letterbox off), which turns the 3D views back to their true shape and the box back to 16:9.

#include "runtime/display.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "runtime/display_math.h"
#include "runtime/pc_settings.h"

REXCVAR_DEFINE_BOOL(kuf_trace_display, false, "KUF/Debug",
                    "Log the projections the display code changes or leaves (each distinct one once) (tests)");

namespace {

constexpr uint32_t kRenderViewCaller = 0x821E9F74;   // sub_821E9ED0 (r31 = view)
constexpr uint32_t kGameViewCaller = 0x821EA044;     // sub_821E9FB0 (r31 = view)
constexpr uint32_t kViewAspectCaller = 0x82216C8C;   // builds a matrix from a view's +744 (r5 = view)
constexpr uint32_t kViewAspect = 744;

uint8_t* g_base = nullptr;  // guest memory (set by the 2D renderer's hook before its quads)
std::atomic<bool> g_movie{false};  // a movie is on screen (the title loop or the attract movie)
std::atomic<uint64_t> g_ui_quads{0}, g_ui_full{0};

std::mutex g_trace_mutex;
std::set<std::tuple<uint32_t, int, int, int>> g_traced;  // (caller, fov, aspect, changed) in 1e-4

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

float LoadF(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

void StoreF(uint8_t* base, uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  bits = __builtin_bswap32(bits);
  std::memcpy(base + address, &bits, sizeof(bits));
}

void TraceProjection(uint32_t caller, double fov, double aspect, bool changed, double new_fov, double new_aspect) {
  if (!REXCVAR_GET(kuf_trace_display)) {
    return;
  }
  std::lock_guard lock(g_trace_mutex);
  if (!g_traced.insert({caller, int(std::lround(fov * 1e4)), int(std::lround(aspect * 1e4)), changed}).second) {
    return;
  }
  if (changed) {
    REXLOG_INFO("[kuf-display] projection from {:08X}: fov {:.4f} aspect {:.4f} -> fov {:.4f} aspect {:.4f}", caller,
                fov, aspect, new_fov, new_aspect);
  } else {
    REXLOG_INFO("[kuf-display] projection from {:08X}: fov {:.4f} aspect {:.4f} (kept)", caller, fov, aspect);
  }
}

}  // namespace

namespace kuf::display {

void BeforePerspective(PPCContext& ctx, uint8_t* base) {
  const uint32_t caller = uint32_t(ctx.lr);
  if (caller != kRenderViewCaller && caller != kGameViewCaller && caller != kViewAspectCaller) {
    return;
  }
  const double fov = ctx.f1.f64, aspect = ctx.f2.f64;
  const Output out = CurrentOutput();
  if (IsGuestAspect(out.aspect)) {
    TraceProjection(caller, fov, aspect, false, fov, aspect);
    return;
  }
  // The third caller reads the aspect stored by the view callers: already the output's.
  const bool guest = IsGuestAspect(aspect);
  if (!guest && !(caller == kViewAspectCaller && std::abs(aspect - out.aspect) < 1e-4)) {
    TraceProjection(caller, fov, aspect, false, fov, aspect);
    return;
  }
  const Projection p = WidenProjection(fov, out.aspect);
  ctx.f1.f64 = p.fov;
  ctx.f2.f64 = p.aspect;
  if (caller != kViewAspectCaller) {
    StoreF(base, ctx.r31.u32 + kViewAspect, float(p.aspect));
  }
  TraceProjection(caller, fov, aspect, true, p.fov, p.aspect);
}

bool MovieOnScreen() { return g_movie.load(); }

void LogUiStats() {
  if (g_ui_quads.load()) {
    REXLOG_INFO("[kuf-display] 2D quads placed in the 16:9 box: {} ({} full-screen layers left full)",
                g_ui_quads.load() - g_ui_full.load(), g_ui_full.load());
  }
}

}  // namespace kuf::display

// The game's movie player (the console SDK's WMV player behind a small command queue; only the title loop and the
// attract movie use it): play (player, file name, loop) and stop (player), both from the title scene. The player
// draws the movie itself over the whole frame, so while one plays the output is the original 16:9 picture.
REX_EXTERN(__imp__sub_826E90C0);
REX_HOOK_RAW(sub_826E90C0) {
  const uint32_t name = ctx.r4.u32;
  const bool was = g_movie.exchange(true);
  if (!was) {
    REXLOG_INFO("[kuf-display] movie {}: shown at 16:9", name ? reinterpret_cast<const char*>(base + name) : "?");
    kuf::display::Refresh();
  }
  __imp__sub_826E90C0(ctx, base);
}

REX_EXTERN(__imp__sub_826E91F0);
REX_HOOK_RAW(sub_826E91F0) {
  if (g_movie.exchange(false)) {
    REXLOG_INFO("[kuf-display] movie stopped");
    kuf::display::Refresh();
  }
  __imp__sub_826E91F0(ctx, base);
}

// The 2D renderer's per-layer flush: (renderer, quad list, ...). Only keeps guest memory's base for the mid-asm hook.
REX_EXTERN(__imp__sub_8222C360);
REX_HOOK_RAW(sub_8222C360) {
  g_base = base;
  __imp__sub_8222C360(ctx, base);
}

// Mid-asm hook (0x8222C9B4): one 2D quad's four vertices are written at r25 - 32 (stride 24: x, y, z, ...).
void KufUiQuadHook(PPCRegister& r25, PPCRegister& /*r30*/) {
  uint8_t* base = g_base;
  if (!base) {
    return;
  }
  const auto scale = kuf::display::UiBoxScale(kuf::display::CurrentOutput().aspect);
  if (scale.x == 1 && scale.y == 1) {
    return;
  }
  constexpr uint32_t kStride = 24;
  constexpr float kEdge = 0.999f;
  const uint32_t v = r25.u32 - 32;
  float x[4], y[4];
  for (uint32_t i = 0; i < 4; ++i) {
    x[i] = LoadF(base, v + kStride * i);
    y[i] = LoadF(base, v + kStride * i + 4);
  }
  const auto [x_min, x_max] = std::minmax({x[0], x[1], x[2], x[3]});
  const auto [y_min, y_max] = std::minmax({y[0], y[1], y[2], y[3]});
  ++g_ui_quads;
  if (scale.x != 1 && x_min <= -kEdge && x_max >= kEdge) {
    ++g_ui_full;
    return;  // a full-screen layer: fills the output
  }
  if (scale.y != 1 && y_min <= -kEdge && y_max >= kEdge) {
    ++g_ui_full;
    return;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (scale.x != 1) StoreF(base, v + kStride * i, float(x[i] * scale.x));
    if (scale.y != 1) StoreF(base, v + kStride * i + 4, float(y[i] * scale.y));
  }
}
