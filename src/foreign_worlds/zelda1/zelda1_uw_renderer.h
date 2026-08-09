#pragma once

#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace minish::foreign_world::zelda1 {

// Verified PRG offsets for Z_05's compiled UW tables. Values are locations,
// not Zelda asset bytes; table contents always come from the user-owned PRG.
inline constexpr std::size_t kUwBackgroundPrgOffset = 49435;
inline constexpr std::size_t kUwBackgroundPatternSize = 0x820;
inline constexpr std::size_t kUwRoomLayoutsPrgOffset = 90334;
inline constexpr std::size_t kUwWallTilesPrgOffset = 90016;
inline constexpr std::size_t kUwWallTilesSize = 0x4e;
inline constexpr std::size_t kUwColumnDirectoryPrgOffset = 91908;
inline constexpr std::size_t kUwPrimarySquaresPrgOffset = 91928;
inline constexpr std::size_t kUwPrgBank5Offset = 5 * 16 * 1024;
using UwFinalTileMap = std::array<std::uint8_t, kOwPlayfieldTileWidth * kOwPlayfieldTileHeight>;

inline bool copy_underworld_background_from_verified_prg(const FirstQuestData& data,
                                                         std::vector<std::uint8_t>* out,
                                                         std::string* error = nullptr) {
    if (!out) { if (error) *error = "underworld pattern output is null"; return false; }
    const auto bytes = data.prg().bytes();
    if (kUwBackgroundPrgOffset + kUwBackgroundPatternSize > bytes.size()) {
        if (error) *error = "verified PRG lacks underworld background span";
        return false;
    }
    out->assign(bytes.begin() + kUwBackgroundPrgOffset,
                bytes.begin() + kUwBackgroundPrgOffset + kUwBackgroundPatternSize);
    return true;
}

// Exact static tile order of Z_05: LayOutRoom calls FillTileMap($F6),
// FillWalls, then LayoutUWFloor. Dynamic LayOutDoors, opened-door history,
// item sprites and Aquamentus sprites are intentionally not emulated here.
inline bool build_underworld_static_final_tile_map(const FirstQuestData& data,
                                                   std::uint8_t level_number,
                                                   std::uint8_t room_id,
                                                   UwFinalTileMap* out,
                                                   std::string* error = nullptr) {
    if (!out) { if (error) *error = "underworld tile map output is null"; return false; }
    FirstQuestUnderworldLevelView level{};
    UnderworldRoomView room{};
    if (!data.first_quest_underworld_level(level_number, &level, error) ||
        !data.first_quest_underworld_room(level_number, room_id, &room, error)) return false;
    const auto bytes = data.prg().bytes();
    if (kUwWallTilesPrgOffset + kUwWallTilesSize > bytes.size() ||
        kUwColumnDirectoryPrgOffset + 20 > bytes.size() ||
        kUwPrimarySquaresPrgOffset + 8 > bytes.size() ||
        kUwRoomLayoutsPrgOffset + (room.layout_reference + 1) * 12 > bytes.size()) {
        if (error) *error = "verified PRG lacks a required underworld layout table";
        return false;
    }
    // PlayAreaTiles is column-major, 32 columns by 22 rows.
    out->fill(0xf6);
    const auto set = [&](std::size_t x, std::size_t y, std::uint8_t tile) {
        if (x < 32 && y < 22) (*out)[y * 32 + x] = tile;
    };
    const auto get_column_major = [&](std::size_t offset) -> std::uint8_t {
        return (*out)[(offset % 22) * 32 + offset / 22];
    };
    const auto set_column_major = [&](std::size_t offset, std::uint8_t tile) {
        if (offset < 32 * 22) set(offset / 22, offset % 22, tile);
    };
    // Literal translation of FillWalls' left half and 180-degree copy.
    std::size_t top = 0x17, bottom = 0x2a;
    unsigned pair_count = 10;
    for (std::size_t i = 0; i < kUwWallTilesSize; ++i) {
        const auto tile = bytes[kUwWallTilesPrgOffset + i];
        if (tile == 0) { top += 0x13; bottom += 0x19; continue; }
        set_column_major(top, tile); set_column_major(bottom, tile);
        if (tile != 0xde && tile < 0xe2) set_column_major(bottom, static_cast<std::uint8_t>(tile + 1));
        if (--pair_count != 0) { ++top; --bottom; }
        else { pair_count = 10; top += 0x0d; bottom += 0x1f; }
    }
    for (std::size_t source = 0, destination = 0x2bf; source < 0x160; ++source, --destination) {
        auto tile = get_column_major(source);
        if (tile == 0xdd) tile = 0xdc;
        else if (tile < 0xe0) tile = static_cast<std::uint8_t>(tile + (tile >= 0xdc ? 2 : 1));
        set_column_major(destination, tile);
    }
    const auto layout = bytes.subspan(kUwRoomLayoutsPrgOffset + room.layout_reference * 12, 12);
    for (unsigned column = 0; column < 12; ++column) {
        const auto descriptor = layout[column];
        const auto directory_offset = static_cast<std::size_t>((descriptor >> 4) * 2);
        if (kUwColumnDirectoryPrgOffset + directory_offset + 2 > bytes.size()) return false;
        const std::uint16_t cpu_address = static_cast<std::uint16_t>(bytes[kUwColumnDirectoryPrgOffset + directory_offset]) |
                                          static_cast<std::uint16_t>(bytes[kUwColumnDirectoryPrgOffset + directory_offset + 1] << 8);
        if (cpu_address < 0x8000 || cpu_address >= 0xc000) return false;
        std::size_t heap = kUwPrgBank5Offset + (cpu_address - 0x8000);
        int selected = descriptor & 0x0f;
        while (heap < bytes.size()) {
            const auto value = bytes[heap++];
            if ((value & 0x80) == 0) continue;
            if (selected-- == 0) { --heap; break; }
        }
        if (heap >= bytes.size()) return false;
        unsigned row = 0, repeats = 0;
        while (row < 7 && heap < bytes.size()) {
            const auto square = bytes[heap];
            const auto primary = bytes[kUwPrimarySquaresPrgOffset + (square & 7)];
            const bool type1 = primary >= 0x70 && primary < 0xf3;
            const auto x = 4 + column * 2, y = 4 + row * 2;
            set(x, y, primary); set(x, y + 1, type1 ? static_cast<std::uint8_t>(primary + 1) : primary);
            set(x + 1, y, type1 ? static_cast<std::uint8_t>(primary + 2) : primary);
            set(x + 1, y + 1, type1 ? static_cast<std::uint8_t>(primary + 3) : primary);
            const auto count = static_cast<unsigned>((square >> 4) & 7);
            if (repeats != count) ++repeats;
            else { repeats = 0; ++heap; }
            ++row;
        }
        if (row != 7) return false;
    }
    return true;
}

inline bool render_underworld_room(const FirstQuestData& data, std::uint8_t level_number,
                                   std::uint8_t room_id, std::span<const std::uint8_t> patterns,
                                   OwFramebuffer* out, std::string* error = nullptr) {
    if (!out || patterns.size() != kUwBackgroundPatternSize) return false;
    FirstQuestUnderworldLevelView level{}; UnderworldRoomView room{}; UwFinalTileMap tiles{};
    if (!data.first_quest_underworld_level(level_number, &level, error) ||
        !data.first_quest_underworld_room(level_number, room_id, &room, error) ||
        !build_underworld_static_final_tile_map(data, level_number, room_id, &tiles, error)) return false;
    if (level.info.palettes_transfer_buffer.size() < 35 || level.info.palettes_transfer_buffer[0] != 0x3f ||
        level.info.palettes_transfer_buffer[1] != 0) return false;
    std::array<std::uint8_t, 32> palette{};
    for (unsigned i = 0; i < 32; ++i) palette[i] = level.info.palettes_transfer_buffer[3 + i] & 63;
    const auto bytes = data.prg().bytes();
    if (34687 + 0x700 > bytes.size() || 36479 + 0xe0 > bytes.size()) return false;
    out->fill(nes_to_bgr555(palette[0]));
    for (unsigned ty = 0; ty < 22; ++ty) for (unsigned tx = 0; tx < 32; ++tx) {
        const auto tile = tiles[ty * 32 + tx];
        const unsigned selector = tx >= 4 && tx < 28 && ty >= 4 && ty < 18
            ? room.attributes.attr_b & 3 : room.attributes.attr_a & 3;
        std::span<const std::uint8_t> bank; unsigned offset = 0;
        if (tile < 0x70) { bank = bytes.subspan(34687, 0x700); offset = tile * 16; }
        else if (tile < 0xf2) { bank = patterns; offset = (tile - 0x70) * 16; }
        else { bank = bytes.subspan(36479, 0xe0); offset = (tile - 0xf2) * 16; }
        if (offset + 15 >= bank.size()) return false;
        for (unsigned py = 0; py < 8; ++py) for (unsigned px = 0; px < 8; ++px) {
            const auto colour = ((bank[offset + py] >> (7 - px)) & 1) |
                                (((bank[offset + 8 + py] >> (7 - px)) & 1) << 1);
            const int x = static_cast<int>(tx * 8 + px) - 8;
            const int y = static_cast<int>(ty * 8 + py) - 8;
            if (x >= 0 && x < 240 && y >= 0 && y < 160)
                (*out)[y * 240 + x] = nes_to_bgr555(palette[selector * 4 + colour]);
        }
    }
    return true;
}

}  // namespace minish::foreign_world::zelda1
