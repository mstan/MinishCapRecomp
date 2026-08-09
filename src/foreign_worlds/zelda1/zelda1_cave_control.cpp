#include "foreign_worlds/zelda1/zelda1_cave_control.h"

#include "foreign_worlds/zelda1/zelda1_ow_geometry.h"

#include <algorithm>

namespace minish::foreign_world::zelda1 {
namespace {

constexpr std::size_t kOverworldSpritePatternPrgOffset = 53595;
constexpr std::size_t kOverworldSpritePatternPpuStart = 0x08e0;
constexpr std::size_t kOldManPatternPrgOffset =
    kOverworldSpritePatternPrgOffset + 0x98 * 16 - kOverworldSpritePatternPpuStart;
constexpr std::size_t kOldManSpritePalettePrgOffset = 103195;

bool is_vertical(CaveDirection direction) {
  return direction == CaveDirection::kUp || direction == CaveDirection::kDown;
}

bool is_valid_direction(CaveDirection direction) {
  switch (direction) {
  case CaveDirection::kRight:
  case CaveDirection::kLeft:
  case CaveDirection::kDown:
  case CaveDirection::kUp:
    return true;
  default:
    return false;
  }
}

}  // namespace

bool get_first_quest_start_cave_old_man_sprite(
    const FirstQuestData& data, StartCaveOldManSpriteFacts* out) {
  if (!out) return false;
  const auto prg = data.prg().bytes();
  if (kOldManPatternPrgOffset + 32 > prg.size() ||
      kOldManSpritePalettePrgOffset + 4 > prg.size())
    return false;
  StartCaveOldManSpriteFacts candidate{};
  std::copy_n(prg.begin() + kOldManPatternPrgOffset, candidate.top_pattern.size(),
              candidate.top_pattern.begin());
  std::copy_n(prg.begin() + kOldManPatternPrgOffset + candidate.top_pattern.size(),
              candidate.bottom_pattern.size(), candidate.bottom_pattern.begin());
  std::copy_n(prg.begin() + kOldManSpritePalettePrgOffset,
              candidate.palette.size(), candidate.palette.begin());
  *out = candidate;
  return true;
}

bool Zelda1StartCaveControl::load(const FirstQuestData& data) {
  OwFinalTileMap candidate{};
  if (!build_overworld_normal_cave_final_tile_map(data, &candidate)) return false;
  final_tiles_ = candidate;
  position_ = kEntryStart;
  loaded_ = true;
  entering_ = true;
  dialogue_acknowledged_ = false;
  dialogue_visible_character_count_ = 0;
  dialogue_frame_delay_ = 0;
  standing_fire_animation_frame_ = 0;
  standing_fire_animation_counter_ = kStandingFireFramesPerPhase;
  start_sword_taken_ = false;
  return true;
}

bool Zelda1StartCaveControl::settle_entry() {
  if (!loaded_ || !entering_) return false;
  position_ = kEntrySettled;
  entering_ = false;
  return true;
}

bool Zelda1StartCaveControl::tick_dialogue() {
  if (!loaded_ || entering_ || dialogue_acknowledged_) return false;
  // UpdatePersonState_Textbox first observes ObjTimer+1. A transfer sets it
  // to six; the ordinary object timer tick happens between later calls.
  if (dialogue_frame_delay_ != 0) {
    --dialogue_frame_delay_;
    // Z_07's global timer decrement runs before UpdatePersonState_Textbox.
    // When a six-frame delay reaches zero, that same source frame transfers
    // the next glyph rather than introducing a seventh idle update.
    if (dialogue_frame_delay_ != 0) return true;
  }
  if (dialogue_visible_character_count_ < kFirstQuestStartDialogueGlyphCount) {
    ++dialogue_visible_character_count_;
    if (dialogue_visible_character_count_ == kFirstQuestStartDialogueGlyphCount) {
      // `$EC` is itself the final transferred glyph. Its `$C0` high bits
      // select the first-line address and call UnhaltLink before the routine
      // exits; unlike earlier glyphs, it leaves no delay gate on control.
      dialogue_acknowledged_ = true;
      dialogue_frame_delay_ = 0;
    } else {
      dialogue_frame_delay_ = kTextboxFramesPerGlyph;
    }
    return true;
  }
  return false;
}

bool Zelda1StartCaveControl::tick_standing_fire() {
  if (!loaded_) return false;
  // Exact Z_07:AnimateObjectWalking order: DEC first; when it reaches zero,
  // RollOverAnimCounter stores six and XORs ObjAnimFrame with one.
  if (--standing_fire_animation_counter_ == 0) {
    standing_fire_animation_counter_ = kStandingFireFramesPerPhase;
    standing_fire_animation_frame_ ^= 1;
  }
  return true;
}

bool Zelda1StartCaveControl::restore_dialogue_progress(
    std::uint8_t visible_character_count, std::uint8_t frame_delay) {
  if (!loaded_ || entering_ || dialogue_acknowledged_ ||
      visible_character_count > kFirstQuestStartDialogueGlyphCount ||
      frame_delay > kTextboxFramesPerGlyph)
    return false;
  dialogue_visible_character_count_ = visible_character_count;
  dialogue_frame_delay_ = frame_delay;
  return true;
}

bool Zelda1StartCaveControl::restore_standing_fire_animation(
    std::uint8_t animation_frame, std::uint8_t animation_counter) {
  if (!loaded_ || animation_frame > 1 || animation_counter == 0 ||
      animation_counter > kStandingFireFramesPerPhase)
    return false;
  standing_fire_animation_frame_ = animation_frame;
  standing_fire_animation_counter_ = animation_counter;
  return true;
}

bool Zelda1StartCaveControl::acknowledge_dialogue() {
  if (!loaded_ || entering_) return false;
  dialogue_acknowledged_ = true;
  dialogue_visible_character_count_ = kFirstQuestStartDialogueGlyphCount;
  dialogue_frame_delay_ = 0;
  return true;
}

bool Zelda1StartCaveControl::tile_is_walkable(std::int16_t tile_x,
                                               std::int16_t tile_y) const {
  if (tile_x < 0 || tile_x >= static_cast<std::int16_t>(kOwPlayfieldTileWidth) ||
      tile_y < 0 || tile_y >= static_cast<std::int16_t>(kOwPlayfieldTileHeight))
    return false;
  return ow_final_tile_is_walkable(
      final_tiles_[static_cast<std::size_t>(tile_y) * kOwPlayfieldTileWidth +
                   static_cast<std::size_t>(tile_x)]);
}

bool Zelda1StartCaveControl::source_move_is_walkable(CaveDirection direction) const {
  // Z_07:GetCollidingTileMoving/GetCollidableTile. Link's collision hotspot
  // is -8 for up/left, +8 for down, +16 for right, based on ObjY+$0b.
  std::int16_t sample_x = position_.x;
  std::int16_t sample_y = static_cast<std::int16_t>(position_.y) + 0x0b;
  if (direction == CaveDirection::kUp) {
    sample_y -= 8;
  } else if (direction == CaveDirection::kDown) {
    // The source deliberately avoids adding the down hotspot once the base
    // coordinate is already at/below $dd.
    if (sample_y < 0xdd) sample_y += 8;
  } else if (direction == CaveDirection::kLeft) {
    sample_x -= 8;
  } else if (direction == CaveDirection::kRight) {
    sample_x += 16;
  }
  const std::int16_t tile_x = sample_x / 8;
  const std::int16_t tile_y = (sample_y - 0x40) / 8;
  if (!is_vertical(direction)) return tile_is_walkable(tile_x, tile_y);

  // Vertical movement probes the adjacent tile column too, taking the
  // higher-valued final tile exactly as the source routine does. A boolean
  // conjunction is equivalent because every normalized walkable tile is
  // below the first-solid threshold.
  return tile_is_walkable(tile_x, tile_y) && tile_is_walkable(tile_x + 1, tile_y);
}

StartCaveMoveResult Zelda1StartCaveControl::move_one(CaveDirection direction) {
  if (!is_valid_direction(direction)) return StartCaveMoveResult::kInvalidDirection;
  if (!loaded_) return StartCaveMoveResult::kNotLoaded;
  if (entering_) return StartCaveMoveResult::kEntering;
  if (!dialogue_acknowledged_) return StartCaveMoveResult::kDialogueBlocked;

  // CheckSubroom checks the exit before Walker_CheckTileCollision. It has no
  // X predicate; the source tile map is what confines a reachable Link to the
  // mouth throat.
  if (direction == CaveDirection::kDown && position_.y == kExitY)
    return StartCaveMoveResult::kExited;

  // CheckPersonBlocking is a source-wide upper-half barrier, not a sprite
  // rectangle. It tests the pre-move Y coordinate.
  if (direction == CaveDirection::kUp && position_.y < kPersonUpperBarrierY)
    return StartCaveMoveResult::kBlocked;
  if (!source_move_is_walkable(direction)) return StartCaveMoveResult::kBlocked;

  switch (direction) {
  case CaveDirection::kRight: ++position_.x; break;
  case CaveDirection::kLeft: --position_.x; break;
  case CaveDirection::kDown: ++position_.y; break;
  case CaveDirection::kUp: --position_.y; break;
  default: return StartCaveMoveResult::kInvalidDirection;
  }
  return StartCaveMoveResult::kMoved;
}

StartCaveSwordEligibility Zelda1StartCaveControl::start_sword_eligibility() const {
  if (!loaded_) return StartCaveSwordEligibility::kNotLoaded;
  if (entering_) return StartCaveSwordEligibility::kEntering;
  if (!dialogue_acknowledged_) return StartCaveSwordEligibility::kDialoguePending;
  if (start_sword_taken_) return StartCaveSwordEligibility::kAlreadyTaken;
  return is_first_quest_start_sword_pickup_position(position_)
             ? StartCaveSwordEligibility::kEligible
             : StartCaveSwordEligibility::kWrongPosition;
}

bool Zelda1StartCaveControl::record_start_sword_taken() {
  if (start_sword_eligibility() != StartCaveSwordEligibility::kEligible) return false;
  start_sword_taken_ = true;
  return true;
}

}  // namespace minish::foreign_world::zelda1
