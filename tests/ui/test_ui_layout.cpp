// Menu layout reading and the PC Settings rows (src/runtime/ui_layout.cpp), on synthetic layouts built here (no
// game data).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "runtime/ui_layout.h"

using namespace kuf::ui;

namespace {

struct Control {
  uint32_t type, id;
  int32_t x, y;
};

std::vector<uint8_t> Record(const Control& c) {
  size_t size = c.type == 1 ? 136 : c.type == 2 ? 180 : c.type == 4 ? 92 : 380;
  std::vector<uint8_t> r(size, 0xEE);  // filler like the exporter's leftovers
  Put32(r, 0, c.type);
  Put32(r, 4, c.id);
  Put32(r, 8, 100);  // width
  Put32(r, 12, 30);  // height
  if (c.type == 2) {
    Put32(r, 172, 0);
  }
  return r;
}

std::vector<uint8_t> Build(const std::vector<std::pair<uint32_t, std::vector<Control>>>& scenes,
                           std::vector<std::vector<uint8_t>>* records_out = nullptr) {
  std::vector<uint8_t> out(44, 0);
  const std::string magic = "BlueSide UI Export File";
  std::copy(magic.begin(), magic.end(), out.begin());
  Put32(out, 40, uint32_t(scenes.size()));
  for (const auto& [scene_id, controls] : scenes) {
    std::vector<uint8_t> header(100, 0);
    Put32(header, 4, scene_id);
    Put32(header, 88, uint32_t(controls.size()));
    out.insert(out.end(), header.begin(), header.end());
    for (const Control& c : controls) {
      const size_t at = out.size();
      out.resize(at + 12);
      Put32(out, at, c.id);
      Put32(out, at + 4, uint32_t(c.x));
      Put32(out, at + 8, uint32_t(c.y));
    }
    for (const Control& c : controls) {
      std::vector<uint8_t> r = Record(c);
      if (c.id == 0x270100D) {  // the Vibration label: its three state images
        Put32(r, 104, 0x2705048);
        Put32(r, 116, 0x2705049);
        Put32(r, 128, 0x270504A);
        Put32(r, 20, 1882);
      }
      if (c.id >= 0x2705048 && c.id <= 0x270504A) {
        Put32(r, 376, 0x270100D);
      }
      if (c.id == 0x410100D) {  // the title's Change Storage Device label: its three state images
        Put32(r, 104, 0x4105046);
        Put32(r, 116, 0x4105047);
        Put32(r, 128, 0x4105048);
        Put32(r, 20, 1645);
      }
      if (c.id >= 0x4105046 && c.id <= 0x4105048) {
        Put32(r, 376, 0x410100D);
      }
      if (records_out) records_out->push_back(r);
      out.insert(out.end(), r.begin(), r.end());
    }
  }
  return out;
}

const std::vector<Control> kConfig = {
    {5, 0x2705004, 428, 131}, {5, 0x2705005, 444, 131}, {5, 0x2705006, 1184, 131},  // panel sides
    {5, 0x2705007, 428, 557}, {5, 0x2705008, 444, 557}, {5, 0x2705009, 1184, 557},  // panel bottom
    {4, 0x2704001, 446, 517},                                                       // button help
    {5, 0x2705045, 460, 477}, {5, 0x2705046, 468, 477}, {5, 0x2705047, 1160, 477},  // Vibration frame
    {5, 0x2705048, 462, 479}, {5, 0x2705049, 466, 479}, {5, 0x270504A, 742, 479},   // its label images
    {1, 0x270100D, 461, 478},                                                       // its label
    {1, 0x2701005, 461, 140},                                                       // the highlight
    {1, 0x270100E, 755, 483}, {1, 0x270100F, 956, 483},                             // On / Off
};

const std::vector<Control> kTitleOption = {
    {5, 0x4105004, 70, 47},                                                          // panel side
    {4, 0x4104002, 92, 634},                                                         // help
    {5, 0x4105043, 175, 547}, {5, 0x4105044, 183, 547}, {5, 0x4105045, 1075, 547},  // the row's frame
    {5, 0x4105046, 177, 549}, {5, 0x4105047, 181, 549}, {5, 0x4105048, 1077, 549},  // its label images
    {1, 0x410100D, 176, 548},                                                        // its label
};

const Scene& FindScene(const Layout& l, uint32_t id) {
  return *std::find_if(l.scenes.begin(), l.scenes.end(), [&](const Scene& s) { return s.id() == id; });
}

const ControlRecord* FindControl(const Scene& s, uint32_t id, size_t* index = nullptr) {
  for (size_t i = 0; i < s.controls.size(); ++i) {
    if (s.controls[i].id == id) {
      if (index) *index = i;
      return &s.controls[i];
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("layouts parse and serialize back byte for byte") {
  const auto data = Build({{0x2700000, kConfig}, {0x1300000, {{4, 0x1304001, 5, 6}, {2, 0x1302001, 7, 8}}}});
  std::string error;
  const auto layout = Parse(data, &error);
  REQUIRE(layout);
  CHECK(layout->scenes.size() == 2);
  CHECK(layout->scenes[0].controls.size() == kConfig.size());
  CHECK(layout->scenes[1].placements[1].x == 7);
  CHECK(Serialize(*layout) == data);
}

TEST_CASE("a list box with the extension block is 128 bytes longer") {
  auto data = Build({{0x1300000, {{2, 0x1302001, 0, 0}}}});
  // Mark the list box as extended and append the extension block.
  const size_t record = 44 + 100 + 12;
  Put32(data, record + 172, 1);
  data.insert(data.end(), 128, 0);
  std::string error;
  const auto layout = Parse(data, &error);
  REQUIRE(layout);
  CHECK(layout->scenes[0].controls[0].bytes.size() == 308);
  CHECK(Serialize(*layout) == data);
}

TEST_CASE("malformed layouts are refused with a reason") {
  const auto good = Build({{0x2700000, kConfig}});
  std::string error;
  auto bad_magic = good;
  bad_magic[0] = 'X';
  CHECK_FALSE(Parse(bad_magic, &error));
  CHECK(error == "not a BlueSide UI layout");
  CHECK_FALSE(Parse(std::span(good).first(good.size() - 1), &error));
  auto extra = good;
  extra.push_back(0);
  CHECK_FALSE(Parse(extra, &error));
  CHECK(error == "trailing bytes after the last scene");
  auto wrong_id = good;
  Put32(wrong_id, 44 + 100, 0x2705999);  // first placement no longer matches its record
  CHECK_FALSE(Parse(wrong_id, &error));
  auto bad_type = good;
  Put32(bad_type, 44 + 100 + 12 * kConfig.size(), 9);
  CHECK_FALSE(Parse(bad_type, &error));
}

TEST_CASE("the PC Settings row is cloned from Vibration one row lower (no toggle), with the panel grown") {
  const auto data = Build({{0x1300000, {{4, 0x1304001, 5, 6}}}, {0x2700000, kConfig}});
  const auto original = data;
  std::string error;
  const auto extended = AddSettingsActionRow(data, 0xFF00, &error);
  REQUIRE(extended);
  CHECK(data == original);
  const auto before = Parse(data, &error);
  const auto after = Parse(*extended, &error);
  REQUIRE(after);
  CHECK(FindScene(*after, 0x1300000).controls.size() == 1);  // other scenes untouched
  CHECK(Serialize(Layout{after->header, {FindScene(*after, 0x1300000)}}) ==
        Serialize(Layout{before->header, {FindScene(*before, 0x1300000)}}));
  const Scene& scene = FindScene(*after, 0x2700000);
  CHECK(scene.controls.size() == kConfig.size() + 7);

  const std::vector<std::pair<uint32_t, uint32_t>> clones = {
      {0x2705045, 0x2705080}, {0x2705046, 0x2705081}, {0x2705047, 0x2705082},
      {0x2705048, 0x2705083}, {0x2705049, 0x2705084}, {0x270504A, 0x2705085},
      {0x270100D, kPcRowButton}};
  for (const auto& [old_id, new_id] : clones) {
    size_t i = 0, j = 0;
    REQUIRE(FindControl(scene, old_id, &i));
    REQUIRE(FindControl(scene, new_id, &j));
    CHECK(j == i + 1);  // right after its original: the same drawing order relative to the highlight
    CHECK(scene.placements[j].id == new_id);
    CHECK(scene.placements[j].x == scene.placements[i].x);
    CHECK(scene.placements[j].y == scene.placements[i].y + kRowSpacing);
    CHECK(scene.controls[j].bytes.size() == scene.controls[i].bytes.size());
  }
  const ControlRecord* label = FindControl(scene, kPcRowButton);
  CHECK(Get32(label->bytes, 20) == 0xFF00);
  CHECK(Get32(label->bytes, 104) == 0x2705083);
  CHECK(Get32(label->bytes, 116) == 0x2705084);
  CHECK(Get32(label->bytes, 128) == 0x2705085);
  CHECK(Get32(FindControl(scene, 0x2705084)->bytes, 376) == kPcRowButton);
  CHECK(Get32(FindControl(scene, 0x270100D)->bytes, 20) == 1882);  // the original keeps its label
  for (uint32_t side : {0x2705004u, 0x2705005u, 0x2705006u}) {
    CHECK(Get32(FindControl(scene, side)->bytes, 12) == 30 + uint32_t(kRowSpacing));
  }
  for (uint32_t below : {0x2705007u, 0x2705008u, 0x2705009u, 0x2704001u}) {
    size_t i = 0;
    FindControl(scene, below, &i);
    const auto& old_scene = FindScene(*before, 0x2700000);
    size_t k = 0;
    FindControl(old_scene, below, &k);
    CHECK(scene.placements[i].y == old_scene.placements[k].y + kRowSpacing);
  }
  CHECK(FindControl(scene, 0x2701005));  // the highlight stays where it was
  CHECK_FALSE(FindControl(scene, 0x2701081));  // no toggle halves
  CHECK_FALSE(FindControl(scene, 0x2701082));

  CHECK_FALSE(AddSettingsActionRow(*extended, 0xFF00, &error));  // twice: the new IDs are taken
  const auto keep_label = AddSettingsActionRow(data, 0, &error);
  REQUIRE(keep_label);
  CHECK(Get32(FindControl(FindScene(*Parse(*keep_label, &error), 0x2700000), kPcRowButton)->bytes, 20) == 1882);
}

TEST_CASE("a layout without the Settings scene or its Vibration row is left alone") {
  std::string error;
  CHECK_FALSE(AddSettingsActionRow(Build({{0x1300000, {{4, 0x1304001, 5, 6}}}}), 0, &error));
  CHECK(error == "no SN_PROFILE_OPTION_CONFIG scene");
  std::vector<Control> without_vibration(kConfig.begin(), kConfig.begin() + 7);
  CHECK_FALSE(AddSettingsActionRow(Build({{0x2700000, without_vibration}}), 0, &error));
}

TEST_CASE("the title SETTINGS gets a PC Settings row below Change Storage Device; nothing else moves") {
  const auto data = Build({{0x1300000, {{4, 0x1304001, 5, 6}}}, {0x4100000, kTitleOption}});
  std::string error;
  const auto extended = AddTitleSettingsActionRow(data, 0xFF00, &error);
  REQUIRE(extended);
  const auto before = Parse(data, &error);
  const auto after = Parse(*extended, &error);
  REQUIRE(after);
  const Scene& scene = FindScene(*after, 0x4100000);
  CHECK(scene.controls.size() == kTitleOption.size() + 7);
  for (const auto& [old_id, new_id] : std::vector<std::pair<uint32_t, uint32_t>>{
           {0x4105043, 0x4105080}, {0x4105044, 0x4105081}, {0x4105045, 0x4105082}, {0x4105046, 0x4105083},
           {0x4105047, 0x4105084}, {0x4105048, 0x4105085}, {0x410100D, kTitlePcRowButton}}) {
    size_t i = 0, j = 0;
    REQUIRE(FindControl(scene, old_id, &i));
    REQUIRE(FindControl(scene, new_id, &j));
    CHECK(j == i + 1);
    CHECK(scene.placements[j].y == scene.placements[i].y + kRowSpacing);
  }
  const ControlRecord* label = FindControl(scene, kTitlePcRowButton);
  CHECK(Get32(label->bytes, 20) == 0xFF00);
  CHECK(Get32(label->bytes, 104) == 0x4105083);
  CHECK(Get32(FindControl(scene, 0x4105083)->bytes, 376) == kTitlePcRowButton);
  CHECK(Get32(FindControl(scene, 0x410100D)->bytes, 20) == 1645);  // the original keeps its label
  // The panel and the help text are unchanged.
  const Scene& old_scene = FindScene(*before, 0x4100000);
  for (uint32_t id : {0x4105004u, 0x4104002u}) {
    size_t i = 0, k = 0;
    FindControl(scene, id, &i);
    FindControl(old_scene, id, &k);
    CHECK(scene.placements[i].y == old_scene.placements[k].y);
    CHECK(scene.controls[i].bytes == old_scene.controls[k].bytes);
  }
  CHECK_FALSE(AddTitleSettingsActionRow(*extended, 0xFF00, &error));  // the new IDs are taken
  CHECK_FALSE(AddTitleSettingsActionRow(Build({{0x2700000, kConfig}}), 0, &error));
  CHECK(error == "no SN_TITLE_OPTION scene");
}

TEST_CASE("the title list gets rows below CREDITS (M7): cloned with new IDs, the list moved up a row each") {
  // The title list as the game has it: seven frame images (owned by their buttons), the highlight's image, seven row
  // buttons and the highlight button; CREDITS (0x3901005) is the last row at y 650, its frame 0x3905011 at 647.
  const uint32_t rows[] = {0x3901001, 0x3901002, 0x3901006, 0x3901004, 0x3901003, 0x3901008, 0x3901005};
  const uint32_t frames[] = {0x3905002, 0x3905005, 0x3905008, 0x390500B, 0x390500E, 0x3905003, 0x3905011};
  std::vector<Control> list;
  for (int k = 0; k < 7; ++k) list.push_back({5, frames[k], 440, 479 + 28 * k});
  list.push_back({5, 0x3905001, 455, 479});
  for (int k = 0; k < 7; ++k) list.push_back({1, rows[k], 390, 482 + 28 * k});
  list.push_back({1, 0x3901007, 390, 482});
  auto data = Build({{0x3900000, list}});
  // Ownership as in the game: each frame's +376 names its button; CREDITS's state image is its frame.
  {
    std::string error;
    auto layout = Parse(data, &error);
    REQUIRE(layout);
    for (auto& c : layout->scenes[0].controls) {
      for (int k = 0; k < 7; ++k) {
        if (c.id == frames[k]) Put32(c.bytes, 376, rows[k]);
        if (c.id == rows[k]) Put32(c.bytes, 104, frames[k]);
      }
    }
    data = Serialize(*layout);
  }
  std::string error;
  const auto extended = AddTitleMainRows(data, {0xFF01, 0xFF02}, &error);
  REQUIRE(extended);
  const auto after = Parse(*extended, &error);
  REQUIRE(after);
  const Scene& scene = FindScene(*after, 0x3900000);
  CHECK(scene.controls.size() == list.size() + 4);
  size_t credits = 0, first = 0, second = 0, credits_frame = 0, first_frame = 0, highlight = 0;
  REQUIRE(FindControl(scene, 0x3901005, &credits));
  REQUIRE(FindControl(scene, kTitleMainRowButton, &first));
  REQUIRE(FindControl(scene, kTitleMainRowButton + 1, &second));
  REQUIRE(FindControl(scene, 0x3905011, &credits_frame));
  REQUIRE(FindControl(scene, kTitleMainRowImage, &first_frame));
  REQUIRE(FindControl(scene, 0x3901007, &highlight));
  CHECK(first == credits + 1);
  CHECK(second == credits + 2);
  CHECK(highlight > second);  // drawn on top of the new rows
  CHECK(first_frame == credits_frame + 1);
  CHECK(scene.placements[credits].y == 650 - 56);  // moved up two rows
  CHECK(scene.placements[highlight].y == 482 - 56);
  CHECK(scene.placements[first].y == 650 - 56 + 28);
  CHECK(scene.placements[second].y == 650);  // the list still ends where it did
  CHECK(scene.placements[first_frame].y == 647 - 56 + 28);
  CHECK(Get32(scene.controls[first].bytes, 20) == 0xFF01);
  CHECK(Get32(scene.controls[second].bytes, 20) == 0xFF02);
  CHECK(Get32(scene.controls[first].bytes, 104) == kTitleMainRowImage);
  CHECK(Get32(scene.controls[first_frame].bytes, 376) == kTitleMainRowButton);
  CHECK(Get32(scene.controls[credits].bytes, 104) == 0x3905011);  // the original keeps its own
  CHECK_FALSE(AddTitleMainRows(*extended, {0xFF01}, &error));  // the new IDs are taken
  CHECK(error == "the new title list IDs are taken");
  CHECK_FALSE(AddTitleMainRows(Build({{0x2700000, kConfig}}), {0xFF01}, &error));
  CHECK(error == "no title list scene");
}

TEST_CASE("a text control is cloned with a new ID, place, size and font, its text set by code (M7 UX fix 4)") {
  const auto data = Build({{0x04100000, {{4, 0x4104002, 92, 634}, {1, 0x4101001, 176, 135}}}});
  std::string error;
  const auto out = AddTextClone(data, 0x04100000, 0x4104002, 0x4104080, 480, 150, 600, 300, 13, &error);
  REQUIRE(out);
  const auto layout = Parse(*out, &error);
  REQUIRE(layout);
  const Scene& scene = layout->scenes[0];
  REQUIRE(scene.controls.size() == 3);
  const ControlRecord& text = scene.controls.back();  // drawn last
  CHECK(text.type == 4);
  CHECK(text.id == 0x4104080);
  CHECK(Get32(text.bytes, 8) == 600);
  CHECK(Get32(text.bytes, 12) == 300);
  CHECK(Get32(text.bytes, 20) == 0xFFFFFFFFu);
  CHECK(Get32(text.bytes, 40) == 13);
  CHECK(scene.placements.back().id == 0x4104080);
  CHECK(scene.placements.back().x == 480);
  CHECK(scene.placements.back().y == 150);
  // The source and the other controls are unchanged.
  CHECK(scene.placements[0].y == 634);
  // A taken ID or a source that is not a text control: refused.
  CHECK_FALSE(AddTextClone(*out, 0x04100000, 0x4104002, 0x4104080, 0, 0, 1, 1, 0, &error));
  CHECK_FALSE(AddTextClone(data, 0x04100000, 0x4101001, 0x4104081, 0, 0, 1, 1, 0, &error));
  CHECK_FALSE(AddTextClone(data, 0x02700000, 0x4104002, 0x4104081, 0, 0, 1, 1, 0, &error));
}

TEST_CASE("controls are moved by their placements; a missing one refuses the whole move (M7 UX fix 5)") {
  const auto data = Build({{0x04100000, {{1, 0x410100D, 176, 548}, {5, 0x4105043, 175, 547}, {1, 0x4101080, 176, 594}}}});
  std::string error;
  auto out = MoveControls(data, 0x04100000, {0x410100D, 0x4105043}, 0, 4000, &error);
  REQUIRE(out);
  out = MoveControls(*out, 0x04100000, {0x4101080}, 0, -46, &error);
  REQUIRE(out);
  const auto layout = Parse(*out, &error);
  REQUIRE(layout);
  CHECK(layout->scenes[0].placements[0].y == 4548);
  CHECK(layout->scenes[0].placements[1].y == 4547);
  CHECK(layout->scenes[0].placements[2].y == 548);
  CHECK_FALSE(MoveControls(data, 0x04100000, {0x410100D, 0x4109999}, 0, 10, &error));
  CHECK(error.find("4109999") != std::string::npos);
}

TEST_CASE("image controls are cloned under a control, in place or placed anew, with their alpha (M7 menu overhaul)") {
  const auto data = Build({{0x00F00000, {{4, 0xF04001, 400, 140}, {5, 0xF05001, 309, 110}, {5, 0xF05005, 309, 126}}}});
  std::string error;
  const auto out = AddImageClones(data, 0x00F00000,
                                  {{0xF05005, 0xF05080, true, 0, 0, 1280, 720, 150}, {0xF05001, 0xF05081}}, 0xF05001,
                                  &error);
  REQUIRE(out);
  const auto layout = Parse(*out, &error);
  REQUIRE(layout);
  const Scene& scene = layout->scenes[0];
  REQUIRE(scene.controls.size() == 5);
  // Inserted in order just before the panel's first image (drawn under it), after the text before it.
  CHECK(scene.controls[0].id == 0xF04001);
  CHECK(scene.controls[1].id == 0xF05080);
  CHECK(scene.controls[2].id == 0xF05081);
  CHECK(scene.controls[3].id == 0xF05001);
  CHECK(scene.placements[1].id == 0xF05080);
  CHECK(scene.placements[1].x == 0);
  CHECK(scene.placements[1].y == 0);
  CHECK(Get32(scene.controls[1].bytes, 4) == 0xF05080);
  CHECK(Get32(scene.controls[1].bytes, 8) == 1280);
  CHECK(Get32(scene.controls[1].bytes, 12) == 720);
  CHECK(Get32(scene.controls[1].bytes, 84) == 150);
  // In place: the source's placement, size and alpha.
  CHECK(scene.placements[2].x == 309);
  CHECK(scene.placements[2].y == 110);
  CHECK(Get32(scene.controls[2].bytes, 8) == 100);
  CHECK(Get32(scene.controls[2].bytes, 84) == Get32(scene.controls[3].bytes, 84));
  // A taken ID, a source that is not an image, or no control to insert before: refused.
  CHECK_FALSE(AddImageClones(*out, 0x00F00000, {{0xF05001, 0xF05080}}, 0, &error));
  CHECK_FALSE(AddImageClones(data, 0x00F00000, {{0xF04001, 0xF05082}}, 0, &error));
  CHECK_FALSE(AddImageClones(data, 0x00F00000, {{0xF05001, 0xF05082}}, 0xF05999, &error));
  CHECK(AddImageClones(data, 0x00F00000, {{0xF05001, 0xF05082}}, 0, &error));  // added last
}
