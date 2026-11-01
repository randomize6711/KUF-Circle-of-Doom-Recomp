#include "runtime/ui_layout.h"

#include <algorithm>
#include <climits>
#include <tuple>
#include <array>
#include <cstdio>
#include <map>

namespace kuf::ui {

namespace {

constexpr size_t kFileHeader = 44, kSceneHeader = 100, kPlacement = 12;
constexpr char kMagic[] = "BlueSide UI Export File";

// A row cloned to make the PC Settings row: which scene, which controls (old ID -> new ID: the row's frame and
// label; the label button is the one whose new ID is `button`), which panel images grow by a row and which
// controls move down by a row.
struct RowSpec {
  uint32_t scene;
  const char* scene_name;
  const char* row_name;
  std::vector<std::pair<uint32_t, uint32_t>> clones;
  uint32_t button;
  std::vector<uint32_t> grow, move_down;
};

// In-game Settings: the Vibration row's frame and label (not its On/Off halves: the new row is an action row);
// the panel's sides grow, its bottom and the button help move down.
const RowSpec kInGameRow = {
    0x2700000, "SN_PROFILE_OPTION_CONFIG", "Vibration",
    {{0x2705045, 0x2705080},  // IM_..._VIB_L / _C / _R: the row's frame
     {0x2705046, 0x2705081},
     {0x2705047, 0x2705082},
     {0x2705048, 0x2705083},  // IM_..._VIB_OFF_L / _C / _R: the label button's images
     {0x2705049, 0x2705084},
     {0x270504A, 0x2705085},
     {0x270100D, kPcRowButton}},  // BT_..._VIB: the label
    kPcRowButton,
    {0x2705004, 0x2705005, 0x2705006},              // IM_..._LC / CC / RC
    {0x2705007, 0x2705008, 0x2705009, 0x2704001}};  // IM_..._LD / CD / RD, TT_..._BTNINFO

// Title SETTINGS: the Change Storage Device row (a full-width action row); the panel already reaches below it.
const RowSpec kTitleRow = {
    0x4100000, "SN_TITLE_OPTION", "Change Storage Device",
    {{0x4105043, 0x4105080},  // IM_TITLE_OPTION_CONFIG_SAVE_L / _C / _R
     {0x4105044, 0x4105081},
     {0x4105045, 0x4105082},
     {0x4105046, 0x4105083},  // IM_TITLE_OPTION_CONFIG_SAVE_OFF_L / _C / _R
     {0x4105047, 0x4105084},
     {0x4105048, 0x4105085},
     {0x410100D, kTitlePcRowButton}},  // BT_TITLE_OPTION_CONFIG_SAVE
    kTitlePcRowButton,
    {},
    {}};

size_t ControlSize(std::span<const uint8_t> data, size_t offset, uint32_t type) {
  switch (type) {
    case 1: return 136;
    case 2: return offset + 176 <= data.size() && Get32(data, offset + 172) == 1 ? 308 : 180;
    case 4: return 92;
    case 5: return 380;
    default: return 0;
  }
}

}  // namespace

uint32_t Get32(std::span<const uint8_t> bytes, size_t offset) {
  return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 | uint32_t(bytes[offset + 2]) << 8 |
         uint32_t(bytes[offset + 3]);
}

void Put32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
  bytes[offset] = uint8_t(value >> 24);
  bytes[offset + 1] = uint8_t(value >> 16);
  bytes[offset + 2] = uint8_t(value >> 8);
  bytes[offset + 3] = uint8_t(value);
}

uint32_t Scene::id() const { return Get32(header, 4); }

std::optional<Layout> Parse(std::span<const uint8_t> data, std::string* error) {
  auto fail = [&](std::string why) -> std::optional<Layout> {
    *error = std::move(why);
    return std::nullopt;
  };
  if (data.size() < kFileHeader || !std::equal(kMagic, kMagic + sizeof(kMagic) - 1, data.begin())) {
    return fail("not a BlueSide UI layout");
  }
  Layout layout;
  layout.header.assign(data.begin(), data.begin() + kFileHeader);
  const uint32_t scene_count = Get32(data, 40);
  size_t o = kFileHeader;
  for (uint32_t s = 0; s < scene_count; ++s) {
    if (o + kSceneHeader > data.size() || Get32(data, o) != 0) {
      return fail("scene " + std::to_string(s) + ": no scene record at " + std::to_string(o));
    }
    Scene scene;
    scene.header.assign(data.begin() + o, data.begin() + o + kSceneHeader);
    const uint32_t children = Get32(data, o + 88);
    o += kSceneHeader;
    if (o + size_t(children) * kPlacement > data.size()) {
      return fail("scene " + std::to_string(s) + ": placements past the end");
    }
    for (uint32_t c = 0; c < children; ++c, o += kPlacement) {
      scene.placements.push_back({Get32(data, o), int32_t(Get32(data, o + 4)), int32_t(Get32(data, o + 8))});
    }
    for (uint32_t c = 0; c < children; ++c) {
      if (o + 8 > data.size()) {
        return fail("scene " + std::to_string(s) + ": control records past the end");
      }
      const uint32_t type = Get32(data, o), id = Get32(data, o + 4);
      const size_t size = ControlSize(data, o, type);
      if (size == 0 || o + size > data.size() || id != scene.placements[c].id) {
        return fail("scene " + std::to_string(s) + ": unexpected control record at " + std::to_string(o));
      }
      scene.controls.push_back({type, id, std::vector<uint8_t>(data.begin() + o, data.begin() + o + size)});
      o += size;
    }
    layout.scenes.push_back(std::move(scene));
  }
  if (o != data.size()) {
    return fail("trailing bytes after the last scene");
  }
  return layout;
}

std::vector<uint8_t> Serialize(const Layout& layout) {
  std::vector<uint8_t> out(layout.header);
  Put32(out, 40, uint32_t(layout.scenes.size()));
  for (const Scene& scene : layout.scenes) {
    const size_t at = out.size();
    out.insert(out.end(), scene.header.begin(), scene.header.end());
    Put32(std::span(out).subspan(at), 88, uint32_t(scene.placements.size()));
    for (const Placement& p : scene.placements) {
      const size_t q = out.size();
      out.resize(q + kPlacement);
      Put32(out, q, p.id);
      Put32(out, q + 4, uint32_t(p.x));
      Put32(out, q + 8, uint32_t(p.y));
    }
    for (const ControlRecord& c : scene.controls) {
      out.insert(out.end(), c.bytes.begin(), c.bytes.end());
    }
  }
  return out;
}

namespace {

std::optional<std::vector<uint8_t>> AddActionRow(std::span<const uint8_t> data, const RowSpec& spec,
                                                 uint32_t label_text, std::string* error) {
  std::optional<Layout> layout = Parse(data, error);
  if (!layout) {
    return std::nullopt;
  }
  auto scene = std::find_if(layout->scenes.begin(), layout->scenes.end(),
                            [&](const Scene& s) { return s.id() == spec.scene; });
  if (scene == layout->scenes.end()) {
    *error = std::string("no ") + spec.scene_name + " scene";
    return std::nullopt;
  }
  std::map<uint32_t, uint32_t> new_id(spec.clones.begin(), spec.clones.end());
  for (const auto& [old_id, id] : spec.clones) {
    const bool has_old = std::any_of(scene->controls.begin(), scene->controls.end(),
                                     [&](const ControlRecord& c) { return c.id == old_id; });
    const bool has_new = std::any_of(scene->controls.begin(), scene->controls.end(),
                                     [&](const ControlRecord& c) { return c.id == id; });
    if (!has_old || has_new) {
      *error = std::string("the ") + spec.row_name + " row is not as expected (or the new IDs are taken)";
      return std::nullopt;
    }
  }
  // Each clone goes right after its original (same drawing order relative to the highlight), one row lower.
  for (size_t i = 0; i < scene->controls.size(); ++i) {
    auto it = new_id.find(scene->controls[i].id);
    if (it == new_id.end()) {
      continue;
    }
    ControlRecord clone = scene->controls[i];
    clone.id = it->second;
    Put32(clone.bytes, 4, clone.id);
    if (clone.type == 1) {
      for (size_t at = 100; at <= 132; at += 4) {  // the button's state images
        auto image = new_id.find(Get32(clone.bytes, at));
        if (image != new_id.end()) {
          Put32(clone.bytes, at, image->second);
        }
      }
      if (clone.id == spec.button && label_text != 0) {
        Put32(clone.bytes, 20, label_text);
      }
    } else if (clone.type == 5) {
      auto owner = new_id.find(Get32(clone.bytes, 376));  // the button an image belongs to
      if (owner != new_id.end()) {
        Put32(clone.bytes, 376, owner->second);
      }
    }
    Placement placement = scene->placements[i];
    placement.id = clone.id;
    placement.y += kRowSpacing;
    scene->controls.insert(scene->controls.begin() + i + 1, std::move(clone));
    scene->placements.insert(scene->placements.begin() + i + 1, placement);
    ++i;
  }
  for (size_t i = 0; i < scene->controls.size(); ++i) {
    const uint32_t id = scene->controls[i].id;
    if (std::find(spec.grow.begin(), spec.grow.end(), id) != spec.grow.end()) {
      Put32(scene->controls[i].bytes, 12, Get32(scene->controls[i].bytes, 12) + kRowSpacing);  // height
    }
    if (std::find(spec.move_down.begin(), spec.move_down.end(), id) != spec.move_down.end()) {
      scene->placements[i].y += kRowSpacing;
    }
  }
  return Serialize(*layout);
}

}  // namespace

std::optional<std::vector<uint8_t>> AddSettingsActionRow(std::span<const uint8_t> data, uint32_t label_text,
                                                         std::string* error) {
  return AddActionRow(data, kInGameRow, label_text, error);
}

std::optional<std::vector<uint8_t>> AddTitleSettingsActionRow(std::span<const uint8_t> data, uint32_t label_text,
                                                              std::string* error) {
  return AddActionRow(data, kTitleRow, label_text, error);
}

std::optional<std::vector<uint8_t>> AddTitleMainRows(std::span<const uint8_t> data,
                                                     const std::vector<uint32_t>& labels, std::string* error) {
  constexpr uint32_t kScene = 0x3900000, kCredits = 0x3901005, kCreditsImage = 0x3905011;
  std::optional<Layout> layout = Parse(data, error);
  if (!layout) {
    return std::nullopt;
  }
  auto scene = std::find_if(layout->scenes.begin(), layout->scenes.end(),
                            [&](const Scene& s) { return s.id() == kScene; });
  if (scene == layout->scenes.end()) {
    *error = "no title list scene";
    return std::nullopt;
  }
  auto index_of = [&](uint32_t id) -> long {
    for (size_t i = 0; i < scene->controls.size(); ++i) {
      if (scene->controls[i].id == id) return long(i);
    }
    return -1;
  };
  const long button = index_of(kCredits), image = index_of(kCreditsImage);
  if (button < 0 || image < 0 || scene->controls[size_t(button)].type != 1 ||
      scene->controls[size_t(image)].type != 5 || Get32(scene->controls[size_t(image)].bytes, 376) != kCredits) {
    *error = "the CREDITS row is not as expected";
    return std::nullopt;
  }
  for (size_t k = 0; k < labels.size(); ++k) {
    if (index_of(kTitleMainRowButton + uint32_t(k)) >= 0 || index_of(kTitleMainRowImage + 3 * uint32_t(k)) >= 0) {
      *error = "the new title list IDs are taken";
      return std::nullopt;
    }
  }
  const int32_t shift = kTitleRowSpacing * int32_t(labels.size());
  for (Placement& p : scene->placements) {
    p.y -= shift;
  }
  // New images after the CREDITS frame, new buttons after the CREDITS button (both before the highlight's).
  const ControlRecord button_record = scene->controls[size_t(button)];
  const ControlRecord image_record = scene->controls[size_t(image)];
  const Placement button_at = scene->placements[size_t(button)], image_at = scene->placements[size_t(image)];
  std::vector<ControlRecord> buttons, images;
  std::vector<Placement> button_places, image_places;
  for (size_t k = 0; k < labels.size(); ++k) {
    const uint32_t id = kTitleMainRowButton + uint32_t(k), image_id = kTitleMainRowImage + 3 * uint32_t(k);
    const int32_t dy = kTitleRowSpacing * int32_t(k + 1);
    ControlRecord b = button_record;
    b.id = id;
    Put32(b.bytes, 4, id);
    Put32(b.bytes, 20, labels[k]);
    for (size_t at = 100; at <= 132; at += 4) {  // the button's state images
      if (Get32(b.bytes, at) == kCreditsImage) Put32(b.bytes, at, image_id);
    }
    ControlRecord im = image_record;
    im.id = image_id;
    Put32(im.bytes, 4, image_id);
    Put32(im.bytes, 376, id);
    buttons.push_back(std::move(b));
    images.push_back(std::move(im));
    button_places.push_back({id, button_at.x, button_at.y + dy});
    image_places.push_back({image_id, image_at.x, image_at.y + dy});
  }
  // Insert the later position first so the earlier index stays valid.
  auto insert = [&](long after, std::vector<ControlRecord>& recs, std::vector<Placement>& places) {
    scene->controls.insert(scene->controls.begin() + after + 1, recs.begin(), recs.end());
    scene->placements.insert(scene->placements.begin() + after + 1, places.begin(), places.end());
  };
  if (button > image) {
    insert(button, buttons, button_places);
    insert(image, images, image_places);
  } else {
    insert(image, images, image_places);
    insert(button, buttons, button_places);
  }
  return Serialize(*layout);
}

std::optional<std::vector<uint8_t>> AddTextClone(std::span<const uint8_t> data, uint32_t scene_id, uint32_t source,
                                                 uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                                                 uint32_t font, std::string* error) {
  std::optional<Layout> layout = Parse(data, error);
  if (!layout) {
    return std::nullopt;
  }
  auto scene = std::find_if(layout->scenes.begin(), layout->scenes.end(),
                            [&](const Scene& s) { return s.id() == scene_id; });
  if (scene == layout->scenes.end()) {
    *error = "no such scene";
    return std::nullopt;
  }
  const ControlRecord* text = nullptr;
  for (const ControlRecord& c : scene->controls) {
    if (c.id == id) {
      *error = "the new text ID is taken";
      return std::nullopt;
    }
    if (c.id == source) text = &c;
  }
  if (!text || text->type != 4 || text->bytes.size() != 92) {
    *error = "the source text control is not as expected";
    return std::nullopt;
  }
  ControlRecord copy = *text;
  copy.id = id;
  Put32(copy.bytes, 4, id);
  Put32(copy.bytes, 8, uint32_t(w));
  Put32(copy.bytes, 12, uint32_t(h));
  Put32(copy.bytes, 20, 0xFFFFFFFFu);  // set by code
  if (font) Put32(copy.bytes, 40, font);
  scene->controls.push_back(std::move(copy));
  scene->placements.push_back({id, x, y});
  return Serialize(*layout);
}

std::optional<std::vector<uint8_t>> AddImageClones(std::span<const uint8_t> data, uint32_t scene_id,
                                                   const std::vector<ImageClone>& clones, uint32_t before,
                                                   std::string* error) {
  std::optional<Layout> layout = Parse(data, error);
  if (!layout) {
    return std::nullopt;
  }
  auto scene = std::find_if(layout->scenes.begin(), layout->scenes.end(),
                            [&](const Scene& s) { return s.id() == scene_id; });
  if (scene == layout->scenes.end()) {
    *error = "no such scene";
    return std::nullopt;
  }
  size_t at = scene->controls.size();
  if (before) {
    auto b = std::find_if(scene->controls.begin(), scene->controls.end(),
                          [&](const ControlRecord& c) { return c.id == before; });
    if (b == scene->controls.end()) {
      *error = "no control to insert before";
      return std::nullopt;
    }
    at = size_t(b - scene->controls.begin());
  }
  std::vector<ControlRecord> records;
  std::vector<Placement> places;
  for (const ImageClone& clone : clones) {
    size_t source = scene->controls.size();
    for (size_t i = 0; i < scene->controls.size(); ++i) {
      if (scene->controls[i].id == clone.id) {
        *error = "a new image ID is taken";
        return std::nullopt;
      }
      if (scene->controls[i].id == clone.source) source = i;
    }
    if (source == scene->controls.size() || scene->controls[source].type != 5 ||
        scene->controls[source].bytes.size() != 380) {
      *error = "a source image control is not as expected";
      return std::nullopt;
    }
    ControlRecord copy = scene->controls[source];
    Placement place = scene->placements[source];
    copy.id = place.id = clone.id;
    Put32(copy.bytes, 4, clone.id);
    if (clone.place) {
      place.x = clone.x;
      place.y = clone.y;
      Put32(copy.bytes, 8, uint32_t(clone.w));
      Put32(copy.bytes, 12, uint32_t(clone.h));
    }
    if (clone.alpha >= 0) Put32(copy.bytes, 84, uint32_t(std::min(clone.alpha, 255)));
    records.push_back(std::move(copy));
    places.push_back(place);
  }
  scene->controls.insert(scene->controls.begin() + ptrdiff_t(at), records.begin(), records.end());
  scene->placements.insert(scene->placements.begin() + ptrdiff_t(at), places.begin(), places.end());
  return Serialize(*layout);
}

std::optional<std::vector<uint8_t>> MoveControls(std::span<const uint8_t> data, uint32_t scene_id,
                                                 const std::vector<uint32_t>& ids, int32_t dx, int32_t dy,
                                                 std::string* error) {
  std::optional<Layout> layout = Parse(data, error);
  if (!layout) {
    return std::nullopt;
  }
  auto scene = std::find_if(layout->scenes.begin(), layout->scenes.end(),
                            [&](const Scene& s) { return s.id() == scene_id; });
  if (scene == layout->scenes.end()) {
    *error = "no such scene";
    return std::nullopt;
  }
  for (const uint32_t id : ids) {
    auto p = std::find_if(scene->placements.begin(), scene->placements.end(),
                          [&](const Placement& q) { return q.id == id; });
    if (p == scene->placements.end()) {
      char hex[16];
      std::snprintf(hex, sizeof(hex), "%08X", id);
      *error = std::string("no control ") + hex;
      return std::nullopt;
    }
    p->x += dx;
    p->y += dy;
  }
  return Serialize(*layout);
}

std::optional<std::vector<uint8_t>> AddLobbyColumns(std::span<const uint8_t> data, std::string* error) {
  std::optional<Layout> layout = Parse(data, error);
  if (!layout) return std::nullopt;
  auto scene = std::find_if(layout->scenes.begin(), layout->scenes.end(),
                            [&](const Scene& s) { return s.id() == kLobbyScene; });
  if (scene == layout->scenes.end()) {
    *error = "no game type screen";
    return std::nullopt;
  }
  // The rows' line (y) and their common left edge (the leftmost row: row 0, Difficulty, is set in).
  int32_t ys[6] = {};
  int32_t left = INT32_MAX;
  for (size_t r = 0; r < 6; ++r) {
    auto p = std::find_if(scene->placements.begin(), scene->placements.end(),
                          [&](const Placement& pl) { return pl.id == kLobbyRowButtons[r]; });
    if (p == scene->placements.end()) {
      *error = "a row button is missing";
      return std::nullopt;
    }
    ys[r] = p->y;
    left = std::min(left, p->x);
  }
  std::vector<uint8_t> out = Serialize(*layout);
  for (uint32_t r = 0; r < 6; ++r) {
    for (const auto& [id, x, w] : {std::tuple{kLobbyLabelText + r, left + kLobbyLabelInset, kLobbyLabelWidth},
                                    std::tuple{kLobbyValueText + r, left + kLobbyValueOffset, kLobbyValueWidth}}) {
      auto next = AddTextClone(out, kLobbyScene, kLobbyTextSource, id, x, ys[r], w, kLobbyRowHeight, 0, error);
      if (!next) return std::nullopt;
      out = std::move(*next);
    }
  }
  return out;
}

}  // namespace kuf::ui
