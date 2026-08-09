#include "foreign_worlds/zelda1/zelda1_cave_control.h"
#include "foreign_worlds/zelda1/zelda1_cave_renderer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace z1 = minish::foreign_world::zelda1;

namespace {
int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << "\n";
  return 1;
}

bool write_frame_ppm(const std::filesystem::path& path, const z1::OwFramebuffer& frame) {
  std::ofstream ppm(path, std::ios::binary);
  if (!ppm) return false;
  ppm << "P6\n240 160\n255\n";
  for (const auto pixel : frame) {
    const unsigned r = pixel & 31, g = (pixel >> 5) & 31, b = (pixel >> 10) & 31;
    ppm.put(static_cast<char>((r << 3) | (r >> 2)));
    ppm.put(static_cast<char>((g << 3) | (g >> 2)));
    ppm.put(static_cast<char>((b << 3) | (b >> 2)));
  }
  return static_cast<bool>(ppm);
}

bool same_rectangle(const z1::OwFramebuffer& first, const z1::OwFramebuffer& second,
                    unsigned x, unsigned y, unsigned width, unsigned height) {
  for (unsigned py = y; py < y + height; ++py)
    for (unsigned px = x; px < x + width; ++px)
      if (first[py * z1::kOwRenderWidth + px] != second[py * z1::kOwRenderWidth + px])
        return false;
  return true;
}

int test_synthetic() {
  if (!z1::is_first_quest_start_sword_pickup_position({0x78, 0x93}) ||
      !z1::is_first_quest_start_sword_pickup_position({0x78, 0x9d}) ||
      z1::is_first_quest_start_sword_pickup_position({0x77, 0x98}) ||
      z1::is_first_quest_start_sword_pickup_position({0x78, 0x92}) ||
      z1::is_first_quest_start_sword_pickup_position({0x78, 0x9e}))
    return fail("source sword hotspot predicate changed");
  z1::Zelda1StartCaveControl control;
  if (control.move_one(static_cast<z1::CaveDirection>(0xff)) !=
      z1::StartCaveMoveResult::kInvalidDirection)
    return fail("forged CaveDirection was not rejected");
  if (control.move_one(z1::CaveDirection::kDown) != z1::StartCaveMoveResult::kNotLoaded ||
      control.start_sword_eligibility() != z1::StartCaveSwordEligibility::kNotLoaded ||
      control.settle_entry() || control.acknowledge_dialogue())
    return fail("unloaded cave control did not fail closed");

  std::string error;
  auto prg = z1::VerifiedPrg::from_verified_bytes(
      std::vector<std::uint8_t>(z1::kVerifiedPrgSize), &error);
  if (!prg) return fail("valid-shape synthetic PRG was rejected");
  auto data = z1::FirstQuestData::create(std::move(*prg), &error);
  if (!data || control.load(*data))
    return fail("invalid synthetic cave tables were accepted");
  return 0;
}

int test_actual(const char* path) {
  std::ifstream input(path, std::ios::binary);
  std::vector<std::uint8_t> ines((std::istreambuf_iterator<char>(input)), {});
  if (ines.size() != 16 + z1::kVerifiedPrgSize)
    return fail("actual cave test requires a 128 KiB-PRG iNES image");
  std::vector<std::uint8_t> prg_bytes(ines.begin() + 16, ines.end());
  std::string error;
  auto prg = z1::VerifiedPrg::from_verified_bytes(std::move(prg_bytes), &error);
  if (!prg) return fail("actual PRG rejected: " + error);
  auto data = z1::FirstQuestData::create(std::move(*prg), &error);
  if (!data) return fail("actual first-quest data rejected: " + error);
  std::vector<std::uint8_t> patterns;
  z1::OwFramebuffer before_dialogue{}, dialogue_complete{}, sword_taken{};
  z1::FirstQuestStartCaveFacts render_facts{};
  if (!z1::copy_overworld_background_from_verified_prg(*data, &patterns, &error) ||
      !z1::render_first_quest_start_cave(*data, patterns, {false, false},
                                         &before_dialogue, &render_facts) ||
      !z1::render_first_quest_start_cave(*data, patterns, {false, true},
                                         &dialogue_complete) ||
      !z1::render_first_quest_start_cave(*data, patterns, {true, true},
                                         &sword_taken))
    return fail("source cave sprite render failed: " + error);
  // InitCave creates both the Old Man and fires before source dialogue
  // acknowledgement, while the persistent sword bit removes only the person.
  if (render_facts.old_man_visual_is_opaque || !render_facts.old_man_visible_before_sword ||
      !render_facts.fire_static_frame_zero || !render_facts.fire_visible_before_and_after_sword ||
      before_dialogue != dialogue_complete || before_dialogue == sword_taken ||
      !same_rectangle(before_dialogue, sword_taken, 0x48 - 8, 0x80 - 72, 16, 8) ||
      !same_rectangle(before_dialogue, sword_taken, 0xa8 - 8, 0x80 - 72, 16, 8) ||
      same_rectangle(before_dialogue, sword_taken, 0x78 - 8, 0x80 - 72, 16, 8))
    return fail("Old Man/fire source visibility contract changed");
  std::filesystem::create_directories("build");
  if (!write_frame_ppm("build/zelda1_start_cave_before_dialogue.ppm", before_dialogue) ||
      !write_frame_ppm("build/zelda1_start_cave_dialogue_complete.ppm", dialogue_complete) ||
      !write_frame_ppm("build/zelda1_start_cave_sword_taken.ppm", sword_taken))
    return fail("could not write source cave sprite QA artifacts");
  z1::Zelda1StartCaveControl control;
  z1::StartCaveOldManSpriteFacts old_man{};
  if (!control.load(*data) || control.position().x != 0x70 || control.position().y != 0xdd ||
      !control.entering() || !z1::get_first_quest_start_cave_old_man_sprite(*data, &old_man) ||
      old_man.object_type != 0x6a || old_man.left.x != 0x78 || old_man.left.y != 0x80 ||
      old_man.right.x != 0x80 || old_man.tile != 0x98 ||
      old_man.left_attribute != 0x02 || old_man.right_attribute != 0x42 ||
      old_man.palette != std::array<std::uint8_t, 4>{{0x0f, 0x16, 0x27, 0x30}})
    return fail("source Old Man facts or cave entry changed");
  if (control.move_one(z1::CaveDirection::kUp) != z1::StartCaveMoveResult::kEntering ||
      !control.settle_entry() || control.position().y != 0xad ||
      control.move_one(z1::CaveDirection::kDown) != z1::StartCaveMoveResult::kDialogueBlocked ||
      control.start_sword_eligibility() != z1::StartCaveSwordEligibility::kDialoguePending ||
      !control.acknowledge_dialogue())
    return fail("entry/dialogue control state changed");
  const auto before_invalid_direction = control.position();
  if (control.move_one(static_cast<z1::CaveDirection>(0x03)) !=
          z1::StartCaveMoveResult::kInvalidDirection ||
      control.position().x != before_invalid_direction.x ||
      control.position().y != before_invalid_direction.y)
    return fail("loaded cave accepted or moved for a forged CaveDirection");

  // The source's person barrier uses the pre-move coordinate: y=$8e may move
  // to $8d, while y=$8d cannot move further up.
  for (unsigned i = 0; i != 31; ++i)
    if (control.move_one(z1::CaveDirection::kUp) != z1::StartCaveMoveResult::kMoved)
      return fail("walkable cave floor did not lead to the person barrier");
  if (control.position().y != 0x8e ||
      control.move_one(z1::CaveDirection::kUp) != z1::StartCaveMoveResult::kMoved ||
      control.position().y != 0x8d ||
      control.move_one(z1::CaveDirection::kUp) != z1::StartCaveMoveResult::kBlocked)
    return fail("source upper-half person barrier changed");
  // Reload to exercise the isolated bottom throat and pickup gate without
  // encoding a host pathfinder in this focused source test.
  if (!control.load(*data) || !control.settle_entry() || !control.acknowledge_dialogue())
    return fail("could not reset actual cave control");
  while (control.position().y != 0x98) {
    const auto result = control.position().y < 0x98 ? control.move_one(z1::CaveDirection::kDown)
                                                     : control.move_one(z1::CaveDirection::kUp);
    if (result != z1::StartCaveMoveResult::kMoved) return fail("cave center aisle changed");
  }
  while (control.position().x != 0x78) {
    const auto result = control.position().x < 0x78 ? control.move_one(z1::CaveDirection::kRight)
                                                     : control.move_one(z1::CaveDirection::kLeft);
    if (result != z1::StartCaveMoveResult::kMoved) return fail("cave sword aisle changed");
  }
  if (control.start_sword_eligibility() != z1::StartCaveSwordEligibility::kEligible ||
      !control.record_start_sword_taken() ||
      control.start_sword_eligibility() != z1::StartCaveSwordEligibility::kAlreadyTaken)
    return fail("source sword pickup control changed");
  while (control.position().y != 0xdd)
    if (control.move_one(z1::CaveDirection::kDown) != z1::StartCaveMoveResult::kMoved)
      return fail("source cave bottom throat no longer reaches its exit edge");
  if (control.move_one(z1::CaveDirection::kDown) != z1::StartCaveMoveResult::kExited)
    return fail("source cave exit was not recognized at exact Y=$dd");
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (const int result = test_synthetic(); result != 0) return result;
  if (argc == 1) {
    std::cout << "Zelda 1 cave control synthetic tests passed without ROM assets\n";
    return 0;
  }
  if (argc != 2) return fail("usage: test_zelda1_cave_control [hash-validated-ines-path]");
  return test_actual(argv[1]);
}
