#pragma once

#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"
#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"

#include <array>
#include <cstdint>

namespace minish::foreign_world::zelda1 {

// This is deliberately a source-coordinate control model.  It is not a
// Minish movement adapter: x/y are the NES ObjX/ObjY values used by the
// source's cave routines.
struct CaveSourcePosition {
  std::uint8_t x = 0;
  std::uint8_t y = 0;
};

enum class CaveDirection : std::uint8_t { kRight = 1, kLeft = 2, kDown = 4, kUp = 8 };

enum class StartCaveMoveResult : std::uint8_t {
  // CaveDirection is a public, byte-backed type.  Reject a cast/untrusted
  // value explicitly rather than treating it as a successful no-op move.
  kInvalidDirection,
  kMoved,
  kBlocked,
  kExited,
  kNotLoaded,
  kEntering,
  kDialogueBlocked,
};

enum class StartCaveSwordEligibility : std::uint8_t {
  kEligible,
  kNotLoaded,
  kEntering,
  kDialoguePending,
  kAlreadyTaken,
  kWrongPosition,
};

// Facts needed to draw the static Old Man without importing a guest actor or
// inventing a host sprite. pattern is the decoded source CHR tile, while
// palette is the source sprite palette row selected by the descriptor.
struct StartCaveOldManSpriteFacts {
  std::uint8_t object_type = 0x6a;
  CaveSourcePosition left{0x78, 0x80};
  CaveSourcePosition right{0x80, 0x80};
  std::uint8_t tile = 0x98;
  std::uint8_t left_attribute = 0x02;
  std::uint8_t right_attribute = 0x42;
  std::array<std::uint8_t, 16> pattern{};
  std::array<std::uint8_t, 4> palette{};
};

// Extracts only the Old Man's source assets.  This does not draw or mutate a
// framebuffer, so the active renderer/session can choose its own composition.
bool get_first_quest_start_cave_old_man_sprite(const FirstQuestData& data,
                                               StartCaveOldManSpriteFacts* out);

// Exact predicate from Z_01.asm:UpdateCavePersonState_TalkOrShopOrDoorCharge.
// It is public for tests and for a UI that needs to show a pickup affordance.
constexpr bool is_first_quest_start_sword_pickup_position(CaveSourcePosition p) {
  return p.x == 0x78 && p.y >= 0x93 && p.y <= 0x9d;
}

// A tiny, deterministic control-only model for the OW77 normal cave.  It
// owns no inventory and emits no dialogue pixels.  The caller explicitly
// acknowledges the source textbox before movement is allowed, then commits a
// successful sword acquisition with record_start_sword_taken().
class Zelda1StartCaveControl {
 public:
  static constexpr CaveSourcePosition kEntryStart{0x70, 0xdd};
  static constexpr CaveSourcePosition kEntrySettled{0x70, 0xad};
  static constexpr std::uint8_t kPersonUpperBarrierY = 0x8e;
  static constexpr std::uint8_t kExitY = 0xdd;

  bool load(const FirstQuestData& data);

  [[nodiscard]] bool loaded() const { return loaded_; }
  [[nodiscard]] bool entering() const { return entering_; }
  [[nodiscard]] bool dialogue_acknowledged() const { return dialogue_acknowledged_; }
  [[nodiscard]] bool start_sword_taken() const { return start_sword_taken_; }
  [[nodiscard]] CaveSourcePosition position() const { return position_; }
  [[nodiscard]] const OwFinalTileMap& final_tiles() const { return final_tiles_; }

  // Models the source's automatic $30-pixel walk from $dd to $ad as one
  // atomic control transition.  Per-frame speed/grid-offset timing is outside
  // this pure component.
  bool settle_entry();
  bool acknowledge_dialogue();
  StartCaveMoveResult move_one(CaveDirection direction);
  StartCaveSwordEligibility start_sword_eligibility() const;
  bool record_start_sword_taken();

 private:
  bool tile_is_walkable(std::int16_t tile_x, std::int16_t tile_y) const;
  bool source_move_is_walkable(CaveDirection direction) const;

  OwFinalTileMap final_tiles_{};
  CaveSourcePosition position_{};
  bool loaded_ = false;
  bool entering_ = false;
  bool dialogue_acknowledged_ = false;
  bool start_sword_taken_ = false;
};

}  // namespace minish::foreign_world::zelda1
