#include "foreign_worlds/zelda1/zelda1_cave_control.h"
#include "foreign_worlds/zelda1/zelda1_cave_renderer.h"

#include <algorithm>
#include <array>
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

bool different_rectangle(const z1::OwFramebuffer& first, const z1::OwFramebuffer& second,
                         unsigned x, unsigned y, unsigned width, unsigned height) {
  return !same_rectangle(first, second, x, y, width, height);
}

std::uint64_t framebuffer_hash(const z1::OwFramebuffer& frame) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto pixel : frame) {
    hash ^= pixel;
    hash *= 1099511628211ull;
  }
  return hash;
}

char source_text_glyph(std::uint8_t glyph) {
  if (glyph >= 0x0a && glyph <= 0x23)
    return static_cast<char>('A' + glyph - 0x0a);
  switch (glyph) {
  case 0x24:
  case 0x25: return ' ';
  case 0x29: return '!';
  case 0x2a: return '\'';
  case 0x2c: return '.';
  default: return '?';
  }
}

bool decode_start_cave_dialogue_rows(
    const z1::FirstQuestStartCaveDialogueFacts& facts,
    std::array<std::string, 3>* rows) {
  if (!rows) return false;
  rows->fill({});
  unsigned row = 0;
  for (const std::uint8_t encoded : facts.encoded) {
    const std::uint8_t glyph = encoded & 0x3f;
    (*rows)[row].push_back(source_text_glyph(glyph));
    const std::uint8_t marker = encoded & 0xc0;
    if (marker == 0) continue;
    if (marker == 0xc0) return row == 1;
    row = marker == 0x40 ? 2 : 1;
  }
  return false;
}

bool rendered_text_tile_uses_palette(const z1::OwFramebuffer& frame,
                                     std::span<const std::uint8_t> prg,
                                     std::uint8_t tile, std::uint8_t vram_low,
                                     const std::array<std::uint8_t, 4>& palette) {
  // The first visible source glyph is `I` ($12) at `$21A6`, after the two
  // no-delay `$25` spaces. It is clear of every OAM sprite in the start cave,
  // so this checks the renderer's actual composited BGR555 output rather than
  // merely repeating the source palette calculation.
  constexpr std::size_t kCommonBackgroundPatternPrgOffset = 34687;
  if (kCommonBackgroundPatternPrgOffset + (static_cast<std::size_t>(tile) + 1) * 16 >
      prg.size())
    return false;
  const int start_x = static_cast<int>((vram_low & 0x1f) * 8) - 8;
  const int start_y = static_cast<int>((vram_low >> 5) * 8) - 8;
  const auto glyph = prg.subspan(kCommonBackgroundPatternPrgOffset +
                                     static_cast<std::size_t>(tile) * 16,
                                 16);
  bool saw_ink = false;
  for (unsigned py = 0; py < 8; ++py)
    for (unsigned px = 0; px < 8; ++px) {
      const auto color = ((glyph[py] >> (7 - px)) & 1) |
                         (((glyph[8 + py] >> (7 - px)) & 1) << 1);
      saw_ink = saw_ink || color != 0;
      const auto expected = z1::nes_to_bgr555(palette[color] & 63);
      if (frame[(start_y + static_cast<int>(py)) * z1::kOwRenderWidth +
                start_x + static_cast<int>(px)] != expected)
        return false;
    }
  return saw_ink;
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
  z1::OwFramebuffer before_dialogue{}, dialogue_in_progress{}, dialogue_complete{}, sword_taken{},
      fire_phase_one{};
  z1::FirstQuestStartCaveFacts render_facts{};
  z1::FirstQuestStartCaveDialogueFacts dialogue_facts{};
  if (!z1::copy_overworld_background_from_verified_prg(*data, &patterns, &error) ||
      !z1::get_first_quest_start_cave_dialogue(*data, &dialogue_facts) ||
      !z1::render_first_quest_start_cave(*data, patterns, {false, false, 0},
                                         &before_dialogue, &render_facts) ||
      !z1::render_first_quest_start_cave(*data, patterns, {false, false, 10},
                                         &dialogue_in_progress) ||
      !z1::render_first_quest_start_cave(*data, patterns, {false, true, 0},
                                         &dialogue_complete) ||
      !z1::render_first_quest_start_cave(*data, patterns, {true, true, 0},
                                         &sword_taken) ||
      !z1::render_first_quest_start_cave(*data, patterns, {false, false, 0, 1},
                                         &fire_phase_one))
    return fail("source cave sprite render failed: " + error);
  std::array<std::string, 3> dialogue_rows{};
  z1::OverworldRoomView cave_palette_room{};
  if (!decode_start_cave_dialogue_rows(dialogue_facts, &dialogue_rows) ||
      !data->overworld_room(4, 4, &cave_palette_room) ||
      dialogue_facts.line_starts != std::array<std::uint8_t, 3>{{0xc4, 0xe4, 0xa4}} ||
      dialogue_facts.used_line_starts != std::array<std::uint8_t, 2>{{0xa4, 0xc4}} ||
      dialogue_facts.visible_glyph_count != 37 ||
      dialogue_facts.line_transition_count != 1 || dialogue_facts.final_marker != 0xc0 ||
      dialogue_facts.encoded[21] != 0x98 || dialogue_facts.encoded.back() != 0xec ||
      dialogue_rows[0] != "  IT'S DANGEROUS TO GO" ||
      dialogue_rows[1] != "    ALONE! TAKE THIS." || !dialogue_rows[2].empty())
    return fail("actual-ROM selector-zero textbox decode/line markers changed");
  // Z_05:FillPlayAreaAttrs uses room $44 AttrsB for inner PlayAreaAttrs[$09].
  // InitModeB's transfer writes that byte to `$23D9`, covering `$21A4` and
  // `$21C4`; Z_06's cave transfer writes the selected background palette row
  // `$0f,$30,$00,$12` at PPU `$3f08`.
  if ((cave_palette_room.attributes.attr_a & 3) != 3 ||
      (cave_palette_room.attributes.attr_b & 3) != 2 ||
      render_facts.textbox_background_palette_selector != 2 ||
      render_facts.textbox_background_palette !=
          std::array<std::uint8_t, 4>{{0x0f, 0x30, 0x00, 0x12}} ||
      !rendered_text_tile_uses_palette(
          dialogue_complete, data->prg().bytes(), 0x12, 0xa6,
          render_facts.textbox_background_palette))
    return fail("actual-ROM cave textbox palette/inner attribute selection changed");
  // InitCave creates both the Old Man and fires before source dialogue
  // acknowledgement, while the persistent sword bit removes only the person.
  if (render_facts.old_man_visual_is_opaque || !render_facts.old_man_visible_before_sword ||
      !render_facts.fire_static_frame_zero || !render_facts.fire_visible_before_and_after_sword ||
      before_dialogue == dialogue_in_progress || dialogue_in_progress == dialogue_complete ||
      dialogue_complete == sword_taken ||
      // Z_07 writes PPUCTRL=$30: cave OAM uses the source 8x16 sprite mode.
      // Both fires must persist in their complete 16x16 paired-OAM regions;
      // the Old Man's two 8x16 sides and sword's narrow 8x16 OAM entry must
      // disappear with their respective source state.
      !same_rectangle(before_dialogue, sword_taken, 0x48 - 8, 0x80 - 72, 16, 16) ||
      !same_rectangle(before_dialogue, sword_taken, 0xa8 - 8, 0x80 - 72, 16, 16) ||
      // UpdateStandingFire's ObjAnimFrame is a source horizontal-flip bit:
      // its phase-one OAM pair swaps $5c/$5e and mirrors both 8x16 sides.
      !different_rectangle(before_dialogue, fire_phase_one, 0x48 - 8, 0x80 - 72, 16, 16) ||
      !different_rectangle(before_dialogue, fire_phase_one, 0xa8 - 8, 0x80 - 72, 16, 16) ||
      same_rectangle(before_dialogue, sword_taken, 0x78 - 8, 0x80 - 72, 16, 16) ||
      same_rectangle(before_dialogue, sword_taken, 0x78 + 4 - 8, 0x98 - 72, 8, 16) ||
      // `$21A4`/`$21C4` become crop-space Y=32/40. `$E4` is not used by
      // this source record, so row 48 must not acquire a third line.
      !different_rectangle(before_dialogue, dialogue_complete, 24, 32, 208, 8) ||
      !different_rectangle(before_dialogue, dialogue_complete, 24, 40, 208, 8) ||
      different_rectangle(before_dialogue, dialogue_complete, 24, 48, 208, 8))
    return fail("Old Man/fire source visibility contract changed");
  std::filesystem::create_directories("build");
  if (!write_frame_ppm("build/zelda1_start_cave_text_before.ppm", before_dialogue) ||
      !write_frame_ppm("build/zelda1_start_cave_text_in_progress.ppm", dialogue_in_progress) ||
      !write_frame_ppm("build/zelda1_start_cave_text_visible.ppm", dialogue_complete) ||
      !write_frame_ppm("build/zelda1_start_cave_text_after.ppm", sword_taken) ||
      !write_frame_ppm("build/zelda1_start_cave_fire_phase0.ppm", before_dialogue) ||
      !write_frame_ppm("build/zelda1_start_cave_fire_phase1.ppm", fire_phase_one))
    return fail("could not write source cave dialogue QA artifacts");
  z1::Zelda1StartCaveControl control;
  z1::StartCaveOldManSpriteFacts old_man{};
  if (!control.load(*data) || control.position().x != 0x70 || control.position().y != 0xdd ||
      !control.entering() || !z1::get_first_quest_start_cave_old_man_sprite(*data, &old_man) ||
      old_man.object_type != 0x6a || old_man.left.x != 0x78 || old_man.left.y != 0x80 ||
      old_man.right.x != 0x80 || old_man.tile != 0x98 ||
      old_man.left_attribute != 0x02 || old_man.right_attribute != 0x42 ||
      old_man.top_pattern == old_man.bottom_pattern ||
      old_man.palette != std::array<std::uint8_t, 4>{{0x0f, 0x16, 0x27, 0x30}})
    return fail("source Old Man facts or cave entry changed");
  if (control.move_one(z1::CaveDirection::kUp) != z1::StartCaveMoveResult::kEntering ||
      !control.settle_entry() || control.position().y != 0xad ||
      control.move_one(z1::CaveDirection::kDown) != z1::StartCaveMoveResult::kDialogueBlocked ||
      control.start_sword_eligibility() != z1::StartCaveSwordEligibility::kDialoguePending ||
      !control.tick_dialogue() || control.dialogue_visible_character_count() != 1 ||
      control.dialogue_frame_delay() != z1::Zelda1StartCaveControl::kTextboxFramesPerGlyph ||
      control.move_one(z1::CaveDirection::kDown) != z1::StartCaveMoveResult::kDialogueBlocked)
    return fail("entry/dialogue control state changed");
  // Z_04:UpdateStandingFire -> Z_07:AnimateObjectWalking decrements its
  // counter before testing zero, then restores six and XORs ObjAnimFrame.
  // The bounded host canonically enters on the valid source boundary (0,6).
  if (control.standing_fire_animation_frame() != 0 ||
      control.standing_fire_animation_counter() !=
          z1::Zelda1StartCaveControl::kStandingFireFramesPerPhase)
    return fail("standing fire did not begin at canonical source boundary");
  for (unsigned frame = 0;
       frame + 1 != z1::Zelda1StartCaveControl::kStandingFireFramesPerPhase;
       ++frame)
    if (!control.tick_standing_fire()) return fail("standing fire timer stopped");
  if (control.standing_fire_animation_frame() != 0 ||
      control.standing_fire_animation_counter() != 1 || !control.tick_standing_fire() ||
      control.standing_fire_animation_frame() != 1 ||
      control.standing_fire_animation_counter() !=
          z1::Zelda1StartCaveControl::kStandingFireFramesPerPhase)
    return fail("standing fire six-frame source cadence changed");
  if (control.restore_standing_fire_animation(2, 6) ||
      control.restore_standing_fire_animation(1, 0) ||
      control.standing_fire_animation_frame() != 1 ||
      control.standing_fire_animation_counter() !=
          z1::Zelda1StartCaveControl::kStandingFireFramesPerPhase)
    return fail("invalid standing-fire restore mutated control state");
  for (unsigned frame = 0; frame != z1::Zelda1StartCaveControl::kTextboxFramesPerGlyph;
       ++frame)
    if (!control.tick_dialogue()) return fail("source textbox timer stopped");
  if (control.dialogue_visible_character_count() != 2 ||
      control.dialogue_frame_delay() != z1::Zelda1StartCaveControl::kTextboxFramesPerGlyph)
    return fail("source six-frame textbox cadence changed");
  unsigned total_play_frames = 7;
  while (!control.dialogue_acknowledged() && total_play_frames++ != 512)
    if (!control.tick_dialogue()) return fail("source textbox did not make bounded progress");
  if (!control.dialogue_acknowledged() ||
      control.dialogue_visible_character_count() !=
          z1::Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount ||
      control.dialogue_frame_delay() != 0 || total_play_frames != 217)
    return fail("final $C0 textbox marker did not automatically unhalt Link");
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
