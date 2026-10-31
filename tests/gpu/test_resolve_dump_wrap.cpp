// Resolve dumps of host render targets owned through EDRAM addressing wrapping (M6D D3b-0, KNOWN_ISSUES K34, SDK
// patch 0010). Before a resolve on the host render target path, the SDK dumps the render targets that own the
// resolved tiles into its EDRAM buffer: a compute dispatch per rectangle, whose shader reads, for each tile, the
// render target's texel at (dump base + dispatch offset + tile in the dispatch) - render target base, and writes the
// EDRAM buffer at that index & 2047. A render target owns the tiles from its base to its base plus 2048 tiles, so
// tiles below its base are owned through wrapping: the game's 4x MSAA depth pass at tile 720 claims the whole EDRAM,
// including the frame's colour at tile 0. Without the period correction their source index was negative, i.e. out of
// the render target, and the resolve read zeros: the black frame of K34.
//
// These cases emulate the dump shader's addressing for the rectangles RenderTargetCache builds
// (ResolveCopyDumpRectangle::AppendForOwnedRange and GetDispatches) and check every dumped tile: it is a tile of the
// owned range, written once, and read from inside the owning render target at the right place.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include <rex/graphics/pipeline/render_target/cache.h>

namespace {

constexpr uint32_t kTiles = rex::graphics::xenos::kEdramTileCount;  // 2048

// RenderTargetCache's dump rectangle is a protected nested type; nothing here instantiates the cache.
struct Access : rex::graphics::RenderTargetCache {
  using RenderTargetCache::ResolveCopyDumpRectangle;
};
using Rectangle = Access::ResolveCopyDumpRectangle;

struct Dump {
  uint32_t base, pitch, row_length, rows;
  // The tiles of the dump area: rows of row_length tiles, pitch tiles apart, wrapping at the EDRAM's end.
  std::set<uint32_t> Tiles() const {
    std::set<uint32_t> t;
    for (uint32_t r = 0; r < rows; ++r) {
      for (uint32_t x = 0; x < row_length; ++x) t.insert((base + r * pitch + x) % kTiles);
    }
    return t;
  }
};

struct Owner {
  uint32_t rt_base;      // the render target's base tile
  uint32_t start, end;   // the tiles it owns, [start, end) in EDRAM tile indices (end <= 2048; no wrap inside)
};

struct Checked {
  std::vector<uint32_t> tiles;   // EDRAM tiles written
  bool all_in_rt = true;         // every source index was inside the render target's period
  bool all_right_texel = true;   // and was the owned tile's place in it
  bool all_owned = true;         // every tile written belongs to the owner's range
  std::vector<uint32_t> periods;
};

// What RenderTargetCache::GetResolveCopyRectanglesToDump does for one owner: the dump area's extent in the period of
// its base, then the part after the dump area wraps (local offset 2048), each intersected with the owned range.
Checked DumpOwner(const Dump& d, const Owner& o) {
  std::vector<Rectangle> rects;
  uint32_t area_end = d.base + (d.rows - 1) * d.pitch + d.row_length;
  auto extent = [&](uint32_t extent_start, uint32_t extent_end, uint32_t local_offset) {
    uint32_t s = std::max(o.start, extent_start), e = std::min(o.end, extent_end);
    if (s < e) {
      Rectangle::AppendForOwnedRange(nullptr, o.rt_base, s, e, d.base, d.pitch, d.row_length, local_offset, rects);
    }
  };
  extent(d.base, std::min(area_end, kTiles), 0);
  if (area_end > kTiles) extent(0, std::min(area_end & (kTiles - 1), d.base), kTiles);

  Checked c;
  for (const Rectangle& r : rects) {
    c.periods.push_back(r.source_period_offset_tiles);
    Rectangle::Dispatch dispatches[Rectangle::kMaxDispatches];
    uint32_t n = r.GetDispatches(d.pitch, d.row_length, dispatches);
    for (uint32_t i = 0; i < n; ++i) {
      for (uint32_t y = 0; y < dispatches[i].height_tiles; ++y) {
        for (uint32_t x = 0; x < dispatches[i].width_tiles; ++x) {
          // The dump shader (VulkanRenderTargetCache::GetDumpPipeline; the D3D12 one is the same).
          uint32_t non_wrapped = d.base + dispatches[i].offset + y * d.pitch + x;
          uint32_t edram_tile = non_wrapped & (kTiles - 1);
          uint32_t source_index = non_wrapped - o.rt_base;
          c.tiles.push_back(edram_tile);
          c.all_in_rt &= source_index < kTiles;
          c.all_right_texel &= ((o.rt_base + source_index) & (kTiles - 1)) == edram_tile;
          c.all_owned &= edram_tile >= o.start && edram_tile < o.end;
        }
      }
    }
  }
  return c;
}

// The tiles a dump must write for an owner: the dump area's tiles in the owned range.
std::multiset<uint32_t> Expected(const Dump& d, const Owner& o) {
  std::multiset<uint32_t> t;
  for (uint32_t tile : d.Tiles()) {
    if (tile >= o.start && tile < o.end) t.insert(tile);
  }
  return t;
}

void CheckDump(const Dump& d, const Owner& o) {
  Checked c = DumpOwner(d, o);
  CHECK(c.all_owned);
  CHECK(c.all_in_rt);
  CHECK(c.all_right_texel);
  CHECK(std::multiset<uint32_t>(c.tiles.begin(), c.tiles.end()) == Expected(d, o));
}

}  // namespace

TEST_CASE("resolve dump: the K34 frame - colour at 0 owned by the 4x MSAA depth buffer at 720", "[gpu][k34]") {
  // The final resolve of the 1280x720 colour at tile 0 (pitch 16 tiles, 45 rows), after the game's depth-only
  // passes bound a 640-pitch 4x MSAA depth buffer at 720 whose estimated extent claimed the whole EDRAM.
  Dump d{0, 16, 16, 45};
  Owner o{720, 0, 720};
  Checked c = DumpOwner(d, o);
  REQUIRE(c.periods.size() == 1);
  CHECK(c.periods[0] == kTiles);  // read from the depth buffer's wrapped tail, one period on
  CHECK(c.all_in_rt);
  CHECK(c.all_right_texel);
  CHECK(c.tiles.size() == 720);
}

TEST_CASE("resolve dump: an owned range across the owner's base is split there", "[gpu][k34]") {
  // The same depth buffer owns everything; a resolve of tiles 700..763 covers its wrapped tail and its start.
  Dump d{700, 16, 16, 4};
  Owner o{720, 0, kTiles};
  Checked c = DumpOwner(d, o);
  CHECK(c.periods.size() == 2);
  CheckDump(d, o);
}

TEST_CASE("resolve dump: a wrapping dump area of a render target that does not wrap", "[gpu][k34]") {
  // The dump area from 1800 wraps to the EDRAM's start; the tiles there belong to a render target at 100 that does
  // not wrap (before the fix they were read one period too far, outside it).
  Dump d{1800, 16, 16, 30};
  Owner o{100, 100, 300};
  Checked c = DumpOwner(d, o);
  REQUIRE(!c.periods.empty());
  CHECK(c.periods[0] == uint32_t(0) - kTiles);  // back by one period
  CheckDump(d, o);
}

TEST_CASE("resolve dump: ordinary ranges keep their dispatch offsets", "[gpu][k34]") {
  // The frame's colour owned by itself, and a wrapping dump area owned by a wrapping render target: no correction,
  // so normal frames dump exactly as before.
  Checked a = DumpOwner(Dump{0, 16, 16, 45}, Owner{0, 0, 720});
  CHECK(a.periods == std::vector<uint32_t>{0});
  CheckDump(Dump{0, 16, 16, 45}, Owner{0, 0, 720});
  Dump d{1800, 16, 16, 30};
  Checked b = DumpOwner(d, Owner{1800, 0, 300});
  CHECK(b.periods == std::vector<uint32_t>{0});
  CheckDump(d, Owner{1800, 0, 300});
  CheckDump(d, Owner{1800, 1800, kTiles});
}

TEST_CASE("resolve dump: every geometry and owner, randomised", "[gpu][k34]") {
  std::mt19937 rng(34);
  auto uniform = [&](uint32_t lo, uint32_t hi) { return std::uniform_int_distribution<uint32_t>(lo, hi)(rng); };
  for (int i = 0; i < 20000; ++i) {
    Dump d;
    d.pitch = uniform(1, 64);
    d.row_length = uniform(1, d.pitch);
    d.rows = uniform(1, std::max<uint32_t>(1, (kTiles - d.row_length) / d.pitch + 1));
    d.base = uniform(0, kTiles - 1);
    Owner o;
    o.rt_base = uniform(0, kTiles - 1);
    o.start = uniform(0, kTiles - 1);
    o.end = uniform(o.start + 1, kTiles);
    Checked c = DumpOwner(d, o);
    INFO("dump base " << d.base << " pitch " << d.pitch << " row " << d.row_length << " rows " << d.rows
                      << "; owner base " << o.rt_base << " [" << o.start << ", " << o.end << ")");
    REQUIRE(c.all_owned);
    REQUIRE(c.all_in_rt);
    REQUIRE(c.all_right_texel);
    REQUIRE(std::multiset<uint32_t>(c.tiles.begin(), c.tiles.end()) == Expected(d, o));
  }
}
