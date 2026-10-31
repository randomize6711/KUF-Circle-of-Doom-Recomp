// Display math (display_math.h).

#include "runtime/display_math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace kuf::display {

double OutputAspect(uint32_t width, uint32_t height, bool widescreen) {
  if (!widescreen || width == 0 || height == 0) {
    return kGuestAspect;
  }
  return double(width) / double(height);
}

bool IsGuestAspect(double aspect) { return std::abs(aspect / kGuestAspect - 1.0) < 0.002; }

Projection WidenProjection(double fov, double output_aspect) {
  if (IsGuestAspect(output_aspect) || output_aspect <= 0) {
    return {fov, kGuestAspect};
  }
  if (output_aspect > kGuestAspect) {
    return {fov, output_aspect};  // Hor+: same vertical angle, the aspect widens the horizontal one
  }
  // Vert+: the horizontal angle of the 16:9 view, with the taller output's vertical angle.
  const double half_horizontal = std::tan(fov / 2) * kGuestAspect;
  return {2 * std::atan(half_horizontal / output_aspect), output_aspect};
}

UiScale UiBoxScale(double output_aspect) {
  if (IsGuestAspect(output_aspect) || output_aspect <= 0) {
    return {};
  }
  if (output_aspect > kGuestAspect) {
    return {kGuestAspect / output_aspect, 1};
  }
  return {1, output_aspect / kGuestAspect};
}

Rect UiBox(uint32_t output_width, uint32_t output_height) {
  const double w = output_width, h = output_height;
  if (w <= 0 || h <= 0) {
    return {};
  }
  const UiScale s = UiBoxScale(w / h);
  return {w * (1 - s.x) / 2, h * (1 - s.y) / 2, w * s.x, h * s.y};
}

bool WindowToUi(uint32_t output_width, uint32_t output_height, int32_t x, int32_t y, Point* out) {
  const Rect box = UiBox(output_width, output_height);
  if (box.width <= 0 || box.height <= 0) {
    return false;
  }
  // Pixel centres: pixel x covers [x, x + 1).
  const double u = (x + 0.5 - box.x) / box.width, v = (y + 0.5 - box.y) / box.height;
  if (u < 0 || v < 0 || u >= 1 || v >= 1) {
    return false;
  }
  out->x = int32_t(u * kGuestWidth);
  out->y = int32_t(v * kGuestHeight);
  return true;
}

RenderScale RenderScaleFor(uint32_t output_width, uint32_t output_height) {
  auto axis = [](uint32_t output, uint32_t guest) {
    return std::clamp<uint32_t>((output + guest - 1) / guest, 1, kMaxRenderScale);
  };
  return {axis(output_width, kGuestWidth), axis(output_height, kGuestHeight)};
}

RenderScale RenderScaleForSetting(int render_scale, uint32_t output_width, uint32_t output_height) {
  if (render_scale <= 0) {
    return RenderScaleFor(output_width, output_height);
  }
  const uint32_t n = std::min<uint32_t>(uint32_t(render_scale), kMaxRenderScale);
  return {n, n};
}

std::string RenderSettingName(int render_scale) {
  return render_scale <= 0 ? "Native" : std::to_string(render_scale) + "x";
}

bool StoreWindowSize(std::atomic<uint32_t>& width, std::atomic<uint32_t>& height, uint32_t w, uint32_t h) {
  const bool width_changed = width.exchange(w) != w;
  const bool height_changed = height.exchange(h) != h;
  return width_changed || height_changed;
}

std::string AspectName(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return "?";
  }
  struct Named {
    double ratio;
    const char* name;
  };
  // 21:9 is the marketing name of 64:27 (2.370) and 43:18 (2.389, 3440x1440); 32:9 likewise.
  static constexpr Named kNames[] = {{4.0 / 3, "4:3"},   {5.0 / 4, "5:4"},     {3.0 / 2, "3:2"},
                                     {16.0 / 10, "16:10"}, {16.0 / 9, "16:9"}, {64.0 / 27, "21:9"},
                                     {43.0 / 18, "21:9"},  {32.0 / 9, "32:9"}, {32.0 / 10, "32:10"}};
  const double ratio = double(width) / height;
  const Named* best = nullptr;
  for (const auto& n : kNames) {
    if (std::abs(ratio / n.ratio - 1) < 0.015 && (!best || std::abs(ratio - n.ratio) < std::abs(ratio - best->ratio))) {
      best = &n;
    }
  }
  if (best) {
    return best->name;
  }
  const uint32_t g = std::gcd(width, height);
  return std::to_string(width / g) + ":" + std::to_string(height / g);
}

namespace {

bool Larger(const Mode& a, const Mode& b) {
  if (a.width != b.width) return a.width > b.width;
  if (a.height != b.height) return a.height > b.height;
  return a.refresh_mhz > b.refresh_mhz;
}

}  // namespace

std::vector<Mode> FullscreenModes(const std::vector<Mode>& modes) {
  std::vector<Mode> out;
  for (const Mode& m : modes) {
    if (m.width && m.height && std::find(out.begin(), out.end(), m) == out.end()) {
      out.push_back(m);
    }
  }
  std::sort(out.begin(), out.end(), Larger);
  return out;
}

std::vector<Mode> WindowSizes(const std::vector<Mode>& modes, uint32_t usable_width, uint32_t usable_height) {
  std::vector<Mode> out;
  auto add = [&](uint32_t w, uint32_t h) {
    if (w && h && w <= usable_width && h <= usable_height &&
        std::none_of(out.begin(), out.end(), [&](const Mode& m) { return m.width == w && m.height == h; })) {
      out.push_back({w, h, 0});
    }
  };
  for (const Mode& m : modes) {
    add(m.width, m.height);
  }
  add(kGuestWidth, kGuestHeight);
  std::sort(out.begin(), out.end(), Larger);
  return out;
}

int FindMode(const std::vector<Mode>& choices, uint32_t width, uint32_t height, uint32_t refresh_mhz) {
  int best = -1;
  int64_t best_diff = 0;
  for (size_t i = 0; i < choices.size(); ++i) {
    const Mode& m = choices[i];
    if (m.width != width || m.height != height) {
      continue;
    }
    const int64_t diff = std::abs(int64_t(m.refresh_mhz) - int64_t(refresh_mhz));
    if (best < 0 || diff < best_diff) {
      best = int(i);
      best_diff = diff;
    }
  }
  return best;
}

std::string ModeName(const Mode& mode, bool with_refresh) {
  std::string name = std::to_string(mode.width) + " x " + std::to_string(mode.height);
  if (with_refresh && mode.refresh_mhz) {
    char hz[32];
    std::snprintf(hz, sizeof(hz), "%.2f", (mode.refresh_mhz + 5) / 10 / 100.0);
    std::string s = hz;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    name += " " + s + " Hz";
  }
  return name;
}

}  // namespace kuf::display
