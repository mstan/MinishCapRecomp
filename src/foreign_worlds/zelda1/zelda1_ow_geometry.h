#pragma once

#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"

#include <array>
#include <cstdint>

namespace minish::foreign_world::zelda1 {

// Z_05.asm:ObjectRoomBoundsOW, installed by SetupObjRoomBounds, makes $89 the
// first normally-unwalkable OW final tile.  Z_07.asm:GetCollidableTile also
// normalizes precisely these nine tiles to $26 before the comparison.
inline constexpr std::uint8_t kOwFirstUnwalkableFinalTile = 0x89;
inline constexpr std::array<std::uint8_t, 9> kOwExceptionalWalkableFinalTiles{{
    0x8d, 0x91, 0x9c, 0xac, 0xad, 0xcc, 0xd2, 0xd5, 0xdf,
}};

enum class OwGeometryClass : std::uint8_t {
    // These are intentionally collision facts, not guessed terrain names.
    kWalkable = 0,
    kSolid = 1,
};

// HandleWarpOW accepts exactly these final tiles before it consults AttrsB.
// This deliberately remains orthogonal to collision: $24 is walkable by the
// ordinary ObjectRoomBoundsOW comparison, while its portal meaning comes from
// HandleWarpOW and the room's source attribute record.
inline constexpr bool ow_final_tile_is_warp_trigger(std::uint8_t final_tile) {
    return final_tile == 0x24 || final_tile == 0x88 ||
           (final_tile >= 0x70 && final_tile < 0x74);
}

// Source-coordinate facts for leaving a normal cave.  Z_05:HandleWarpOW
// recognizes the mouth; on return Z_05:InitMode2 applies AttrsA/F as
// ObjX=(A&$F0), StairsTargetY=$4D+((F&7)*$10), then starts the walk-out at
// ObjY=StairsTargetY+$10.  Viewport conversion is the renderer's absolute
// NES crop (x=8, y=72), not a new gameplay rule.
struct OwCaveReturnHotspot {
    std::uint8_t source_x;
    std::uint8_t source_target_y;
    std::uint8_t source_initial_y;
    std::uint8_t viewport_x;
    std::uint8_t viewport_y;
};

inline bool overworld_cave_return_hotspot(const FirstQuestData& data,
                                          std::uint8_t room_id,
                                          OwCaveReturnHotspot* out) {
    if (!out) return false;
    OverworldRoomView room{};
    if (!data.overworld_room(room_id & 0x0f, room_id >> 4, &room)) return false;
    const auto entrance = FirstQuestData::decode_overworld_entrance(room);
    if (entrance.kind != OverworldEntranceKind::kCave) return false;
    const unsigned target_y = 0x4d + static_cast<unsigned>(entrance.return_square_row) * 16;
    const unsigned initial_y = target_y + 16;
    if (entrance.return_x < 8 || initial_y < 72 || initial_y >= 232) return false;
    *out = {entrance.return_x, static_cast<std::uint8_t>(target_y),
            static_cast<std::uint8_t>(initial_y),
            static_cast<std::uint8_t>(entrance.return_x - 8),
            static_cast<std::uint8_t>(initial_y - 72)};
    return true;
}

struct OwRoomGeometry {
    OwFinalTileMap final_tiles{};
    std::array<OwGeometryClass, kOwRenderWidth * kOwRenderHeight> cells{};

    [[nodiscard]] OwGeometryClass at(std::size_t x, std::size_t y) const {
        return cells[y * kOwRenderWidth + x];
    }
    // The exact final tile consulted for the pixel, before the collision
    // routine's exceptional-tile normalization.
    [[nodiscard]] std::uint8_t final_tile_at(std::size_t x, std::size_t y) const {
        return final_tiles[(y / 8 + 1) * kOwPlayfieldTileWidth + (x / 8 + 1)];
    }
    [[nodiscard]] bool is_source_warp_trigger_at(std::size_t x, std::size_t y) const {
        return ow_final_tile_is_warp_trigger(final_tile_at(x, y));
    }
};

inline bool ow_final_tile_is_walkable(std::uint8_t final_tile) {
    for (const auto exception : kOwExceptionalWalkableFinalTiles) {
        if (final_tile == exception) return true;
    }
    return final_tile < kOwFirstUnwalkableFinalTile;
}

// Builds only the static initial room.  The source can later mutate this RAM
// playfield for secrets, ladders and pond animation; no mutation is inferred
// from PRG-only data here.  The 240x160 cells use the renderer's centered
// playfield crop exactly: source tile (x/8+1, y/8+1).
inline bool build_overworld_room_geometry(const FirstQuestData& data, std::uint8_t room_id,
                                          OwRoomGeometry* out) {
    if (!out || !build_overworld_final_tile_map(data, room_id, &out->final_tiles)) return false;
    for (std::size_t y = 0; y < kOwRenderHeight; ++y) {
        for (std::size_t x = 0; x < kOwRenderWidth; ++x) {
            out->cells[y * kOwRenderWidth + x] = ow_final_tile_is_walkable(out->final_tile_at(x, y))
                ? OwGeometryClass::kWalkable : OwGeometryClass::kSolid;
        }
    }
    return true;
}

enum class OwScreenEdge : std::uint8_t { kNorth, kSouth, kWest, kEast };

// Z_07.asm:PlayerScreenEdgeBounds.  These are original object coordinates,
// intentionally not projected to GBA pixels; an adapter owns that policy.
inline constexpr std::uint8_t ow_player_screen_edge_coordinate(OwScreenEdge edge) {
    switch (edge) {
    case OwScreenEdge::kNorth: return 0x3d;
    case OwScreenEdge::kSouth: return 0xdd;
    case OwScreenEdge::kWest: return 0x00;
    case OwScreenEdge::kEast: return 0xf0;
    }
    return 0;
}

// This is the source room-ID lattice (x + 16*y), not a claim that the entire
// edge is walkable.  Z_07's CheckScreenEdge additionally requires Link at an
// exact source coordinate; callers must combine this topology with geometry.
inline bool overworld_neighbor(std::uint8_t room_id, OwScreenEdge edge, std::uint8_t* out) {
    if (!out) return false;
    const auto x = static_cast<std::uint8_t>(room_id & 0x0f);
    const auto y = static_cast<std::uint8_t>(room_id >> 4);
    switch (edge) {
    case OwScreenEdge::kNorth: if (y == 0) return false; *out = room_id - 0x10; return true;
    case OwScreenEdge::kSouth: if (y + 1 >= kOverworldHeight) return false; *out = room_id + 0x10; return true;
    case OwScreenEdge::kWest:  if (x == 0) return false; *out = room_id - 1; return true;
    case OwScreenEdge::kEast:  if (x + 1 >= kOverworldWidth) return false; *out = room_id + 1; return true;
    }
    return false;
}

}  // namespace minish::foreign_world::zelda1
