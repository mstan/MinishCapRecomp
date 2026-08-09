#pragma once

#include "foreign_worlds/zelda1/zelda1_cave_control.h"
#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"

#include <array>
#include <cstdint>
#include <span>

namespace minish::foreign_world::zelda1 {

// Z_06.asm:CaveBgPaletteRowsTransferBuf.  This is a verified-PRG load-file
// offset (not a CPU address), established from the pinned source label.  It
// is used only after checking the entire transfer record, so a foreign PRG
// cannot silently be interpreted as Zelda's cave palette.
inline constexpr std::size_t kCaveBgPaletteTransferPrgOffset = 107101;
inline constexpr std::array<std::uint8_t, 3> kCaveBgPaletteHeader{
    {0x3f, 0x08, 0x08}};

struct FirstQuestStartCaveRenderState {
  bool sword_acquired = false;
  // Kept as an explicit source-control input.  InitCave has already made the
  // person and fires visible while the first textbox is still blocking Link,
  // so it intentionally has no visual effect here.
  bool intro_complete = false;
};

struct FirstQuestStartCaveFacts {
  // Z_01.asm:InitCave puts the cave person at ($78,$80). SetupTileObjectOW
  // maps OW77's normal-cave slot zero to object type $6A. ObjAnimations[$6b]
  // -> ObjAnimFrameHeap[$58] selects tile $98; the second tile is the same
  // tile with attribute $42 (horizontal flip).
  std::uint8_t old_man_object_type = 0x6a;
  std::uint8_t old_man_source_x = 0x78;
  std::uint8_t old_man_source_y = 0x80;
  bool old_man_visual_is_opaque = false;
  bool old_man_visible_before_sword = true;
  // Z_01.asm:SetUpCommonCaveObjects creates type $40 fires before the item
  // persistence check. UpdateFire draws frame zero through object animation
  // descriptor $41: tiles $5c/$5e, sprite palette row 2, at these positions.
  // The animation cadence beyond this source-proven initial frame is outside
  // this static renderer.
  std::array<CaveSourcePosition, 2> fire_source_positions{{{0x48, 0x80},
                                                             {0xa8, 0x80}}};
  std::uint8_t fire_left_tile = 0x5c;
  std::uint8_t fire_right_tile = 0x5e;
  std::uint8_t fire_attribute = 0x02;
  bool fire_static_frame_zero = true;
  bool fire_visible_before_and_after_sword = true;
  // Z_01.asm:DrawCaveItems puts cave item slot 1 at ($78,$98); item $01 is
  // the wooden sword and Anim_ItemFrameTiles slot zero selects CHR tile $20.
  std::uint8_t sword_item_id = 0x01;
  std::uint8_t sword_source_x = 0x78;
  std::uint8_t sword_source_y = 0x98;
};

namespace detail {

inline bool draw_cave_sprite_tile(OwFramebuffer *out,
                                  std::span<const std::uint8_t> pattern,
                                  CaveSourcePosition source_position,
                                  bool horizontal_flip,
                                  const std::array<std::uint8_t, 4> &palette) {
  if (pattern.size() != 16) return false;
  const int start_x = static_cast<int>(source_position.x) - 8;
  const int start_y = static_cast<int>(source_position.y) - 72;
  for (unsigned py = 0; py < 8; ++py)
    for (unsigned px = 0; px < 8; ++px) {
      const unsigned pattern_x = horizontal_flip ? 7 - px : px;
      const auto color = ((pattern[py] >> (7 - pattern_x)) & 1) |
                         (((pattern[8 + py] >> (7 - pattern_x)) & 1) << 1);
      if (color == 0) continue;
      const int x = start_x + static_cast<int>(px);
      const int y = start_y + static_cast<int>(py);
      if (x >= 0 && x < 240 && y >= 0 && y < 160)
        (*out)[y * 240 + x] = nes_to_bgr555(palette[color] & 63);
    }
  return true;
}

}  // namespace detail

inline bool
render_first_quest_start_cave(const FirstQuestData &data,
                              std::span<const std::uint8_t> overworld_patterns,
                              FirstQuestStartCaveRenderState state,
                              OwFramebuffer *out,
                              FirstQuestStartCaveFacts *facts = nullptr) {
  if (!out || overworld_patterns.size() != kOverworldBackgroundPatternSize)
    return false;
  LevelInfoView info{};
  OverworldRoomView cave_palette_room{};
  OverworldRoomView start_entrance_room{};
  OverworldCaveItemsView items{};
  OwFinalTileMap tiles{};
  OverworldSquareGrid grid{};
  if (!data.overworld_level_info(&info) ||
      !data.overworld_room(4, 4, &cave_palette_room) ||
      !data.overworld_room(7, 7, &start_entrance_room) ||
      !data.overworld_cave_items(start_entrance_room, &items) ||
      !data.overworld_normal_cave_square_grid(&grid) ||
      !build_overworld_final_tile_map_from_square_grid(data, grid, &tiles) ||
      info.palettes_transfer_buffer.size() < 35 ||
      info.palettes_transfer_buffer[0] != 0x3f ||
      info.palettes_transfer_buffer[1] != 0)
    return false;
  const auto prg = data.prg().bytes();
  if (kCaveBgPaletteTransferPrgOffset + 12 > prg.size() ||
      !std::equal(kCaveBgPaletteHeader.begin(), kCaveBgPaletteHeader.end(),
                  prg.begin() + kCaveBgPaletteTransferPrgOffset) ||
      prg[kCaveBgPaletteTransferPrgOffset + 2] != 8 ||
      34687 + 0x700 > prg.size() || 36479 + 0xe0 > prg.size() ||
      32895 + 0x700 > prg.size())
    return false;
  std::array<std::uint8_t, 32> pal{};
  for (unsigned i = 0; i < 32; ++i)
    pal[i] = info.palettes_transfer_buffer[3 + i] & 63;
  // The transfer writes palette entries $08..$0f. FillPlayAreaAttrs in
  // InitModeB_Sub5 uses room $44's AttrsA low bits for the play area.
  for (unsigned i = 0; i < 8; ++i)
    pal[8 + i] = prg[kCaveBgPaletteTransferPrgOffset + 3 + i] & 63;
  const unsigned playfield_palette = cave_palette_room.attributes.attr_a & 3;
  out->fill(nes_to_bgr555(pal[0]));
  for (unsigned ty = 0; ty < kOwPlayfieldTileHeight; ++ty)
    for (unsigned tx = 0; tx < kOwPlayfieldTileWidth; ++tx) {
      const auto tile = tiles[ty * kOwPlayfieldTileWidth + tx];
      std::span<const std::uint8_t> bank;
      unsigned offset = 0;
      if (tile < 0x70) {
        bank = prg.subspan(34687, 0x700);
        offset = tile * 16;
      } else if (tile < 0xf2) {
        bank = overworld_patterns;
        offset = (tile - 0x70) * 16;
      } else {
        bank = prg.subspan(36479, 0xe0);
        offset = (tile - 0xf2) * 16;
      }
      if (offset + 16 > bank.size())
        return false;
      for (unsigned py = 0; py < 8; ++py)
        for (unsigned px = 0; px < 8; ++px) {
          const auto color = ((bank[offset + py] >> (7 - px)) & 1) |
                             (((bank[offset + 8 + py] >> (7 - px)) & 1) << 1);
          const int x = static_cast<int>(tx * 8 + px) - 8,
                    y = static_cast<int>(ty * 8 + py) - 8;
          if (x >= 0 && x < 240 && y >= 0 && y < 160)
            (*out)[y * 240 + x] =
                nes_to_bgr555(pal[playfield_palette * 4 + color]);
        }
    }
  // InitCave installs the person and calls SetUpCommonCaveObjects before it
  // enters the initial dialogue. The person is instead absent after the
  // sword's persistent cave-item bit is set; fires remain in both cases.
  StartCaveOldManSpriteFacts old_man{};
  if (!get_first_quest_start_cave_old_man_sprite(data, &old_man)) return false;
  if (!state.sword_acquired &&
      (!detail::draw_cave_sprite_tile(out, old_man.pattern, old_man.left, false,
                                      old_man.palette) ||
       !detail::draw_cave_sprite_tile(out, old_man.pattern, old_man.right, true,
                                      old_man.palette)))
    return false;
  // UpdateFire's initial descriptor frame is a non-flipped pair at ObjX and
  // ObjX+8. It shares sprite palette row 2 with the Old Man. Rendering this
  // one verified frame avoids inventing a host-side fire animation.
  const auto fire_left = prg.subspan(32895 + 0x5c * 16, 16);
  const auto fire_right = prg.subspan(32895 + 0x5e * 16, 16);
  for (const CaveSourcePosition fire : FirstQuestStartCaveFacts{}.fire_source_positions) {
    if (!detail::draw_cave_sprite_tile(out, fire_left, fire, false, old_man.palette) ||
        !detail::draw_cave_sprite_tile(
            out, fire_right,
            {static_cast<std::uint8_t>(fire.x + 8), fire.y}, false,
            old_man.palette))
      return false;
  }
  // This single narrow sprite is source-backed end-to-end: $01 -> slot 0
  // -> frame tile $20, then Anim_WriteSpecificItemSprites centers it +4px.
  // ItemSlotToPaletteOffsetsOrValues[$00]=$ff plus sword grade $01 yields
  // sprite palette row zero. Color zero remains transparent for OAM.
  if (!state.sword_acquired && (items.raw_item_ids[1] & 0x3f) == 0x01) {
    constexpr int kSpriteX = 0x78 + 4 - 8, kSpriteY = 0x98 - 72;
    const auto sprite = prg.subspan(32895 + 0x20 * 16, 16);
    for (unsigned py = 0; py < 8; ++py)
      for (unsigned px = 0; px < 8; ++px) {
        const auto color = ((sprite[py] >> (7 - px)) & 1) |
                           (((sprite[8 + py] >> (7 - px)) & 1) << 1);
        if (color != 0)
          (*out)[(kSpriteY + static_cast<int>(py)) * 240 + kSpriteX +
                 static_cast<int>(px)] = nes_to_bgr555(pal[16 + color]);
      }
  }
  if (facts)
    *facts = {};
  return true;
}

} // namespace minish::foreign_world::zelda1
