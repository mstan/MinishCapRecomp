#pragma once

#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace minish::foreign_world::zelda1 {
// Canonical raw NES palette from nesrecomp/runner/src/ppu_renderer.c. Values
// are code data, not Zelda assets. GBA's native BGR555 framebuffer stores
// red in bits 0..4, green in 5..9, and blue in 10..14 (the numeric order is
// therefore opposite an RGB888 word's visual byte order).
inline constexpr std::array<std::uint32_t, 64> kNesrecompRawPalette{
    {0xFF545454, 0xFF001E74, 0xFF081090, 0xFF300088, 0xFF440064, 0xFF5C0030,
     0xFF540400, 0xFF3C1800, 0xFF202A00, 0xFF083A00, 0xFF004000, 0xFF003C00,
     0xFF00323C, 0,          0,          0,          0xFF989698, 0xFF084CC4,
     0xFF3032EC, 0xFF5C1EE4, 0xFF8814B0, 0xFFA01464, 0xFF982220, 0xFF783C00,
     0xFF545A00, 0xFF287200, 0xFF087C00, 0xFF007628, 0xFF006678, 0,
     0,          0,          0xFFECEEEC, 0xFF4C9AEC, 0xFF787CEC, 0xFFB062EC,
     0xFFE454EC, 0xFFEC58B4, 0xFFEC6A64, 0xFFD48820, 0xFFA0AA00, 0xFF74C400,
     0xFF4CD020, 0xFF38CC6C, 0xFF38B4CC, 0xFF3C3C3C, 0,          0,
     0xFFECEEEC, 0xFFA8CCEC, 0xFFBCBCEC, 0xFFD4B2EC, 0xFFECAEEC, 0xFFECAED4,
     0xFFECB4B0, 0xFFE4C490, 0xFFCCD278, 0xFFB4DE78, 0xFFA8E290, 0xFF98E2B4,
     0xFFA0D6E4, 0xFFA0A2A0, 0,          0}};
inline constexpr std::size_t kOwRenderWidth = 240, kOwRenderHeight = 160;
using OwFramebuffer =
    std::array<std::uint16_t, kOwRenderWidth * kOwRenderHeight>;
// LayoutRoomOW writes a 32 x 22 final-tile playfield (two 8px tiles per
// source square).  Keep this public because collision must consume exactly
// this post-CheckTileObject, pre-CHR plane rather than re-expand descriptors.
inline constexpr std::size_t kOwPlayfieldTileWidth = 32,
                             kOwPlayfieldTileHeight = 22;
using OwFinalTileMap =
    std::array<std::uint8_t, kOwPlayfieldTileWidth * kOwPlayfieldTileHeight>;
inline constexpr std::size_t kOverworldBackgroundPrgOffset = 51515;
inline constexpr std::size_t kOverworldBackgroundPatternSize = 0x820;
inline constexpr std::uint16_t nes_to_bgr555(std::uint8_t n) {
  const auto c = kNesrecompRawPalette[n & 63];
  return static_cast<std::uint16_t>(((c >> 19) & 31) | (((c >> 11) & 31) << 5) |
                                    (((c >> 3) & 31) << 10));
}
// The live loader deliberately derives this transfer block from its owned,
// already hash-validated PRG.  Unlike the importer cache check below, this has
// no cache-directory dependency and cannot accidentally mix two ROM sources.
inline bool
copy_overworld_background_from_verified_prg(const FirstQuestData &data,
                                            std::vector<std::uint8_t> *output,
                                            std::string *error = nullptr) {
  if (!output) {
    if (error)
      *error = "pattern output is null";
    return false;
  }
  const auto prg = data.prg().bytes();
  if (kOverworldBackgroundPrgOffset + kOverworldBackgroundPatternSize >
      prg.size()) {
    if (error)
      *error = "verified PRG lacks overworld pattern span";
    return false;
  }
  output->assign(prg.begin() + kOverworldBackgroundPrgOffset,
                 prg.begin() + kOverworldBackgroundPrgOffset +
                     kOverworldBackgroundPatternSize);
  return true;
}
// The importer manifest names this file patterns/overworld_background and
// records this exact PRG slice and SHA-256. We fail closed unless the ignored
// cache file is byte-identical to the supplied verified PRG slice.
inline bool load_verified_overworld_background(
    const FirstQuestData &data, const std::filesystem::path &cache_root,
    std::vector<std::uint8_t> *output, std::string *error = nullptr) {
  if (!output) {
    if (error)
      *error = "pattern output is null";
    return false;
  }
  const auto prg = data.prg().bytes();
  if (kOverworldBackgroundPrgOffset + kOverworldBackgroundPatternSize >
      prg.size()) {
    if (error)
      *error = "verified PRG lacks overworld pattern span";
    return false;
  }
  std::ifstream f(cache_root / "patterns" / "overworld_background.bin",
                  std::ios::binary);
  if (!f) {
    if (error)
      *error = "missing named cached overworld pattern span";
    return false;
  }
  std::vector<std::uint8_t> v((std::istreambuf_iterator<char>(f)), {});
  if (v.size() != kOverworldBackgroundPatternSize ||
      !std::equal(v.begin(), v.end(),
                  prg.begin() + kOverworldBackgroundPrgOffset)) {
    if (error)
      *error = "cached overworld pattern span does not match verified PRG";
    return false;
  }
  *output = std::move(v);
  return true;
}
// Z_05.asm:LayoutRoomOW calls CheckTileObject then WriteSquareOW.  The latter
// is the canonical final-tile expansion used by both pixels and collision.
// This is the initial/static room state: world-flag secret replacement and
// runtime tile changes (bombs, ladder, pond cycle) deliberately remain out of
// scope rather than being guessed from immutable PRG data.
inline std::uint8_t resolve_static_overworld_primary(std::uint8_t primary) {
  if (primary >= 0xe5 && primary <= 0xea)
    return std::array<std::uint8_t, 6>{
        0xc8, 0xd8, 0xc4, 0xbc, 0xc0, 0xc0}[primary - 0xe5];
  return primary;
}
inline bool
build_overworld_final_tile_map_from_square_grid(const FirstQuestData &data,
                                                const OverworldSquareGrid &grid,
                                                OwFinalTileMap *out) {
  if (!out)
    return false;
  const auto prg = data.prg().bytes();
  if (92596 + 64 > prg.size())
    return false;
  for (unsigned sy = 0; sy < 11; ++sy)
    for (unsigned sx = 0; sx < 16; ++sx) {
      const auto &square = grid.squares[sy * 16 + sx];
      const auto primary =
          resolve_static_overworld_primary(square.primary_square);
      for (unsigned q = 0; q < 4; ++q) {
        const auto tile =
            (square.raw_descriptor & 0x3f) < 0x10
                ? prg[92596 + (square.raw_descriptor & 0x3f) * 4 + q]
                : static_cast<std::uint8_t>(primary + q);
        const unsigned tx = sx * 2 + (q >= 2), ty = sy * 2 + (q & 1);
        (*out)[ty * kOwPlayfieldTileWidth + tx] = tile;
      }
    }
  return true;
}
inline bool build_overworld_final_tile_map(const FirstQuestData &data,
                                           std::uint8_t room_id,
                                           OwFinalTileMap *out) {
  OverworldRoomView room{};
  OverworldSquareGrid grid{};
  return data.overworld_room(room_id & 15, room_id >> 4, &room) &&
         data.overworld_square_grid(room, &grid) &&
         build_overworld_final_tile_map_from_square_grid(data, grid, out);
}
inline bool
build_overworld_normal_cave_final_tile_map(const FirstQuestData &data,
                                           OwFinalTileMap *out) {
  OverworldSquareGrid grid{};
  return data.overworld_normal_cave_square_grid(&grid) &&
         build_overworld_final_tile_map_from_square_grid(data, grid, out);
}
// `patterns` must be the manifest-verified patterns/overworld_background
// stream ($820 bytes), copied by Z_03 to PPU $1700.  Palette transfer records
// are supplied verbatim from LevelInfo; unsupported dynamic substitutions fail.
inline bool render_overworld_room(const FirstQuestData &data,
                                  std::uint8_t room_id,
                                  std::span<const std::uint8_t> patterns,
                                  OwFramebuffer *out) {
  if (!out || patterns.size() != kOverworldBackgroundPatternSize)
    return false;
  OverworldRoomView room{};
  LevelInfoView info{};
  OwFinalTileMap tiles{};
  if (!data.overworld_room(room_id & 15, room_id >> 4, &room) ||
      !data.overworld_level_info(&info) ||
      !build_overworld_final_tile_map(data, room_id, &tiles))
    return false;
  if (info.palettes_transfer_buffer.size() < 35 ||
      info.palettes_transfer_buffer[0] != 0x3f ||
      info.palettes_transfer_buffer[1] != 0)
    return false;
  std::array<std::uint8_t, 32> pal{};
  for (unsigned i = 0; i < 32; ++i)
    pal[i] = info.palettes_transfer_buffer[3 + i] & 63;
  out->fill(nes_to_bgr555(pal[0]));
  const auto prg = data.prg().bytes();
  if (34687 + 0x700 > prg.size() || 36479 + 0xe0 > prg.size() ||
      92596 + 64 > prg.size())
    return false;
  // GBA viewport policy: center-crop the 256x176 source playfield at (8,8),
  // yielding 240x160 terrain only. The NES HUD is intentionally excluded.
  for (unsigned ty = 0; ty < kOwPlayfieldTileHeight; ++ty)
    for (unsigned tx = 0; tx < kOwPlayfieldTileWidth; ++tx) {
      const auto tile = tiles[ty * kOwPlayfieldTileWidth + tx];
      unsigned ps = (tx >= 4 && tx < 28 && ty >= 4 && ty < 20)
                        ? (room.attributes.attr_b & 3)
                        : (room.attributes.attr_a & 3);
      std::span<const std::uint8_t> bank;
      unsigned off = 0;
      if (tile < 0x70) {
        bank = prg.subspan(34687, 0x700);
        off = tile * 16;
      } else if (tile < 0xf2) {
        bank = patterns;
        off = (tile - 0x70) * 16;
      } else {
        bank = prg.subspan(36479, 0xe0);
        off = (tile - 0xf2) * 16;
      }
      if (off + 15 >= bank.size())
        continue;
      for (unsigned py = 0; py < 8; ++py)
        for (unsigned px = 0; px < 8; ++px) {
          auto b = ((bank[off + py] >> (7 - px)) & 1) |
                   (((bank[off + 8 + py] >> (7 - px)) & 1) << 1);
          int x = static_cast<int>(tx * 8 + px) - 8,
              y = static_cast<int>(ty * 8 + py) - 8;
          if (x >= 0 && x < 240 && y >= 0 && y < 160)
            (*out)[y * 240 + x] = nes_to_bgr555(pal[ps * 4 + b]);
        }
    }
  return true;
}
} // namespace minish::foreign_world::zelda1
