#include "foreign_worlds/foreign_world.h"
#include "foreign_worlds/zelda1/zelda1_level1_live_adapter.h"

#include <array>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

namespace fw = minish::foreign_world;
namespace z1 = minish::foreign_world::zelda1;
namespace {
int fail(const std::string& message) { std::cerr << "FAIL: " << message << "\n"; return 1; }

bool add_item(fw::InventoryCore* inventory, fw::CrossWorldItemId id,
              fw::Capability capability, std::size_t slot) {
    std::string error;
    const fw::ItemTraits traits{1, id, fw::ItemUseKind::Equip,
                                fw::ResourcePoolProvenance::Native};
    return inventory->set_item(id, fw::OwnershipFlags::Owned, capability, traits, &error) &&
           inventory->set_loadout_slot(fw::LoadoutId::A, slot, id, &error);
}

// Plan with the same raw Z_07 directional hotspot probes as the session, then
// deliberately drive the real session one D-pad pixel at a time. Every legal
// ordinary/collar state is serialized and restored during replay; the planner
// never gets to change room IDs directly.
bool walk_ow77_to_ow37(z1::Zelda1OverworldSession* overworld,
                       std::string* compressed_trace) {
    if (!overworld || overworld->position().room_id != 0x77) return false;
    const auto* data = overworld->loader().first_quest_data();
    if (!data) return false;
    std::vector<z1::OwRoomGeometry> geometry(z1::kRoomCount);
    std::array<bool, z1::kRoomCount> geometry_ready{};
    const auto room_geometry = [&](std::uint8_t room) -> const z1::OwRoomGeometry* {
        if (!geometry_ready[room] &&
            !z1::build_overworld_room_geometry(*data, room, &geometry[room])) return nullptr;
        geometry_ready[room] = true; return &geometry[room];
    };
    const auto tile_at = [](const z1::OwRoomGeometry& g, int x, int y) -> int {
        if (x < 0 || x >= 256 || y < 0x40 || y >= 0xf0) return -1;
        return g.final_tiles[static_cast<std::size_t>((y - 0x40) / 8) *
                             z1::kOwPlayfieldTileWidth + static_cast<std::size_t>(x / 8)];
    };
    const auto probe_walkable = [&](const z1::OwRoomGeometry& g, int x, int y,
                                    int dx, int dy) {
        int px = x, py = y + 0x0b;
        if (dx) {
            if (!((dx < 0 && x < 0x10) || (dx > 0 && x >= 0xf0)))
                px += dx < 0 ? -0x08 : 0x10;
        } else if (!(dy > 0 && py >= 0xdd)) {
            py += dy < 0 ? -0x08 : 0x08;
        }
        int tile = tile_at(g, px, py);
        if (tile < 0) return false;
        if (!dx) { const int adjacent = tile_at(g, px + 8, py); if (adjacent < 0) return false; tile = std::max(tile, adjacent); }
        return z1::ow_final_tile_is_walkable(static_cast<std::uint8_t>(tile));
    };
    struct State { std::uint8_t room; std::uint16_t x, y; std::int8_t offset; std::uint8_t direction; int previous; char glyph; };
    struct Step { int x, y; char glyph; };
    constexpr std::array<Step, 4> steps{{{0,-1,'U'}, {-1,0,'L'}, {1,0,'R'}, {0,1,'D'}}};
    const auto key_of = [](const State& state) {
        return (static_cast<std::uint64_t>(state.room) << 32) |
            (static_cast<std::uint64_t>(state.x) << 24) |
            (static_cast<std::uint64_t>(state.y) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(state.offset)) << 8) |
            state.direction;
    };
    std::unordered_set<std::uint64_t> seen;
    std::vector<State> states;
    const auto source_start = overworld->source_position();
    states.push_back({source_start.room_id, static_cast<std::uint16_t>(source_start.obj_x),
                      static_cast<std::uint16_t>(source_start.obj_y), 0, 0, -1, 0});
    seen.insert(key_of(states.front()));
    std::queue<int> pending; pending.push(0); int goal = -1;
    while (!pending.empty() && goal < 0) {
        const int at = pending.front(); pending.pop(); const auto state = states[at];
        const auto* g = room_geometry(state.room); if (!g) return false;
        if (state.room == 0x37 && state.offset == 0 &&
            (state.x & 15) == 0 && (state.y & 15) == 13 &&
            z1::ow_source_still_is_warp_trigger(*g, state.x, state.y)) {
            goal = at; break;
        }
        for (const auto step : steps) {
            int move_x = step.x, move_y = step.y;
            std::uint8_t direction = step.x < 0 ? 3 : step.x > 0 ? 4 : step.y < 0 ? 1 : 2;
            if (state.offset != 0) {
                // The replay planner intentionally holds a cardinal D-pad
                // direction to the next source grid point.  The session's
                // separate regression covers Z_05's reverse/perpendicular
                // input behavior; searching those detours here only produces
                // noisy, noncanonical traces.
                if (direction != state.direction) continue;
                move_x = direction == 3 ? -1 : direction == 4 ? 1 : 0;
                move_y = direction == 1 ? -1 : direction == 2 ? 1 : 0;
            }
            int nx = state.x + move_x, ny = state.y + move_y;
            std::uint8_t room = state.room;
            const bool edge = (move_x < 0 && state.x == 0) || (move_x > 0 && state.x == 0xf0) ||
                (move_y < 0 && state.y == 0x3d) || (move_y > 0 && state.y == 0xdd);
            const bool sample_due = state.offset == 0;
            if (edge) {
                if (!sample_due) continue;
                const auto e = move_x < 0 ? z1::OwScreenEdge::kWest : move_x > 0 ? z1::OwScreenEdge::kEast :
                    move_y < 0 ? z1::OwScreenEdge::kNorth : z1::OwScreenEdge::kSouth;
                if (!z1::overworld_neighbor(room, e, &room)) continue;
                if (move_x < 0) nx = 0xf0; else if (move_x > 0) nx = 0; else if (move_y < 0) ny = 0xdd; else ny = 0x3d;
            } else {
                if (nx < 0 || nx > 0xf0 || ny < 0x3d || ny > 0xdd) continue;
                if (sample_due && !probe_walkable(*g, state.x, state.y, move_x, move_y)) continue;
            }
            std::int8_t offset = static_cast<std::int8_t>(
                state.offset + (move_x < 0 || move_y < 0 ? -1 : 1));
            if (edge || offset == 8 || offset == -8) { offset = 0; direction = 0; }
            const State next{room, static_cast<std::uint16_t>(nx), static_cast<std::uint16_t>(ny), offset, direction, at, step.glyph};
            if (!seen.insert(key_of(next)).second) continue;
            states.push_back(next);
            pending.push(static_cast<int>(states.size() - 1));
        }
    }
    if (goal < 0) return false;
    std::string raw;
    for (int at = goal; states[at].previous >= 0; at = states[at].previous) raw.push_back(states[at].glyph);
    std::reverse(raw.begin(), raw.end());
    for (const auto glyph : raw) {
        const auto checkpoint = overworld->serialize();
        const auto result = overworld->move_by(glyph == 'L' ? -1 : glyph == 'R' ? 1 : 0,
                                               glyph == 'U' ? -1 : glyph == 'D' ? 1 : 0);
        if ((result != z1::OverworldSessionMoveResult::kMoved && result != z1::OverworldSessionMoveResult::kCrossedRoom) ||
            !overworld->restore(overworld->serialize())) { std::cerr << "segment replay failed " << glyph << " result=" << static_cast<int>(result) << "\n"; return false; }
        (void)checkpoint;
    }
    if (compressed_trace) {
        compressed_trace->clear();
        for (std::size_t at = 0; at < raw.size();) {
            std::size_t end = at + 1;
            while (end < raw.size() && raw[end] == raw[at]) ++end;
            *compressed_trace += raw[at];
            *compressed_trace += std::to_string(end - at);
            if (end != raw.size()) *compressed_trace += ' ';
            at = end;
        }
    }
    return true;
}

int actual_rom_route(const char* path) {
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize> portal_state{};
    { // No room jump: replay the actual source-space OW77 -> OW37 route.
        z1::Zelda1OverworldSession route;
        std::string route_error, trace;
        const bool loaded = route.load_hash_validated_ines(path, 0x77, &route_error);
        const bool routed = loaded && walk_ow77_to_ow37(&route, &trace);
        const bool portal = routed && route.position().room_id == 0x37;
        if (!loaded || !routed || !portal) {
            return fail("could not source-traverse OW77 to the OW37 level entrance: " + route_error);
        }
        z1::Zelda1Level1LiveAdapter route_adapter;
        if (route_adapter.try_enter_from_overworld(route) != z1::Level1LiveResult::kEntered)
            return fail("OW77 source route reached OW37 but did not enter Level 1");
        portal_state = route.serialize();
        std::cout << "OW77->OW37 source D-pad trace: " << trace << " then portal route\n";
    }
    z1::Zelda1OverworldSession overworld;
    std::string error;
    if (!overworld.load_hash_validated_ines(path, 0x77, &error) ||
        !overworld.restore(portal_state, &error))
        return fail("could not restore the replayed OW37 source portal state: " + error);
    z1::Zelda1Level1LiveAdapter adapter;
    const auto enter_result = adapter.try_enter_from_overworld(overworld);
    if (enter_result != z1::Level1LiveResult::kEntered ||
        !adapter.presentation().framebuffer || adapter.presentation().room_id != 0x73)
        return fail("live adapter rejected source OW37 hotspot or did not expose L1 presentation");
    const auto saved = adapter.serialize();
    z1::Zelda1Level1LiveAdapter restored;
    if (!restored.restore(overworld, saved, &error) || !restored.active() ||
        restored.player_presentation().y != adapter.player_presentation().y)
        return fail("combined L1 adapter persistence did not restore atomically: " + error);

    fw::InventoryCore inventory;
    if (!add_item(&inventory, {fw::WorldId::Native, 0x42}, fw::Capability::Bomb, 0) ||
        !add_item(&inventory, {fw::WorldId::Native, 1}, fw::Capability::Sword, 1))
        return fail("could not build cross-world Native bomb/sword loadout");
    inventory.native_resources().keys = 17;
    const auto move = [&](std::int16_t x, std::int16_t y, z1::Level1LiveResult expected) {
        const auto result = adapter.move_by(x, y, &inventory, fw::LoadoutId::A);
        return result == expected;
    };
    const auto door = [&](z1::Level1Edge edge) {
        const auto player = adapter.player_presentation();
        switch (edge) {
        case z1::Level1Edge::kEast: return move(static_cast<std::int16_t>(240 - player.x), 0, z1::Level1LiveResult::kMoved);
        case z1::Level1Edge::kWest: return move(static_cast<std::int16_t>(-player.x - 1), 0, z1::Level1LiveResult::kMoved);
        case z1::Level1Edge::kSouth: return move(0, static_cast<std::int16_t>(160 - player.y), z1::Level1LiveResult::kMoved);
        case z1::Level1Edge::kNorth: return move(0, static_cast<std::int16_t>(-player.y - 1), z1::Level1LiveResult::kMoved);
        }
        return false;
    };
    const auto collect_here = [&] {
        if (!adapter.session()) return false;
        const auto item = adapter.session()->available_room_item();
        if (!item) return false;
        // Item collection is deliberately source-position gated: proving the
        // distant call is rejected prevents a menu-like room-wide pickup.
        if (adapter.collect_room_item(&inventory, &error) != z1::Level1LiveResult::kBlocked) return false;
        const auto player = adapter.player_presentation();
        if (!move(static_cast<std::int16_t>(item->x - 8 - player.x),
                  static_cast<std::int16_t>(item->y - 72 - player.y),
                  z1::Level1LiveResult::kMoved)) return false;
        return adapter.collect_room_item(&inventory, &error) == z1::Level1LiveResult::kCollected;
    };
    if (!door(z1::Level1Edge::kEast) || !adapter.session() || adapter.session()->room_id() != 0x74 ||
        !collect_here() ||
        inventory.ownership({fw::WorldId::Zelda1, 0x19}) != fw::OwnershipFlags::Owned ||
        inventory.zelda1_resources().keys != 1 ||
        !door(z1::Level1Edge::kWest) || !door(z1::Level1Edge::kNorth) ||
        !door(z1::Level1Edge::kNorth) || !door(z1::Level1Edge::kEast) ||
        !door(z1::Level1Edge::kNorth) || !door(z1::Level1Edge::kEast) ||
        !collect_here() || !door(z1::Level1Edge::kNorth) || !adapter.session() ||
        adapter.session()->room_id() != 0x35)
        return fail("live adapter did not traverse source L1 doors/keys/bomb boundary");
    const auto boss_player = adapter.player_presentation();
    if (!move(static_cast<std::int16_t>(168 - boss_player.x),
              static_cast<std::int16_t>(56 - boss_player.y), z1::Level1LiveResult::kMoved))
        return fail("could not place host presentation at source Aquamentus hitbox");
    const z1::MinishSwordSample swing{1, 7, 2, 1, 0};
    const fw::CrossWorldItemId zelda_sword{fw::WorldId::Zelda1, 1};
    const fw::ItemTraits zelda_sword_traits{1, zelda_sword, fw::ItemUseKind::Equip,
                                             fw::ResourcePoolProvenance::None};
    if (!inventory.acquire_item(zelda_sword, fw::OwnershipFlags::Owned,
                                fw::Capability::Sword, zelda_sword_traits, &error) ||
        !inventory.set_loadout_slot(fw::LoadoutId::A, 1, zelda_sword, &error) ||
        adapter.observe_sword({}, &inventory, fw::LoadoutId::A, true, 2, &error) !=
            z1::Level1LiveResult::kSwordHit || !adapter.session() ||
        adapter.session()->boss().hit_points != 5 ||
        inventory.loadout_slot(fw::LoadoutId::A, 1) != zelda_sword ||
        !inventory.traits({fw::WorldId::Native, 1}) || !inventory.traits(zelda_sword) ||
        inventory.traits({fw::WorldId::Native, 1})->behavior_id ==
            inventory.traits(zelda_sword)->behavior_id)
        return fail("selected Zelda1/01 host edge was not usable or origin-distinct in Level 1");
    // A simultaneous source swing is authoritative and still causes exactly
    // one host hit; the selected Zelda record stays selected.
    if (adapter.observe_sword(swing, &inventory, fw::LoadoutId::A, true, 2, &error) !=
            z1::Level1LiveResult::kSwordHit || !adapter.session() ||
        adapter.session()->boss().hit_points != 4 ||
        inventory.loadout_slot(fw::LoadoutId::A, 1) != zelda_sword)
        return fail("source ItemSword did not suppress duplicate Level 1 host edge");
    (void)adapter.observe_sword({}, &inventory, fw::LoadoutId::A, false, 2, &error);
    for (unsigned hit = 0; hit < 4; ++hit) {
        const auto expected = hit == 3 ? z1::Level1LiveResult::kBossDefeated : z1::Level1LiveResult::kSwordHit;
        if (adapter.observe_sword(swing, &inventory, fw::LoadoutId::A, false, 0, &error) != expected ||
            adapter.observe_sword({}, &inventory, fw::LoadoutId::A, false, 0, &error) == z1::Level1LiveResult::kInvalid)
            return fail("source sword edge did not apply exactly one Aquamentus hit: " + error);
    }
    if (!collect_here() ||
        inventory.ownership({fw::WorldId::Zelda1, 0x1a}) != fw::OwnershipFlags::Owned)
        return fail("boss reward did not remain origin-qualified in inventory");
    if (inventory.native_resources().keys != 17 || inventory.zelda1_resources().keys != 0 ||
        !adapter.session() || adapter.session()->key_count() != 0)
        return fail("source key door did not decrement only the synchronized Zelda key pool (native=" +
                    std::to_string(inventory.native_resources().keys) + ", zelda=" +
                    std::to_string(inventory.zelda1_resources().keys) + ", child=" +
                    std::to_string(adapter.session() ? adapter.session()->key_count() : 255) + ")");
    if (!door(z1::Level1Edge::kSouth) || !door(z1::Level1Edge::kWest) ||
        !door(z1::Level1Edge::kSouth) || !door(z1::Level1Edge::kWest) ||
        !door(z1::Level1Edge::kSouth) || !door(z1::Level1Edge::kSouth))
        return fail("could not return through source Level 1 room graph");
    std::uint8_t return_room = 0;
    if (adapter.move_by(0, static_cast<std::int16_t>(160 - adapter.player_presentation().y), &inventory, fw::LoadoutId::A, &return_room) != z1::Level1LiveResult::kReturned ||
        adapter.active() || return_room != 0x37)
        return fail("adapter did not constrain return to L1 start-room south exit");
    // Returning leaves the actual source OW37 hotspot unchanged. A subsequent
    // exact entry resumes this host-owned dungeon state instead of resetting
    // cleared items, unlocked doors, or the source key count.
    if (adapter.try_enter_from_overworld(overworld) != z1::Level1LiveResult::kEntered ||
        !adapter.session() || adapter.session()->key_count() != 0 ||
        inventory.native_resources().keys != 17 || inventory.zelda1_resources().keys != 0)
        return fail("source return/re-entry did not retain Level 1 and separate resource state");
    std::cout << "Actual-ROM Level 1 live adapter route passed\n";
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) return fail("optional argument is an iNES Zelda 1 ROM path");
    z1::Zelda1Level1LiveAdapter adapter;
    fw::InventoryCore inventory;
    if (adapter.move_by(1, 0, &inventory, fw::LoadoutId::A) != z1::Level1LiveResult::kNotActive ||
        adapter.collect_room_item(&inventory) != z1::Level1LiveResult::kNotActive ||
        adapter.observe_sword({}, &inventory, fw::LoadoutId::A) != z1::Level1LiveResult::kNotActive)
        return fail("inactive live adapter accepted a mutation");
    const auto empty = adapter.serialize();
    z1::Zelda1OverworldSession unloaded;
    if (!adapter.restore(unloaded, empty)) return fail("canonical inactive adapter blob was rejected");
    if (argc == 1) { std::cout << "Level 1 live adapter pure gates passed\n"; return 0; }
    return actual_rom_route(argv[1]);
}
