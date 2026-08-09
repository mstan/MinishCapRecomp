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
// inventing a host sprite. Zelda keeps PPUCTRL bit $20 set (Z_07 reset writes
// $30), so every OAM entry is an 8x16 sprite: each descriptor tile identifies
// its upper 8x8 CHR tile and the following tile is its lower half. The palette
// is the source sprite palette row selected by the descriptor.
struct StartCaveOldManSpriteFacts {
  std::uint8_t object_type = 0x6a;
  CaveSourcePosition left{0x78, 0x80};
  CaveSourcePosition right{0x80, 0x80};
  std::uint8_t tile = 0x98;
  std::uint8_t left_attribute = 0x02;
  std::uint8_t right_attribute = 0x42;
  std::array<std::uint8_t, 16> top_pattern{};
  std::array<std::uint8_t, 16> bottom_pattern{};
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

// A tiny, deterministic control-only model for the OW77 normal cave. It owns
// no inventory. Z_01:UpdatePersonState_Textbox advances the selector-zero
// message automatically: it writes one non-$25 glyph, waits six frames, and
// calls UnhaltLink from the record's final $C0 marker. The framebuffer owner
// consumes the visible-glyph cursor below; this control model only preserves
// the source timing/unhalt gate and commits a successful sword acquisition.
class Zelda1StartCaveControl {
 public:
  static constexpr CaveSourcePosition kEntryStart{0x70, 0xdd};
  static constexpr CaveSourcePosition kEntrySettled{0x70, 0xad};
  static constexpr std::uint8_t kPersonUpperBarrierY = 0x8e;
  static constexpr std::uint8_t kExitY = 0xdd;
  // The verified First Quest selector-zero record is 43 encoded bytes: six
  // `$25` no-delay spaces and 37 transferred glyphs. `ObjTimer+1` is set to
  // six after each transfer in Z_01:UpdatePersonState_Textbox.
  static constexpr std::uint8_t kFirstQuestStartDialogueGlyphCount = 37;
  static constexpr std::uint8_t kTextboxFramesPerGlyph = 6;

  bool load(const FirstQuestData& data);

  [[nodiscard]] bool loaded() const { return loaded_; }
  [[nodiscard]] bool entering() const { return entering_; }
  [[nodiscard]] bool dialogue_acknowledged() const { return dialogue_acknowledged_; }
  [[nodiscard]] std::uint8_t dialogue_visible_character_count() const {
    return dialogue_visible_character_count_;
  }
  [[nodiscard]] std::uint8_t dialogue_frame_delay() const {
    return dialogue_frame_delay_;
  }
  [[nodiscard]] bool start_sword_taken() const { return start_sword_taken_; }
  [[nodiscard]] CaveSourcePosition position() const { return position_; }
  [[nodiscard]] const OwFinalTileMap& final_tiles() const { return final_tiles_; }

  // Models the source's automatic $30-pixel walk from $dd to $ad as one
  // atomic control transition.  Per-frame speed/grid-offset timing is outside
  // this pure component.
  bool settle_entry();
  // Advances source-style text playback by one ordinary play frame. It is a
  // no-op outside the settled, blocking textbox state. The final `$C0` glyph
  // automatically unhalts Link in its own write frame; no A press is
  // required.
  bool tick_dialogue();
  // Validates/restores an in-flight automatically typed textbox. Used only by
  // the session's failure-atomic snapshot restore path.
  bool restore_dialogue_progress(std::uint8_t visible_character_count,
                                 std::uint8_t frame_delay);
  // Retained for legacy/UI compatibility. Gameplay does not require A: this
  // immediately completes the source text record and unhalts Link.
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
  std::uint8_t dialogue_visible_character_count_ = 0;
  std::uint8_t dialogue_frame_delay_ = 0;
  bool start_sword_taken_ = false;
};

}  // namespace minish::foreign_world::zelda1
