#pragma once

#include "foreign_worlds/zelda1/zelda1_cave_control.h"
#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
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
// Pinned `src/bins.xml` maps PersonText.dat to this PRG offset. The source
// `PersonTextAddrs.inc` selector-zero entry is PersonText+0 and selector one
// begins at +43, which bounds the First Quest starting-sword message exactly.
inline constexpr std::size_t kFirstQuestPersonTextPrgOffset = 16460;
inline constexpr std::size_t kFirstQuestStartCaveDialogueByteCount = 43;
inline constexpr std::array<std::uint8_t, 3> kTextboxLineAddrsLo{
    {0xc4, 0xe4, 0xa4}};

struct FirstQuestStartCaveRenderState {
  bool sword_acquired = false;
  // `UpdatePersonState_Textbox` leaves completed characters in the nametable
  // after its final `$C0` marker invokes UnhaltLink.
  bool dialogue_complete = false;
  // Number of non-$25 source character transfers currently visible. This is
  // ignored once the complete nametable result is present.
  std::uint8_t visible_dialogue_characters = 0;
  // Z_04:UpdateStandingFire forces ObjDir=up, then Z_07 uses the toggled
  // ObjAnimFrame as the horizontal-flip bit.  It is therefore a two-phase
  // (normal/mirrored) composition, not a guest-authored fire sprite.
  std::uint8_t standing_fire_animation_frame = 0;
};

struct FirstQuestStartCaveDialogueFacts {
  std::array<std::uint8_t, kFirstQuestStartCaveDialogueByteCount> encoded{};
  // Z_01:TextboxLineAddrsLo is a three-line ring. Selector zero starts at
  // table+2 (`$A4`); its `$98` marker selects `$C4`; final `$EC` selects `$A4`
  // again and unhalts Link. `$E4` is source-supported but unused here.
  std::array<std::uint8_t, 3> line_starts = kTextboxLineAddrsLo;
  std::array<std::uint8_t, 2> used_line_starts{{0xa4, 0xc4}};
  std::uint8_t visible_glyph_count = 0;
  std::uint8_t line_transition_count = 0;
  std::uint8_t final_marker = 0;
};

struct FirstQuestStartCaveFacts {
  // `InitModeB_Sub5` invokes `FillPlayAreaAttrs` for room $44. Its outer
  // AttrsA selector fills all play-area attribute bytes, then AttrsB replaces
  // the inner bytes. `$21A4`/`$21C4` map to PPU attribute byte `$23D9`;
  // InitModeB transfers PlayAreaAttrs[$09] there, and `$09` is inner. The
  // textbox therefore uses AttrsB rather than the cave backdrop's AttrsA
  // selector. The verified First Quest record is selector 2 and the
  // CaveBgPaletteRowsTransferBuf row is {$0f,$30,$00,$12}.
  std::uint8_t textbox_background_palette_selector = 2;
  std::array<std::uint8_t, 4> textbox_background_palette{{0x0f, 0x30, 0x00,
                                                           0x12}};
  // Z_01.asm:InitCave puts the cave person at ($78,$80). SetupTileObjectOW
  // maps OW77's normal-cave slot zero to object type $6A. ObjAnimations[$6b]
  // -> ObjAnimFrameHeap[$58] selects OAM tile $98. PPUCTRL has 8x16 sprites,
  // hence every OAM side uses the consecutive CHR tiles $98/$99; the second
  // OAM side repeats that pair with attribute $42 (horizontal flip).
  std::uint8_t old_man_object_type = 0x6a;
  std::uint8_t old_man_source_x = 0x78;
  std::uint8_t old_man_source_y = 0x80;
  bool old_man_visual_is_opaque = false;
  bool old_man_visible_before_sword = true;
  // Z_01.asm:SetUpCommonCaveObjects creates type $40 fires before the item
  // persistence check. Z_04:UpdateStandingFire draws descriptor frame zero
  // through object animation $41: tiles $5c/$5e, sprite palette row 2, at
  // these positions. Its six-frame ObjAnimFrame toggle mirrors the pair.
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

// Decodes only the verified First Quest selector-zero PersonText record. It
// deliberately returns encoded glyph bytes rather than authored host prose:
// the renderer uses the same low-six-bit tile codes and high-bit line control
// that Z_01:UpdatePersonState_Textbox writes to the PPU transfer buffer.
inline bool get_first_quest_start_cave_dialogue(
    const FirstQuestData& data, FirstQuestStartCaveDialogueFacts* out) {
  if (!out) return false;
  const auto prg = data.prg().bytes();
  if (kFirstQuestPersonTextPrgOffset + kFirstQuestStartCaveDialogueByteCount >
      prg.size())
    return false;
  FirstQuestStartCaveDialogueFacts candidate{};
  std::copy_n(prg.begin() + kFirstQuestPersonTextPrgOffset,
              candidate.encoded.size(), candidate.encoded.begin());
  for (const std::uint8_t encoded : candidate.encoded) {
    if ((encoded & 0x3f) != 0x25) ++candidate.visible_glyph_count;
    const std::uint8_t marker = encoded & 0xc0;
    if (marker == 0) continue;
    if (marker == 0xc0) {
      // A final marker may occur only once and must terminate the record.
      if (candidate.final_marker != 0) return false;
      candidate.final_marker = marker;
    } else {
      ++candidate.line_transition_count;
    }
  }
  // Selector zero has exactly one `$80` first-to-second-row transition and
  // final `$C0` marker. Checking these decoded facts makes the fixed source
  // range fail closed if a non-First-Quest PRG reached this renderer.
  if (candidate.visible_glyph_count !=
          Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount ||
      candidate.line_transition_count != 1 || candidate.final_marker != 0xc0 ||
      candidate.encoded[21] != 0x98 || candidate.encoded.back() != 0xec)
    return false;
  *out = candidate;
  return true;
}

namespace detail {

inline bool draw_cave_sprite_8x16(OwFramebuffer *out,
                                  std::span<const std::uint8_t> top_pattern,
                                  std::span<const std::uint8_t> bottom_pattern,
                                  CaveSourcePosition source_position,
                                  bool horizontal_flip,
                                  const std::array<std::uint8_t, 4> &palette) {
  // Z_07 reset sets PPUCTRL=$30; bit $20 selects the NES 8x16 OAM mode.
  // The OAM tile's even index is the upper pattern and its next pattern is
  // lower. All three cave sprites below use even source tile numbers.
  if (!out || top_pattern.size() != 16 || bottom_pattern.size() != 16) return false;
  const int start_x = static_cast<int>(source_position.x) - 8;
  const int start_y = static_cast<int>(source_position.y) - 72;
  for (unsigned py = 0; py < 16; ++py)
    for (unsigned px = 0; px < 8; ++px) {
      const auto pattern = py < 8 ? top_pattern : bottom_pattern;
      const unsigned pattern_y = py & 7;
      const unsigned pattern_x = horizontal_flip ? 7 - px : px;
      const auto color = ((pattern[pattern_y] >> (7 - pattern_x)) & 1) |
                         (((pattern[8 + pattern_y] >> (7 - pattern_x)) & 1) << 1);
      if (color == 0) continue;
      const int x = start_x + static_cast<int>(px);
      const int y = start_y + static_cast<int>(py);
      if (x >= 0 && x < 240 && y >= 0 && y < 160)
        (*out)[y * 240 + x] = nes_to_bgr555(palette[color] & 63);
    }
  return true;
}

inline void draw_cave_text_tile(OwFramebuffer *out,
                                std::span<const std::uint8_t> patterns,
                                std::uint8_t tile, std::uint8_t vram_low,
                                const std::array<std::uint8_t, 32>& palette,
                                unsigned textbox_palette) {
  if (!out || tile >= 0x70 || patterns.size() < (static_cast<std::size_t>(tile) + 1) * 16)
    return;
  // `$21A4` is nametable row 13, column four. The foreign framebuffer crops
  // source Y=$72, so `$A4`/`$C4`/`$E4` land at output Y=32/40/48. The source
  // only mutates the low byte, retaining the fixed `$21` high byte.
  const int start_x = static_cast<int>((vram_low & 0x1f) * 8) - 8;
  const int start_y = static_cast<int>((vram_low >> 5) * 8) - 8;
  const auto glyph = patterns.subspan(static_cast<std::size_t>(tile) * 16, 16);
  for (unsigned py = 0; py < 8; ++py)
    for (unsigned px = 0; px < 8; ++px) {
      const auto color = ((glyph[py] >> (7 - px)) & 1) |
                         (((glyph[8 + py] >> (7 - px)) & 1) << 1);
      const int x = start_x + static_cast<int>(px);
      const int y = start_y + static_cast<int>(py);
      if (x < 0 || x >= 240 || y < 0 || y >= 160) continue;
      // NES background color zero is universal. `$21A4` maps to `$23D9`,
      // which InitModeB fills from inner PlayAreaAttrs[$09], so nonzero
      // textbox colors use room $44's AttrsB-selected row.
      const auto nes_color = palette[color == 0 ? 0 : textbox_palette * 4 + color];
      (*out)[y * 240 + x] = nes_to_bgr555(nes_color & 63);
    }
}

}  // namespace detail

inline bool
render_first_quest_start_cave(const FirstQuestData &data,
                              std::span<const std::uint8_t> overworld_patterns,
                              FirstQuestStartCaveRenderState state,
                              OwFramebuffer *out,
                              FirstQuestStartCaveFacts *facts = nullptr) {
  if (!out || overworld_patterns.size() != kOverworldBackgroundPatternSize ||
      state.standing_fire_animation_frame > 1)
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
  // The transfer writes palette entries $08..$0f. `FillPlayAreaAttrs` uses
  // room $44's AttrsA low bits for the outer backdrop and AttrsB low bits
  // for inner PlayAreaAttrs[$09], which InitModeB transfers to the `$23D9`
  // attribute byte covering textbox cells `$21A4`/`$21C4`.
  for (unsigned i = 0; i < 8; ++i)
    pal[8 + i] = prg[kCaveBgPaletteTransferPrgOffset + 3 + i] & 63;
  const unsigned playfield_palette = cave_palette_room.attributes.attr_a & 3;
  const unsigned textbox_palette = cave_palette_room.attributes.attr_b & 3;
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
      (!detail::draw_cave_sprite_8x16(out, old_man.top_pattern,
                                      old_man.bottom_pattern, old_man.left, false,
                                      old_man.palette) ||
       !detail::draw_cave_sprite_8x16(out, old_man.top_pattern,
                                      old_man.bottom_pattern, old_man.right, true,
                                      old_man.palette)))
    return false;
  // Z_04:UpdateStandingFire uses descriptor frame zero, then
  // Z_07:AnimateObjectWalking toggles ObjAnimFrame every six updates.
  // Because the fire's forced direction is up, SetUpWalkingSprites uses that
  // bit as OAM horizontal flip. Anim_WriteHorizontallyFlippableSpritePair
  // swaps the left/right 8x16 descriptors when flipped: $5e/$5f comes first
  // and $5c/$5d second. Palette row two comes from the standing-fire's
  // `Anim_SetSpriteDescriptorAttributes #$02`, shared with the Old Man.
  const auto fire_left = prg.subspan(32895 + 0x5c * 16, 32);
  const auto fire_right = prg.subspan(32895 + 0x5e * 16, 32);
  const bool fire_flipped = state.standing_fire_animation_frame != 0;
  for (const CaveSourcePosition fire : FirstQuestStartCaveFacts{}.fire_source_positions) {
    const auto first = fire_flipped ? fire_right : fire_left;
    const auto second = fire_flipped ? fire_left : fire_right;
    if (!detail::draw_cave_sprite_8x16(out, first.first(16), first.subspan(16, 16),
                                        fire, fire_flipped, old_man.palette) ||
        !detail::draw_cave_sprite_8x16(out, second.first(16), second.subspan(16, 16),
                                        {static_cast<std::uint8_t>(fire.x + 8), fire.y},
                                        fire_flipped, old_man.palette))
      return false;
  }
  // This narrow 8x16 OAM sprite is source-backed end-to-end: $01 -> slot 0
  // -> frame tile $20/$21, then Anim_WriteSpecificItemSprites centers it
  // +4px. ItemSlotToPaletteOffsetsOrValues[$00]=$ff plus sword grade $01
  // yields sprite palette row zero. Color zero remains transparent for OAM.
  if (!state.sword_acquired && (items.raw_item_ids[1] & 0x3f) == 0x01) {
    constexpr int kSpriteX = 0x78 + 4 - 8, kSpriteY = 0x98 - 72;
    const auto sprite = prg.subspan(32895 + 0x20 * 16, 32);
    const std::array<std::uint8_t, 4> sword_palette{{pal[16], pal[17], pal[18], pal[19]}};
    if (!detail::draw_cave_sprite_8x16(
            out, sprite.first(16), sprite.subspan(16, 16),
            {static_cast<std::uint8_t>(kSpriteX + 8),
             static_cast<std::uint8_t>(kSpriteY + 72)},
            false, sword_palette))
      return false;
  }
  // Z_01:InitCave returns through UnhaltLink as soon as its persistent room
  // item bit is set.  It does not initialize the person/text record, so a
  // re-entry after the wooden sword has no retained or replayed message.
  if (!state.sword_acquired) {
    FirstQuestStartCaveDialogueFacts dialogue{};
    if (!get_first_quest_start_cave_dialogue(data, &dialogue)) return false;
    const unsigned wanted_glyphs = state.dialogue_complete
        ? dialogue.visible_glyph_count
        : std::min<unsigned>(state.visible_dialogue_characters,
                             dialogue.visible_glyph_count);
  // Render the exact source transfer sequence. `$25` advances the VRAM cursor
  // but is immediately skipped by UpdatePersonState_Textbox; ordinary `$24`
  // is a timed, written background tile. A high-bit marker takes effect only
  // after its glyph has been transferred.
    std::uint8_t vram_low = 0xa4;
    unsigned rendered_glyphs = 0;
    const auto text_patterns = prg.subspan(34687, 0x700);
    for (const std::uint8_t encoded : dialogue.encoded) {
      const std::uint8_t glyph = encoded & 0x3f;
      const std::uint8_t destination = vram_low++;
      if (glyph == 0x25) continue;
      if (rendered_glyphs >= wanted_glyphs) break;
      detail::draw_cave_text_tile(out, text_patterns, glyph, destination, pal,
                                  textbox_palette);
      ++rendered_glyphs;
      const std::uint8_t marker = encoded & 0xc0;
      if (marker == 0) continue;
      const unsigned line_index = marker == 0xc0 ? 2 : marker == 0x40 ? 1 : 0;
      vram_low = dialogue.line_starts[line_index];
      if (marker == 0xc0) break;
    }
  }
  if (facts) {
    FirstQuestStartCaveFacts candidate{};
    candidate.textbox_background_palette_selector =
        static_cast<std::uint8_t>(textbox_palette);
    std::copy_n(pal.begin() + textbox_palette * 4,
                candidate.textbox_background_palette.size(),
                candidate.textbox_background_palette.begin());
    *facts = candidate;
  }
  return true;
}

} // namespace minish::foreign_world::zelda1
