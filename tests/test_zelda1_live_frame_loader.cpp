#include "foreign_worlds/zelda1/zelda1_live_frame_loader.h"
#include "foreign_worlds/zelda1/zelda1_overworld_session.h"
#include "foreign_worlds/zelda1/zelda1_octorok_runtime.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace z1 = minish::foreign_world::zelda1;

namespace {

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

bool write_file(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
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

struct SourceProbe {
    bool walkable = false;
    std::array<std::pair<std::int16_t, std::int16_t>, 2> samples{};
    unsigned sample_count = 0;
};

std::optional<std::uint8_t> source_tile(const z1::OwRoomGeometry& geometry,
                                        std::int16_t x, std::int16_t y) {
    if (x < 0 || x >= 256 || y < 0x40 || y >= 0xf0) return std::nullopt;
    return geometry.final_tiles[static_cast<std::size_t>((y - 0x40) >> 3) *
                                    z1::kOwPlayfieldTileWidth +
                                static_cast<std::size_t>(x >> 3)];
}

// Test-side transcription of Z_07:GetCollidingTileMoving.  It deliberately
// does not call session internals: the actual session response below is the
// oracle we compare against this source probe and paint onto its framebuffer.
SourceProbe z07_source_probe(const z1::OwRoomGeometry& geometry,
                             std::int16_t obj_x, std::int16_t obj_y,
                             int direction, bool horizontal) {
    std::int16_t probe_x = obj_x;
    std::int16_t probe_y = static_cast<std::int16_t>(obj_y + 0x0b);
    if (horizontal) {
        if (!((direction < 0 && obj_x < 0x10) ||
              (direction > 0 && obj_x >= 0xf0)))
            probe_x += direction < 0 ? -0x08 : 0x10;
    } else if (!(direction > 0 && probe_y >= 0xdd)) {
        probe_y += direction < 0 ? -0x08 : 0x08;
    }
    SourceProbe result{};
    result.samples[result.sample_count++] = {probe_x, probe_y};
    const auto first = source_tile(geometry, probe_x, probe_y);
    if (!first) return result;
    std::uint8_t chosen = *first;
    if (!horizontal) {
        const auto adjacent_x = static_cast<std::int16_t>(probe_x + 8);
        result.samples[result.sample_count++] = {adjacent_x, probe_y};
        const auto adjacent = source_tile(geometry, adjacent_x, probe_y);
        if (!adjacent) return result;
        // Z_07 keeps the numerically higher adjacent tile; blocking final
        // tiles sort after ordinary walkable tiles.
        if (*adjacent > chosen) chosen = *adjacent;
    }
    result.walkable = z1::ow_final_tile_is_walkable(chosen);
    return result;
}

void paint_probe(z1::OwFramebuffer* frame, const SourceProbe& probe) {
    if (!frame) return;
    // Green marks a source-walkable probe and red a source blocker.  Mark the
    // exact raw NES tile sample(s), converted through the renderer crop
    // (ObjX-8, ObjY-72), instead of painting a guessed Link footprint.
    const std::uint16_t color = probe.walkable ? 0x03e0 : 0x001f;
    for (unsigned i = 0; i < probe.sample_count; ++i) {
        const auto [source_x, source_y] = probe.samples[i];
        const auto x = static_cast<std::int16_t>(source_x - 8);
        const auto y = static_cast<std::int16_t>(source_y - 72);
        if (x < 0 || y < 0 || x >= static_cast<std::int16_t>(z1::kOwRenderWidth) ||
            y >= static_cast<std::int16_t>(z1::kOwRenderHeight))
            continue;
        // Use a five-pixel cross rather than a single opaque dot: the marker
        // remains legible in a screenshot at native 240x160 while preserving
        // almost all source art. A grid point may be sampled by several
        // directions, so keep red dominant over a later open probe.
        constexpr std::array<std::pair<int, int>, 5> kCross{{
            {0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        }};
        for (const auto [dx, dy] : kCross) {
            const auto px = static_cast<std::int16_t>(x + dx);
            const auto py = static_cast<std::int16_t>(y + dy);
            if (px < 0 || py < 0 || px >= static_cast<std::int16_t>(z1::kOwRenderWidth) ||
                py >= static_cast<std::int16_t>(z1::kOwRenderHeight))
                continue;
            auto& pixel = (*frame)[static_cast<std::size_t>(py) * z1::kOwRenderWidth +
                                   static_cast<std::size_t>(px)];
            if (!probe.walkable || pixel != 0x001f) pixel = color;
        }
    }
}

void paint_collision_legend(z1::OwFramebuffer* frame) {
    if (!frame) return;
    // Top-left legend: green = PRG/source probe walkable; red = PRG/source
    // probe blocked; blue/magenta are reserved for an actual-session mismatch.
    constexpr std::array<std::uint16_t, 4> kLegend{{0x03e0, 0x001f, 0x7c00, 0x7c1f}};
    for (std::size_t block = 0; block < kLegend.size(); ++block)
        for (std::size_t y = 0; y < 5; ++y)
            for (std::size_t x = 0; x < 5; ++x)
                (*frame)[y * z1::kOwRenderWidth + block * 7 + x] = kLegend[block];
}

void stage_source_position(
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state,
    std::int16_t x, std::int16_t y);

bool audit_source_collision_overlay(const char* path, std::uint8_t room,
                                    const std::filesystem::path& artifact,
                                    std::string* failure) {
    auto session = std::make_unique<z1::Zelda1OverworldSession>();
    std::string error;
    if (!session->load_hash_validated_ines(path, room, &error)) {
        if (failure) *failure = "could not load room $" + std::to_string(room) + ": " + error;
        return false;
    }
    const auto room_start = session->serialize();
    auto overlay = std::make_unique<z1::OwFramebuffer>(session->framebuffer());
    paint_collision_legend(overlay.get());
    struct Direction { int dx, dy; bool horizontal; };
    constexpr std::array<Direction, 4> kDirections{{
        {-1, 0, true}, {1, 0, true}, {0, -1, false}, {0, 1, false},
    }};
    unsigned checked = 0;
    unsigned source_blocked = 0;
    for (std::int16_t y = 0x45; y <= 0xcd; y += 8) {
        for (std::int16_t x = 0x10; x <= 0xe0; x += 8) {
            for (const auto direction : kDirections) {
                const SourceProbe probe = z07_source_probe(
                    session->geometry(), x, y,
                    direction.horizontal ? direction.dx : direction.dy,
                    direction.horizontal);
                paint_probe(overlay.get(), probe);
                auto candidate = room_start;
                stage_source_position(&candidate, x, y);
                if (!session->restore(candidate, &error)) {
                    if (failure) *failure = "could not restore aligned source probe";
                    return false;
                }
                const auto result = session->move_by(direction.dx, direction.dy);
                const bool moved = result == z1::OverworldSessionMoveResult::kMoved ||
                                   result == z1::OverworldSessionMoveResult::kCrossedRoom;
                if (moved != probe.walkable) {
                    // Retain the artifact even on a divergence. Blue means
                    // source said walkable but the session blocked; magenta
                    // means source said blocked but the session moved.
                    const auto mismatch = probe.walkable ? 0x7c00 : 0x7c1f;
                    for (unsigned i = 0; i < probe.sample_count; ++i) {
                        const auto x = static_cast<std::int16_t>(probe.samples[i].first - 8);
                        const auto y = static_cast<std::int16_t>(probe.samples[i].second - 72);
                        if (x >= 0 && y >= 0 && x < static_cast<std::int16_t>(z1::kOwRenderWidth) &&
                            y < static_cast<std::int16_t>(z1::kOwRenderHeight))
                            (*overlay)[static_cast<std::size_t>(y) * z1::kOwRenderWidth + x] = mismatch;
                    }
                    (void)write_frame_ppm(artifact, *overlay);
                    if (failure) {
                        *failure = "Z_07 probe mismatch in room $" + std::to_string(room) +
                            " at Obj=(" + std::to_string(x) + "," + std::to_string(y) + ")";
                    }
                    return false;
                }
                if (!probe.walkable) ++source_blocked;
                ++checked;
            }
        }
    }
    if (checked == 0 || source_blocked == 0 || !session->restore(room_start, &error) ||
        !write_frame_ppm(artifact, *overlay)) {
        if (failure) *failure = "could not retain collision overlay";
        return false;
    }
    return true;
}

bool ordinary_dpad_path_reaches_visible_wall(const char* path, std::uint8_t room,
                                             std::string* failure) {
    // This deliberately starts from the real load position (no serialized
    // hotspot staging) and feeds only ordinary held cardinal input.  It must
    // cross open terrain for at least one complete source grid segment and
    // stop at a rendered blocker.
    auto session = std::make_unique<z1::Zelda1OverworldSession>();
    std::string error;
    if (!session->load_hash_validated_ines(path, room, &error)) return false;
    // These two paths begin at the exact source-load point, never restore a
    // staged hotspot, and feed a single held Right D-pad input. Their stop
    // coordinates are deliberately pinned in renderer-crop space so a future
    // coordinate/focus translation cannot silently move collision away from
    // the visible tree wall.
    struct Path { unsigned pixels; std::int16_t stop_x, stop_y; };
    const auto expected = room == 0x77 ? Path{80, 208, 157} :
                          room == 0x76 ? Path{16, 144, 157} : Path{};
    if (expected.pixels == 0) return false;
    for (unsigned i = 0; i < expected.pixels; ++i)
        if (session->move_by(1, 0) != z1::OverworldSessionMoveResult::kMoved) {
            if (failure) *failure = "ordinary D-pad open-ground path stopped early";
            return false;
        }
    const auto stop = session->source_position();
    const auto probe = z07_source_probe(session->geometry(), stop.obj_x, stop.obj_y, 1, true);
    if (stop.obj_x != expected.stop_x || stop.obj_y != expected.stop_y || probe.walkable ||
        session->move_by(1, 0) != z1::OverworldSessionMoveResult::kBlocked) {
        if (failure) {
            *failure = "ordinary D-pad did not stop at the pinned visible wall crop=(" +
                std::to_string(expected.stop_x - 8) + "," +
                std::to_string(expected.stop_y - 72) + ")";
        }
        return false;
    }
    return true;
}

std::vector<std::uint8_t> synthetic_prg0_ines() {
    std::vector<std::uint8_t> bytes(16 + 128 * 1024);
    constexpr std::array<std::uint8_t, 16> header{{
        'N', 'E', 'S', 0x1a, 8, 0, 0x12, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    }};
    std::copy(header.begin(), header.end(), bytes.begin());
    return bytes;
}

std::uint64_t framebuffer_hash(const z1::OwFramebuffer& frame) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto pixel : frame) { hash ^= pixel; hash *= 1099511628211ull; }
    return hash;
}

bool same_rectangle(const z1::OwFramebuffer& first, const z1::OwFramebuffer& second,
                    unsigned x, unsigned y, unsigned width, unsigned height) {
    for (unsigned py = y; py < y + height; ++py)
        for (unsigned px = x; px < x + width; ++px)
            if (first[py * z1::kOwRenderWidth + px] !=
                second[py * z1::kOwRenderWidth + px])
                return false;
    return true;
}

bool footprint_walkable(const z1::OwRoomGeometry& geometry, std::uint8_t x, std::uint8_t y) {
    if (x > z1::kOwRenderWidth - z1::Zelda1OverworldSession::kLinkFootprintWidth ||
        y > z1::kOwRenderHeight - z1::Zelda1OverworldSession::kLinkFootprintHeight) return false;
    for (std::size_t py = y; py < static_cast<std::size_t>(y) + z1::Zelda1OverworldSession::kLinkFootprintHeight; ++py)
        for (std::size_t px = x; px < static_cast<std::size_t>(x) + z1::Zelda1OverworldSession::kLinkFootprintWidth; ++px)
            if (geometry.at(px, py) != z1::OwGeometryClass::kWalkable) return false;
    return true;
}

bool move_to(z1::Zelda1OverworldSession* session, std::uint8_t target_x, std::uint8_t target_y) {
    if (!session || !footprint_walkable(session->geometry(), target_x, target_y)) return false;
    constexpr std::size_t kWidth = z1::kOwRenderWidth - z1::Zelda1OverworldSession::kLinkFootprintWidth + 1;
    constexpr std::size_t kHeight = z1::kOwRenderHeight - z1::Zelda1OverworldSession::kLinkFootprintHeight + 1;
    const auto index = [](std::uint8_t x, std::uint8_t y) { return static_cast<std::size_t>(y) * kWidth + x; };
    const auto start = session->position();
    std::vector<int> previous(kWidth * kHeight, -2);
    std::queue<std::pair<std::uint8_t, std::uint8_t>> pending;
    previous[index(start.x, start.y)] = -1;
    pending.push({start.x, start.y});
    constexpr std::array<std::pair<int, int>, 4> kSteps{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
    while (!pending.empty() && previous[index(target_x, target_y)] == -2) {
        const auto [x, y] = pending.front(); pending.pop();
        for (const auto [dx, dy] : kSteps) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= static_cast<int>(kWidth) || ny >= static_cast<int>(kHeight) ||
                previous[index(static_cast<std::uint8_t>(nx), static_cast<std::uint8_t>(ny))] != -2 ||
                !footprint_walkable(session->geometry(), static_cast<std::uint8_t>(nx), static_cast<std::uint8_t>(ny))) continue;
            previous[index(static_cast<std::uint8_t>(nx), static_cast<std::uint8_t>(ny))] = static_cast<int>(index(x, y));
            pending.push({static_cast<std::uint8_t>(nx), static_cast<std::uint8_t>(ny)});
        }
    }
    const auto target = index(target_x, target_y);
    if (previous[target] == -2) return false;
    std::vector<std::size_t> path;
    for (auto cursor = target; previous[cursor] != -1; cursor = static_cast<std::size_t>(previous[cursor])) path.push_back(cursor);
    std::reverse(path.begin(), path.end());
    for (const auto point : path) {
        const auto x = static_cast<std::uint8_t>(point % kWidth);
        const auto y = static_cast<std::uint8_t>(point / kWidth);
        const auto current = session->position();
        if (session->move_by(static_cast<std::int16_t>(x - current.x), static_cast<std::int16_t>(y - current.y)) !=
            z1::OverworldSessionMoveResult::kMoved) return false;
    }
    return true;
}

// Mirrors the live LinearMoveDirectionOLD adapter's source movement policy:
// decompose a host delta into one-pixel cardinal probes and test source warp
// hotspots after each successful probe.  This regression protects exact
// Zelda-1 entrances from a fixed-speed Minish two-pixel movement update.
bool move_overworld_delta_with_entry_checks(z1::Zelda1OverworldSession* session,
                                            std::int16_t delta_x,
                                            std::int16_t delta_y) {
    if (!session || session->in_cave()) return false;
    const auto move_axis = [&](std::int16_t amount, bool horizontal) {
        if (amount == 0) return true;
        const std::int16_t step = amount < 0 ? -1 : 1;
        const unsigned count = static_cast<unsigned>(amount < 0 ? -amount : amount);
        for (unsigned pixel = 0; pixel < count; ++pixel) {
            const auto movement = session->move_by(horizontal ? step : 0,
                                                   horizontal ? 0 : step);
            if (movement != z1::OverworldSessionMoveResult::kMoved) return false;
            if (session->try_enter_cave() == z1::OverworldSessionCaveResult::kEntered)
                return false;
        }
        return true;
    };
    if (!move_axis(delta_x, true) || session->in_cave()) return session->in_cave();
    (void)move_axis(delta_y, false);
    return session->in_cave();
}

void stage_source_position(
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state,
    std::int16_t x, std::int16_t y) {
    if (!state) return;
    (*state)[7] = static_cast<std::uint8_t>(x & 0xff);
    (*state)[8] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(x) >> 8) & 0xff);
    (*state)[9] = static_cast<std::uint8_t>(y & 0xff);
    (*state)[10] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(y) >> 8) & 0xff);
    (*state)[17] = 0;
    (*state)[20] = 0;
}

void stage_source_segment(
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state,
    std::int16_t x, std::int16_t y, std::int8_t offset,
    z1::OverworldWalkDirection direction) {
    stage_source_position(state, x, y);
    (*state)[17] = static_cast<std::uint8_t>(offset);
    (*state)[20] = static_cast<std::uint8_t>(direction);
}

bool cross_at_matching_opening(z1::Zelda1OverworldSession* session, z1::OwScreenEdge edge) {
    if (!session) return false;
    std::uint8_t neighbor = 0;
    if (!z1::overworld_neighbor(session->position().room_id, edge, &neighbor)) return false;
    // Stage an exact source grid point one pixel before a PlayerScreenEdge-
    // Bounds value. This tests raw edge semantics without the old cropped
    // opening-search policy (which cannot represent north/west collars).
    auto state = session->serialize();
    std::int16_t x = 0x80, y = 0x9d, dx = 0, dy = 0;
    switch (edge) {
    case z1::OwScreenEdge::kNorth: y = 0x3e; dy = -1; break;
    case z1::OwScreenEdge::kSouth: y = 0xdc; dy = 1; break;
    case z1::OwScreenEdge::kWest: x = 1; dx = -1; break;
    case z1::OwScreenEdge::kEast: x = 0xef; dx = 1; break;
    }
    state[7] = static_cast<std::uint8_t>(x); state[8] = 0;
    state[9] = static_cast<std::uint8_t>(y); state[10] = 0;
    if (!session->restore(state)) return false;
    // Z_07 checks PlayerScreenEdgeBounds before MoveObject. Starting one
    // pixel inboard therefore reaches the exact edge first, then crosses on
    // the next input frame.
    if (session->move_by(dx, dy) != z1::OverworldSessionMoveResult::kMoved)
        return false;
    const auto result = session->move_by(dx, dy);
    return result == z1::OverworldSessionMoveResult::kCrossedRoom &&
           session->position().room_id == neighbor;
}

bool can_prove_solid_block(z1::Zelda1OverworldSession* session) {
    if (!session) return false;
    const auto saved = session->serialize();
    // Set up a source-grid point one pixel before a proven Z_07 right probe
    // into a solid final tile. The host must stop on the aligned sample, not
    // bypass it because the starting presentation coordinate is unaligned.
    const auto& tiles = session->geometry().final_tiles;
    for (unsigned y = 0x45; y < 0xdd; y += 8) for (unsigned x = 8; x < 0xe0; x += 8) {
        const unsigned probe_x = x + 0x10, probe_y = y + 0x0b;
        if (probe_x >= 256 || probe_y >= 0xf0) continue;
        const auto tile = tiles[(probe_y - 0x40) / 8 * z1::kOwPlayfieldTileWidth + probe_x / 8];
        const auto left_y = y + 3;
        if (z1::ow_final_tile_is_walkable(tile) || x < 8 || left_y < 0x40 ||
            !z1::ow_final_tile_is_walkable(tiles[(left_y - 0x40) / 8 * z1::kOwPlayfieldTileWidth + (x - 8) / 8])) continue;
        auto candidate = saved;
        candidate[7] = static_cast<std::uint8_t>(x); candidate[8] = 0;
        candidate[9] = static_cast<std::uint8_t>(y); candidate[10] = 0;
        if (!session->restore(candidate) ||
            session->move_by(1, 0) != z1::OverworldSessionMoveResult::kBlocked ||
            session->source_position().obj_x != static_cast<std::int16_t>(x) ||
            // Reversing at the blocked aligned point is legal.
            session->move_by(-1, 0) != z1::OverworldSessionMoveResult::kMoved ||
            !session->restore(saved)) return false;
        return true;
    }
    return false;
}

bool can_prove_vertical_pair_prefers_visible_blocker(
    z1::Zelda1OverworldSession* session) {
    if (!session) return false;
    const auto saved = session->serialize();
    const auto& tiles = session->geometry().final_tiles;
    // Z_07 down probe: candidate ObjY + $0b + $08, then the next tile
    // column. Find an actual source pair whose first tile is passable and
    // whose adjacent visible tile is blocking; the higher final-tile value
    // must win or Link walks through that boundary.
    for (unsigned y = 0x45; y < 0xc5; y += 8) for (unsigned x = 0; x < 0xe8; x += 8) {
        const unsigned probe_y = y + 0x13;
        if (probe_y >= 0xf0 || x + 8 >= 256) continue;
        const auto first = tiles[(probe_y - 0x40) / 8 * z1::kOwPlayfieldTileWidth + x / 8];
        const auto adjacent = tiles[(probe_y - 0x40) / 8 * z1::kOwPlayfieldTileWidth + x / 8 + 1];
        if (!z1::ow_final_tile_is_walkable(first) || z1::ow_final_tile_is_walkable(adjacent) ||
            adjacent <= first) continue;
        auto candidate = saved;
        candidate[7] = static_cast<std::uint8_t>(x); candidate[8] = 0;
        candidate[9] = static_cast<std::uint8_t>(y); candidate[10] = 0;
        if (!session->restore(candidate) ||
            session->move_by(0, 1) != z1::OverworldSessionMoveResult::kBlocked ||
            session->source_position().obj_y != static_cast<std::int16_t>(y) ||
            !session->restore(saved)) return false;
        return true;
    }
    return false;
}

bool can_prove_midcell_turn_lock(z1::Zelda1OverworldSession* session) {
    if (!session) return false;
    const auto saved = session->serialize();
    const auto& tiles = session->geometry().final_tiles;
    bool staged = false;
    for (unsigned y = 0x45; y < 0xc5 && !staged; y += 8) for (unsigned x = 0x10; x < 0xd8; x += 8) {
        const unsigned probe_x = x + 0x10, probe_y = y + 0x0b;
        const unsigned down_y = y + 0x13;
        if (probe_y >= 0xf0 || down_y >= 0xf0 || x + 8 >= 256 ||
            !z1::ow_final_tile_is_walkable(
                tiles[(probe_y - 0x40) / 8 * z1::kOwPlayfieldTileWidth + probe_x / 8]) ||
            !z1::ow_final_tile_is_walkable(std::max(
                tiles[(down_y - 0x40) / 8 * z1::kOwPlayfieldTileWidth + x / 8],
                tiles[(down_y - 0x40) / 8 * z1::kOwPlayfieldTileWidth + x / 8 + 1]))) continue;
        auto candidate = saved;
        candidate[7] = static_cast<std::uint8_t>(x); candidate[8] = 0;
        candidate[9] = static_cast<std::uint8_t>(y); candidate[10] = 0;
        if (session->restore(candidate)) staged = true;
    }
    if (!staged) return false;
    const auto start = session->source_position();
    const auto staged_state = session->serialize();
    // Z1OS must reject the noncanonical form that previously escaped the
    // live walker: zero displacement with an active-segment Right token.
    auto malformed_zero_segment = staged_state;
    malformed_zero_segment[17] = 0;
    malformed_zero_segment[20] = static_cast<std::uint8_t>(
        z1::OverworldWalkDirection::kRight);
    if (z1::Zelda1OverworldSession::validate_serialized(malformed_zero_segment))
        return false;
    // A release preserves phase. Z_05 allows an opposite direction to walk
    // back to the preceding point; an early perpendicular request performs
    // that same reversal before it can turn on the new axis.
    if (session->move_by(1, 0) != z1::OverworldSessionMoveResult::kMoved ||
        session->move_by(0, 0) != z1::OverworldSessionMoveResult::kMoved ||
        session->source_position().obj_x != start.obj_x + 1 ||
        session->source_position().obj_y != start.obj_y)
        return false;
    const auto mid_segment = session->serialize();
    if (!session->restore(mid_segment) ||
        session->move_by(-1, 0) != z1::OverworldSessionMoveResult::kMoved ||
        session->source_position().obj_x != start.obj_x ||
        !z1::Zelda1OverworldSession::validate_serialized(session->serialize()) ||
        !session->restore(session->serialize()))
        return false;
    // Every partial Right/Down displacement proven safe by the staged source
    // probes above must round-trip after its opposite Left/Up reversal. This
    // is the exact persistence seam exercised by the live plugin when a
    // player alternates D-pad directions between callbacks. Do not initiate
    // unproven Left/Up probes from this fixture merely to make the matrix look
    // symmetric: their input is already covered as the reversal leg.
    for (int axis = 0; axis != 2; ++axis) {
        for (int distance = 1; distance != 8; ++distance) {
            if (!session->restore(staged_state)) return false;
            const auto delta_x = static_cast<std::int16_t>(axis == 0 ? 1 : 0);
            const auto delta_y = static_cast<std::int16_t>(axis == 1 ? 1 : 0);
            bool moved = true;
            for (int i = 0; i != distance; ++i)
                moved = moved && session->move_by(delta_x, delta_y) ==
                    z1::OverworldSessionMoveResult::kMoved;
            for (int i = 0; i != distance; ++i)
                moved = moved && session->move_by(-delta_x, -delta_y) ==
                    z1::OverworldSessionMoveResult::kMoved;
            const auto checkpoint = session->serialize();
            if (!moved || !z1::Zelda1OverworldSession::validate_serialized(checkpoint) ||
                !session->restore(checkpoint) ||
                session->source_position().obj_x != start.obj_x ||
                session->source_position().obj_y != start.obj_y)
                return false;
        }
    }
    if (!session->restore(staged_state)) return false;
    if (session->move_by(1, 0) != z1::OverworldSessionMoveResult::kMoved ||
        session->move_by(0, 1) != z1::OverworldSessionMoveResult::kMoved ||
        session->source_position().obj_x != start.obj_x ||
        session->source_position().obj_y != start.obj_y ||
        session->move_by(0, 1) != z1::OverworldSessionMoveResult::kMoved ||
        session->source_position().obj_x != start.obj_x ||
        session->source_position().obj_y != start.obj_y + 1 ||
        !session->restore(saved)) return false;
    return true;
}

int test_synthetic_rejections() {
    const auto directory = std::filesystem::current_path() / "build";
    std::filesystem::create_directories(directory);
    const auto good_path = directory / "zelda1_live_loader_valid_shape.nes";
    const auto bad_header_path = directory / "zelda1_live_loader_bad_header.nes";
    const auto bad_size_path = directory / "zelda1_live_loader_bad_size.nes";
    auto valid = synthetic_prg0_ines();
    if (!write_file(good_path, valid)) return fail("could not write synthetic iNES input");

    z1::Zelda1LiveFrameLoader loader;
    std::string error;
    if (!loader.load_hash_validated_ines(good_path, &error) || !loader.loaded() ||
        !loader.first_quest_data())
        return fail("canonical synthetic iNES shape was rejected: " + error);
    const auto stable_frame = loader.framebuffer();

    auto bad_header = valid;
    bad_header[0] = 'X';
    if (!write_file(bad_header_path, bad_header) ||
        loader.load_hash_validated_ines(bad_header_path, &error) || error.empty() ||
        !loader.loaded() || loader.framebuffer() != stable_frame)
        return fail("bad iNES header was accepted or changed live loader state");

    valid.pop_back();
    if (!write_file(bad_size_path, valid) ||
        loader.load_hash_validated_ines(bad_size_path, &error) || error.empty() ||
        !loader.loaded() || loader.framebuffer() != stable_frame)
        return fail("bad iNES size was accepted or changed live loader state");
    return 0;
}

int test_actual_ines_path(const char* path) {
    z1::Zelda1LiveFrameLoader loader;
    std::string error;
    if (!loader.load_and_render_hash_validated_ines(path, 0x77, &error))
        return fail("actual hash-validated PRG0 iNES did not load/render: " + error);
    z1::OverworldRoomView start_room{};
    z1::OverworldCaveItemsView start_cave_items{};
    if (!loader.first_quest_data() ||
        !loader.first_quest_data()->overworld_room(7, 7, &start_room, &error) ||
        !loader.first_quest_data()->overworld_cave_items(start_room, &start_cave_items, &error) ||
        start_cave_items.source_cave_slot != 0 ||
        start_cave_items.raw_item_ids != std::array<std::uint8_t, 3>{{0x3f, 0x01, 0x7f}})
        return fail("actual OW77 cave-item record was not source-decoded: " + error);

    std::uint64_t hash = 1469598103934665603ull;
    for (const auto pixel : loader.framebuffer()) {
        hash ^= pixel;
        hash *= 1099511628211ull;
    }
    if (hash != 10743561982003409067ull)
        return fail("actual PRG0 OW77 framebuffer hash changed: " + std::to_string(hash));
    if (!write_frame_ppm("build/zelda1_live_loader_ow77_validation.ppm", loader.framebuffer()))
        return fail("could not create live-loader OW77 QA artifact");
    { // Actual-ROM bounded OW66 source roster/combat/persistence QA.
        z1::Ow66OctorokRuntime octoroks;
        if (!octoroks.initialize(*loader.first_quest_data(), 0))
            return fail("actual OW66 red-Octorok source roster did not initialize");
        if (!loader.render_overworld_room(0x66, &error)) return fail("OW66 terrain render failed: " + error);
        auto before = loader.framebuffer(); octoroks.render_qa_markers(&before);
        if (!write_frame_ppm("build/zelda1_ow66_octorok_before.ppm", before)) return fail("OW66 before artifact failed");
        octoroks.tick();
        std::uint8_t contact = 0;
        if (!octoroks.contact(0, octoroks.actors()[0].x, octoroks.actors()[0].y, &contact) || contact != 1 ||
            !octoroks.sword_hit(0, 3) || !octoroks.actors()[0].defeated || !octoroks.actors()[0].pending_drop)
            return fail("OW66 Octorok contact/hit/defeat contract failed");
        auto combat = loader.framebuffer(); octoroks.render_qa_markers(&combat);
        if (!write_frame_ppm("build/zelda1_ow66_octorok_combat.ppm", combat)) return fail("OW66 combat artifact failed");
        auto drop = loader.framebuffer(); octoroks.render_qa_markers(&drop);
        if (!write_frame_ppm("build/zelda1_ow66_octorok_drop.ppm", drop) || !octoroks.collect_drop(0)) return fail("OW66 drop artifact failed");
        auto defeat = loader.framebuffer(); octoroks.render_qa_markers(&defeat);
        if (!write_frame_ppm("build/zelda1_ow66_octorok_defeat.ppm", defeat)) return fail("OW66 defeat artifact failed");
        const auto saved = octoroks.serialize(); auto corrupt = saved; corrupt[29] = 1;
        if (octoroks.restore(corrupt) || octoroks.serialize() != saved || !octoroks.restore(saved))
            return fail("Octorok persistence corruption/nonmutation failed");
        if (!loader.render_overworld_room(0x77, &error)) return fail("could not restore OW77 after Octorok QA");
    }

    const auto stable_frame = loader.framebuffer();
    if (loader.load_and_render_hash_validated_ines(path, 0x80, &error) || error.empty() ||
        loader.framebuffer() != stable_frame)
        return fail("failed load-and-render changed the stable framebuffer");
    if (loader.render_overworld_room(0x80, &error) || error.empty() ||
        loader.framebuffer() != stable_frame)
        return fail("failed room render changed the stable framebuffer");
    z1::Zelda1OverworldSession session;
    if (!session.load_hash_validated_ines(path, z1::Zelda1OverworldSession::kInitialRoom, &error) || !session.loaded() ||
        session.position().room_id != 0x77 || session.position().x != 120 ||
        session.position().y != 85 || session.framebuffer() != stable_frame)
        return fail("OW session did not atomically initialize from the live loader: " + error);
    const auto initial_feet = session.presentation_feet_position();
    if (initial_feet.x != 128 || initial_feet.y != 101)
        return fail("OW presentation did not map source sprite bottom-center feet from the initial position");
    const auto initial_state = session.serialize();
    if (!z1::Zelda1OverworldSession::validate_serialized(initial_state) ||
        !can_prove_solid_block(&session) ||
        !can_prove_vertical_pair_prefers_visible_blocker(&session) ||
        session.position().room_id != 0x77 || session.framebuffer() != stable_frame)
        return fail("OW session did not match source visible final-tile collision atomically");
    if (!session.restore(initial_state, &error) || !can_prove_midcell_turn_lock(&session))
        return fail("OW session did not retain source ObjGridOffset/ObjDir through a mid-cell turn");
    // Exhaust every interior source-grid collision probe against the actual
    // session response, then retain a screenshot-review overlay.  OW76 is a
    // direct neighbour of the start room and guards that this is not a
    // one-room renderer/collision coincidence.
    std::string collision_failure;
    if (!audit_source_collision_overlay(path, 0x77,
                                        "build/zelda1_ow77_collision_probes.ppm",
                                        &collision_failure) ||
        !audit_source_collision_overlay(path, 0x76,
                                        "build/zelda1_ow76_collision_probes.ppm",
                                        &collision_failure) ||
        !ordinary_dpad_path_reaches_visible_wall(path, 0x77, &collision_failure) ||
        !ordinary_dpad_path_reaches_visible_wall(path, 0x76, &collision_failure))
        return fail("source collision audit failed: " + collision_failure);
    // Cover several independently rendered First Quest rooms. Each check
    // stages an actual source final-tile boundary and proves a one-pixel host
    // movement cannot cross it, so collision cannot drift from the map used
    // to draw those rooms.
    for (const std::uint8_t room : std::array<std::uint8_t, 3>{{0x67, 0x66, 0x76}}) {
        z1::Zelda1OverworldSession visual_collision;
        if (!visual_collision.load_hash_validated_ines(path, room, &error) ||
            !can_prove_solid_block(&visual_collision))
            return fail("rendered source terrain and collision disagreed in OW room $" +
                        std::to_string(room));
    }
    // Z_05:HandleWarpOW accepts the $24 mouth only when the source hotspot is
    // grid-aligned. GetCollidableTileStill samples ObjY+$0B, so the visible
    // OW77 mouth is aligned Obj=($40,$4D), crop=(56,5), not the later
    // InitMode2 walk-out coordinate ($40,$5D). Start one pixel away and
    // supply a two-pixel host delta to prove no fixed-speed input skips it.
    struct PortalCrossingProbe { std::uint8_t x, y; std::int16_t dx, dy; };
    constexpr std::array<PortalCrossingProbe, 4> kPortalCrossingProbes{{
        {54, 5, 2, 0}, {58, 5, -2, 0}, {56, 3, 0, 2}, {56, 7, 0, -2},
    }};
    bool crossed_cave_hotspot = false;
    for (const auto probe : kPortalCrossingProbes) {
        auto crossing_state = initial_state;
        const auto x = static_cast<std::int16_t>(probe.x + 8);
        const auto y = static_cast<std::int16_t>(probe.y + 72);
        const auto direction = probe.dx > 0 ? z1::OverworldWalkDirection::kRight :
            probe.dx < 0 ? z1::OverworldWalkDirection::kLeft :
            probe.dy > 0 ? z1::OverworldWalkDirection::kDown : z1::OverworldWalkDirection::kUp;
        stage_source_segment(&crossing_state, x, y,
                             static_cast<std::int8_t>((probe.dx < 0 || probe.dy < 0) ? -6 : 6),
                             direction);
        if (!session.restore(crossing_state, &error))
            return fail("could not reset OW77 before two-pixel portal probe: " + error);
        if (move_overworld_delta_with_entry_checks(&session, probe.dx, probe.dy) &&
            session.in_cave()) {
            crossed_cave_hotspot = true;
            break;
        }
    }
    auto cave_hotspot = initial_state;
    stage_source_position(&cave_hotspot, 0x40, 0x4d);
    if (!crossed_cave_hotspot || !session.restore(cave_hotspot, &error) ||
        session.try_enter_cave() != z1::OverworldSessionCaveResult::kEntered ||
        !session.in_cave() || session.area() != z1::OverworldSessionArea::kCave ||
        session.cave_index() != 16 ||
        session.move_by(1, 0) != z1::OverworldSessionMoveResult::kBlocked)
        return fail("OW77 source cave mouth did not enter the normal-cave state");
    const auto cave_before_sword = session.framebuffer();
    if (framebuffer_hash(cave_before_sword) != 1204060643028526704ull ||
        !write_frame_ppm("build/zelda1_start_sword_cave_before.ppm", cave_before_sword))
        return fail("start sword cave did not produce its pinned source framebuffer");
    // InitCave creates the Old Man and both fires before Zelda's first
    // textbox character. The source PPU transfer then writes selector-zero
    // glyphs at `$21A4`/`$21C4`; its final `$EC`/`$C0` leaves them visible
    // after UnhaltLink rather than treating dialogue as a host A prompt.
    std::vector<std::uint8_t> cave_patterns;
    z1::OwFramebuffer cave_dialogue_complete{};
    if (!z1::copy_overworld_background_from_verified_prg(*loader.first_quest_data(),
                                                         &cave_patterns, &error) ||
        !z1::render_first_quest_start_cave(*loader.first_quest_data(), cave_patterns,
                                           {false, true, 0}, &cave_dialogue_complete) ||
        cave_dialogue_complete == cave_before_sword ||
        !write_frame_ppm("build/zelda1_start_sword_cave_text_visible.ppm",
                         cave_dialogue_complete))
        return fail("source cave textbox did not write the verified visible text: " + error);
    const auto cave_entry_state = session.serialize();
    const auto cave_entry_position = session.cave_source_position();
    const auto cave_entry_presentation = session.cave_presentation_position();
    const auto overworld_feet = session.presentation_feet_position();
    if (!cave_entry_position || !cave_entry_presentation || cave_entry_position->x != 0x70 ||
        cave_entry_position->y != 0xad || cave_entry_presentation->x != 0x70 ||
        cave_entry_presentation->y != 0x75 || overworld_feet.x != 64 ||
        overworld_feet.y != 21 || !session.cave_entry_settled() ||
        session.cave_dialogue_acknowledged() ||
        session.move_cave_one(z1::CaveDirection::kDown) !=
            z1::StartCaveMoveResult::kDialogueBlocked ||
        !session.restore(cave_entry_state, &error))
        return fail("start cave did not settle, expose mapped position, or preserve dialogue gate");
    // The type-$40 bonfires update each source cave frame even while the
    // textbox is typing. At six updates, UpdateStandingFire's
    // AnimateObjectWalking rollover mirrors both 16x16 OAM pairs. Restore
    // must retain that phase without allowing a malformed fire byte to alter
    // a live framebuffer/session.
    for (unsigned frame = 0;
         frame != z1::Zelda1StartCaveControl::kStandingFireFramesPerPhase;
         ++frame)
      if (!session.tick_cave_frame()) return fail("source cave fire stopped advancing");
    const auto fire_phase_one_state = session.serialize();
    // Keep this additional actual-ROM framebuffer off this already large
    // integration test's stack.
    const auto fire_phase_one_frame =
        std::make_unique<z1::OwFramebuffer>(session.framebuffer());
    if (fire_phase_one_state[4] != 9 || fire_phase_one_state[23] != 1 ||
        fire_phase_one_state[24] != z1::Zelda1StartCaveControl::kStandingFireFramesPerPhase ||
        framebuffer_hash(*fire_phase_one_frame) != 17226980411102044710ull ||
        same_rectangle(cave_before_sword, *fire_phase_one_frame, 0x48 - 8, 0x80 - 72,
                       16, 16) ||
        same_rectangle(cave_before_sword, *fire_phase_one_frame, 0xa8 - 8, 0x80 - 72,
                       16, 16) ||
        !write_frame_ppm("build/zelda1_start_sword_cave_fire_phase1.ppm",
                         *fire_phase_one_frame) ||
        !session.restore(fire_phase_one_state, &error) ||
        session.framebuffer() != *fire_phase_one_frame ||
        !session.restore(cave_entry_state, &error))
      return fail("source cave fire phase was not render/persistence-stable");
    // A source character writes immediately, then Z_07 decrements ObjTimer
    // before subsequent person updates. Persist both fields and reject a bad
    // cursor/fire bytes without mutating the live cave; legacy v7/v8 still
    // migrate into the automatic flow with a canonical standing-fire phase.
    if (!session.tick_cave_dialogue() ||
        session.cave_dialogue_visible_character_count() != 1)
        return fail("source start-cave textbox did not begin automatically");
    const auto text_progress_state = session.serialize();
    if (text_progress_state[4] != 9 || text_progress_state[21] != 1 ||
        text_progress_state[22] != z1::Zelda1StartCaveControl::kTextboxFramesPerGlyph ||
        text_progress_state[23] != 0 || text_progress_state[24] != 5 ||
        !z1::Zelda1OverworldSession::validate_serialized(text_progress_state))
        return fail("Z1OS v9 did not persist textbox/fire timers");
    auto corrupt_text_progress = text_progress_state;
    corrupt_text_progress[21] =
        z1::Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount + 1;
    if (session.restore(corrupt_text_progress, &error) ||
        session.serialize() != text_progress_state)
        return fail("invalid textbox cursor mutated the live session");
    auto corrupt_fire_phase = text_progress_state;
    corrupt_fire_phase[23] = 2;
    if (session.restore(corrupt_fire_phase, &error) ||
        session.serialize() != text_progress_state)
        return fail("invalid fire phase mutated the live session");
    auto corrupt_fire_counter = text_progress_state;
    corrupt_fire_counter[24] = 0;
    if (session.restore(corrupt_fire_counter, &error) ||
        session.serialize() != text_progress_state)
        return fail("invalid fire counter mutated the live session");
    auto legacy_v7_text_pending = text_progress_state;
    legacy_v7_text_pending[4] = 7;
    legacy_v7_text_pending[21] = 0;
    legacy_v7_text_pending[22] = 0;
    if (!z1::Zelda1OverworldSession::validate_serialized(legacy_v7_text_pending) ||
        !session.restore(legacy_v7_text_pending, &error) ||
        session.serialize()[4] != 9 || session.cave_dialogue_visible_character_count() != 0 ||
        session.serialize()[23] != 0 ||
        session.serialize()[24] != z1::Zelda1StartCaveControl::kStandingFireFramesPerPhase)
        return fail("Z1OS v7 cave migration did not retain a valid automatic textbox");
    unsigned text_frames = 0;
    while (!session.cave_dialogue_acknowledged() && text_frames++ != 217)
        if (!session.tick_cave_dialogue())
            return fail("source start-cave textbox stopped advancing");
    if (!session.cave_dialogue_acknowledged() || text_frames != 217 ||
        session.framebuffer() != cave_dialogue_complete)
        return fail("final source textbox marker did not unhalt with text retained");
    minish::foreign_world::InventoryCore inventory;
    if (session.move_cave_by(0, static_cast<std::int16_t>(0x98 - 0xad)) !=
            z1::StartCaveMoveResult::kMoved ||
        session.move_cave_by(0x78 - 0x70, 0) != z1::StartCaveMoveResult::kMoved)
        return fail("cave dialogue acknowledgement or source navigation changed");
    const minish::foreign_world::CrossWorldItemId wooden_sword{
        minish::foreign_world::WorldId::Zelda1, 0x01};
    minish::foreign_world::InventoryCore rejected_inventory;
    const minish::foreign_world::ItemTraits conflicting_traits{
        2, {minish::foreign_world::WorldId::Zelda1, 0x02},
        minish::foreign_world::ItemUseKind::Equip,
        minish::foreign_world::ResourcePoolProvenance::None};
    if (!rejected_inventory.set_item(wooden_sword,
                                     minish::foreign_world::OwnershipFlags::Owned,
                                     minish::foreign_world::Capability::Sword,
                                     conflicting_traits, &error))
        return fail("could not construct atomic-pickup rejection fixture: " + error);
    const auto before_rejected_pickup = session.serialize();
    const auto before_rejected_inventory = rejected_inventory.serialize();
    if (session.try_take_start_sword(&rejected_inventory, &error) !=
            z1::OverworldSessionSwordResult::kInventoryRejected ||
        session.serialize() != before_rejected_pickup ||
        rejected_inventory.serialize() != before_rejected_inventory)
        return fail("rejected cave sword acquisition was not failure-atomic");
    if (
        session.try_take_start_sword(&inventory, &error) !=
            z1::OverworldSessionSwordResult::kAcquired || !session.start_sword_acquired())
        return fail("current-position-only source sword pickup did not commit Zelda1:01");
    if (!minish::foreign_world::has_flag(inventory.ownership(wooden_sword),
                                         minish::foreign_world::OwnershipFlags::Owned) ||
        inventory.loadout_slot(minish::foreign_world::LoadoutId::A, 0) != wooden_sword)
        return fail("starting sword did not atomically acquire and select its origin-qualified fact: " + error);
    const auto sword_use = inventory.resolve_loadout_use(minish::foreign_world::LoadoutId::A, 0);
    if (!sword_use || sword_use->acquired_id != wooden_sword || sword_use->behavior_id != wooden_sword ||
        sword_use->use_kind != minish::foreign_world::ItemUseKind::Equip ||
        sword_use->resource_pool != minish::foreign_world::ResourcePoolProvenance::None)
        return fail("starting sword did not preserve explicit cross-world use semantics");
    // InventoryCore and the Z1OS cave source are independently persisted.  A
    // prior exact Zelda1/01 acquisition must reconcile a reconstructed cave
    // which still displays the source sword: touching it removes that source
    // presentation without replacing or mutating the already selected item.
    z1::Zelda1OverworldSession reconciled_session;
    if (!reconciled_session.load_hash_validated_ines(path,
                                                     z1::Zelda1OverworldSession::kInitialRoom,
                                                   &error) ||
        !reconciled_session.restore(cave_entry_state, &error) ||
        !reconciled_session.acknowledge_cave_dialogue() ||
        reconciled_session.move_cave_by(0, static_cast<std::int16_t>(0x98 - 0xad)) !=
            z1::StartCaveMoveResult::kMoved ||
        reconciled_session.move_cave_by(0x78 - 0x70, 0) !=
            z1::StartCaveMoveResult::kMoved)
        return fail("could not construct persisted-start-sword reconciliation fixture: " + error);
    minish::foreign_world::InventoryCore reconciled_inventory;
    if (!reconciled_inventory.set_item(wooden_sword,
                                       minish::foreign_world::OwnershipFlags::Owned,
                                       minish::foreign_world::Capability::Sword,
                                       {1, wooden_sword,
                                        minish::foreign_world::ItemUseKind::Equip,
                                        minish::foreign_world::ResourcePoolProvenance::None},
                                       &error) ||
        !reconciled_inventory.set_loadout_slot(minish::foreign_world::LoadoutId::A, 2,
                                               wooden_sword, &error))
        return fail("could not seed persisted exact Zelda1/01 inventory: " + error);
    const auto reconciled_inventory_before = reconciled_inventory.serialize();
    if (reconciled_session.try_take_start_sword(&reconciled_inventory, &error) !=
            z1::OverworldSessionSwordResult::kAcquired ||
        !reconciled_session.start_sword_acquired() ||
        reconciled_inventory.serialize() != reconciled_inventory_before ||
        reconciled_inventory.resolve_loadout_use(minish::foreign_world::LoadoutId::A, 2) !=
            std::optional<minish::foreign_world::ResolvedItemUse>{
                {wooden_sword, wooden_sword,
                 minish::foreign_world::ItemUseKind::Equip,
                 minish::foreign_world::ResourcePoolProvenance::None}})
        return fail("persisted exact Zelda1/01 did not reconcile the cave source atomically: " + error);
    // A source cave pickup must never replace an already selected compatible
    // Native sword; ownership of Zelda1/01 remains an independent record.
    z1::Zelda1OverworldSession preserve_session;
    if (!preserve_session.load_hash_validated_ines(path, z1::Zelda1OverworldSession::kInitialRoom, &error) ||
        !preserve_session.restore(cave_entry_state, &error) ||
        !preserve_session.acknowledge_cave_dialogue() ||
        preserve_session.move_cave_by(0, static_cast<std::int16_t>(0x98 - 0xad)) !=
            z1::StartCaveMoveResult::kMoved ||
        preserve_session.move_cave_by(0x78 - 0x70, 0) != z1::StartCaveMoveResult::kMoved)
        return fail("could not restore source cave preserve-selection fixture: " + error);
    minish::foreign_world::InventoryCore preserve_inventory;
    const minish::foreign_world::CrossWorldItemId native_sword{
        minish::foreign_world::WorldId::Native, 1};
    const minish::foreign_world::ItemTraits native_traits{
        1, native_sword, minish::foreign_world::ItemUseKind::Equip,
        minish::foreign_world::ResourcePoolProvenance::None};
    if (!preserve_inventory.set_item(native_sword,
                                     minish::foreign_world::OwnershipFlags::Owned,
                                     minish::foreign_world::Capability::Sword,
                                     native_traits, &error) ||
        !preserve_inventory.set_loadout_slot(minish::foreign_world::LoadoutId::A, 2,
                                             native_sword, &error) ||
        preserve_session.try_take_start_sword(&preserve_inventory, &error) !=
            z1::OverworldSessionSwordResult::kAcquired ||
        preserve_inventory.loadout_slot(minish::foreign_world::LoadoutId::A, 2) != native_sword ||
        preserve_inventory.loadout_slot(minish::foreign_world::LoadoutId::A, 0) ||
        !minish::foreign_world::has_flag(preserve_inventory.ownership(wooden_sword),
                                          minish::foreign_world::OwnershipFlags::Owned))
        return fail("starting sword replaced the selected Native sword: " + error);
    // If A has no compatible sword and no empty slot, the staged Zelda record
    // and source cave state both roll back rather than producing an unusable
    // half-pickup.
    z1::Zelda1OverworldSession full_loadout_session;
    if (!full_loadout_session.load_hash_validated_ines(path, z1::Zelda1OverworldSession::kInitialRoom, &error) ||
        !full_loadout_session.restore(cave_entry_state, &error) ||
        !full_loadout_session.acknowledge_cave_dialogue() ||
        full_loadout_session.move_cave_by(0, static_cast<std::int16_t>(0x98 - 0xad)) !=
            z1::StartCaveMoveResult::kMoved ||
        full_loadout_session.move_cave_by(0x78 - 0x70, 0) != z1::StartCaveMoveResult::kMoved)
        return fail("could not restore full-loadout cave fixture: " + error);
    minish::foreign_world::InventoryCore full_loadout_inventory;
    for (std::size_t slot = 0; slot < minish::foreign_world::kLoadoutSlots; ++slot) {
        const minish::foreign_world::CrossWorldItemId passive{
            minish::foreign_world::WorldId::Native,
            static_cast<std::uint32_t>(0x40 + slot)};
        const minish::foreign_world::ItemTraits passive_traits{
            0, passive, minish::foreign_world::ItemUseKind::Passive,
            minish::foreign_world::ResourcePoolProvenance::None};
        if (!full_loadout_inventory.set_item(passive,
                                              minish::foreign_world::OwnershipFlags::Owned,
                                              minish::foreign_world::Capability::None,
                                              passive_traits, &error) ||
            !full_loadout_inventory.set_loadout_slot(minish::foreign_world::LoadoutId::A,
                                                      slot, passive, &error))
            return fail("could not construct full A loadout fixture: " + error);
    }
    const auto full_loadout_state_before = full_loadout_session.serialize();
    const auto full_loadout_inventory_before = full_loadout_inventory.serialize();
    if (full_loadout_session.try_take_start_sword(&full_loadout_inventory, &error) !=
            z1::OverworldSessionSwordResult::kInventoryRejected ||
        full_loadout_session.serialize() != full_loadout_state_before ||
        full_loadout_inventory.serialize() != full_loadout_inventory_before)
        return fail("full A loadout did not failure-atomically reject start sword selection");
    const auto cave_after_sword = session.framebuffer();
    if (cave_after_sword == cave_before_sword ||
        !write_frame_ppm("build/zelda1_start_sword_cave_after.ppm", cave_after_sword))
        return fail("taken sword did not retain source textbox or change cave sprites");
    const auto acquired_cave_state = session.serialize();
    if (!session.restore(acquired_cave_state, &error) || !session.start_sword_acquired() ||
        session.framebuffer() != cave_after_sword ||
        session.try_take_start_sword(&inventory, &error) !=
            z1::OverworldSessionSwordResult::kAlreadyAcquired)
        return fail("saved starting-sword state did not restore without respawning");
    auto corrupt_cave = acquired_cave_state;
    corrupt_cave[15] = 1;  // entering cannot coexist with a taken sword.
    const auto stable_cave = session.serialize();
    if (session.restore(corrupt_cave, &error) || session.serialize() != stable_cave)
        return fail("corrupt cave control state mutated the live session");
    const auto cave_state = session.serialize();
    if (!z1::Zelda1OverworldSession::validate_serialized(cave_state) ||
        !session.restore(cave_state, &error) || !session.in_cave() ||
        session.cave_index() != 16 ||
        session.move_cave_by(0, static_cast<std::int16_t>(0xdd - 0x98)) !=
            z1::StartCaveMoveResult::kMoved ||
        session.move_cave_one(z1::CaveDirection::kDown) != z1::StartCaveMoveResult::kExited ||
        session.in_cave() || session.position().room_id != 0x77 ||
        session.position().x != 56 || session.position().y != 21 ||
        session.framebuffer() != stable_frame)
        return fail("OW77 source cave return did not preserve the verified terrain frame: " + error);
    // InitMode2 returns Link to ($40,$5D), then the original game walks him
    // away from the mouth. That walk-out coordinate is not itself a warp
    // point: CheckWarps samples its tile at ObjY+$0B. Re-enter only after
    // staging the independently verified ($40,$4D) mouth hotspot.
    if (session.try_enter_cave() != z1::OverworldSessionCaveResult::kNoEntrance)
        return fail("start-cave walk-out coordinate spuriously re-entered the cave");
    auto reentry_hotspot = session.serialize();
    stage_source_position(&reentry_hotspot, 0x40, 0x4d);
    if (!session.restore(reentry_hotspot, &error) ||
        session.try_enter_cave() != z1::OverworldSessionCaveResult::kEntered ||
        session.framebuffer() == cave_after_sword ||
        !same_rectangle(session.framebuffer(), cave_before_sword, 0x48 - 8, 0x80 - 72, 16, 16) ||
        !same_rectangle(session.framebuffer(), cave_before_sword, 0xa8 - 8, 0x80 - 72, 16, 16) ||
        same_rectangle(session.framebuffer(), cave_before_sword, 0x78 - 8, 0x80 - 72, 16, 16) ||
        same_rectangle(session.framebuffer(), cave_before_sword, 0x78 + 4 - 8, 0x98 - 72, 8, 16))
        return fail("re-entered start cave did not retain fires while hiding sword and Old Man");
    if (!session.restore(initial_state, &error) || session.in_cave() ||
        session.framebuffer() != stable_frame)
        return fail("OW session did not restore before edge traversal: " + error);
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kNorth))
        return fail("OW session could not cross OW77 north into OW67");
    const auto north_state = session.serialize();
    const auto north_frame = session.framebuffer();
    if (framebuffer_hash(north_frame) != 11311219386543151198ull || session.position().x != 120 ||
        session.position().y != 149)
        return fail("OW67 frame or opposite-edge entry position changed");
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kWest) || session.position().room_id != 0x66)
        return fail("OW session could not cross OW67 west into OW66");
    const auto room66_frame = session.framebuffer();
    if (!session.octoroks().initialized() || session.position().x != 232 ||
        session.position().y != 85)
        return fail("OW66 frame or opposite-edge entry position changed");
    const auto session66_state = session.serialize();
    if (!session.tick_octoroks())
        return fail("session OW66 Octorok tick failed");
    auto clean_ow66_loader = loader;
    if (!clean_ow66_loader.render_overworld_room(0x66, &error))
        return fail("could not render clean OW66 comparison frame: " + error);
    auto expected_clean_ow66 = clean_ow66_loader.framebuffer();
    session.octoroks().render_qa_markers(&expected_clean_ow66);
    if (session.framebuffer() != expected_clean_ow66)
        return fail("moving OW66 QA actors accumulated trails over source terrain");
    if (!session.restore(session66_state, &error))
        return fail("session could not restore after clean Octorok redraw check: " + error);
    if (!session.hit_octorok(0, 1) || !session.hit_octorok(0, 1) ||
        !session.hit_octorok(0, 1) || !session.octoroks().actors()[0].defeated ||
        !session.octoroks().actors()[0].pending_drop)
        return fail("session fast Octorok did not require three source HP hits");
    const auto defeated66_state = session.serialize();
    auto corrupt_combat = defeated66_state;
    corrupt_combat[14] = 0;
    const auto stable_combat = session.serialize();
    if (session.restore(corrupt_combat, &error) || session.serialize() != stable_combat ||
        !session.restore(defeated66_state, &error) || !session.octoroks().actors()[0].defeated)
        return fail("session OW66 Octorok v4 restore/corruption contract failed");
    // Combat remains resident away from OW66, so a source-host defeated actor
    // cannot respawn on an east/west revisit or a v6 save/restore in between.
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kEast) ||
        session.position().room_id != 0x67 || !session.octoroks().initialized())
        return fail("leaving OW66 discarded retained combat state");
    const auto away66_state = session.serialize();
    if (!session.restore(away66_state, &error) ||
        !cross_at_matching_opening(&session, z1::OwScreenEdge::kWest) ||
        session.position().room_id != 0x66 || !session.octoroks().actors()[0].defeated)
        return fail("OW66 combat did not persist across away/reentry restore");
    if (!session.restore(north_state, &error) || session.position().room_id != 0x67 ||
        session.framebuffer() != north_frame)
        return fail("OW session failed to atomically restore a mid-route room: " + error);
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kSouth) || session.position().room_id != 0x77)
        return fail("OW session could not cross OW67 south into OW77");
    if (framebuffer_hash(session.framebuffer()) != 10743561982003409067ull || session.position().x != 120 ||
        session.position().y != -11)
        return fail("OW77 south-entry frame or position changed");
    if (!session.restore(initial_state, &error) || session.position().room_id != 0x77 ||
        session.framebuffer() != stable_frame ||
        !cross_at_matching_opening(&session, z1::OwScreenEdge::kWest) || session.position().room_id != 0x76)
        return fail("OW session could not restore/cross OW77 west into OW76: " + error);
    const auto west_frame = session.framebuffer();
    if (framebuffer_hash(west_frame) != 14736087833302446610ull || session.position().x != 232 ||
        session.position().y != 85)
        return fail("OW76 frame or opposite-edge entry position changed");
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kEast) || session.position().room_id != 0x77)
        return fail("OW session could not cross OW76 east into OW77");
    if (framebuffer_hash(session.framebuffer()) != 10743561982003409067ull || session.position().x != -8 ||
        session.position().y != 85)
        return fail("OW77 east-entry frame or position changed");
    auto corrupt_session = session.serialize();
    corrupt_session[6] = 0x80;
    const auto stable_session = session.serialize();
    if (session.restore(corrupt_session, &error) || session.serialize() != stable_session)
        return fail("OW session accepted corrupt state or changed on failed restore");
    corrupt_session = stable_session;
    corrupt_session[11] = 2;
    if (session.restore(corrupt_session, &error) || session.serialize() != stable_session)
        return fail("OW session accepted an invalid cave-area enum or changed on failed restore");
    corrupt_session = stable_session;
    corrupt_session[11] = static_cast<std::uint8_t>(z1::OverworldSessionArea::kCave);
    corrupt_session[12] = 0x11;
    if (session.restore(corrupt_session, &error) || session.serialize() != stable_session)
        return fail("OW session accepted a cave index that disagrees with source room facts");
    corrupt_session = stable_session;
    corrupt_session[20] = static_cast<std::uint8_t>(z1::OverworldWalkDirection::kRight);
    if (session.restore(corrupt_session, &error) || session.serialize() != stable_session)
        return fail("OW session accepted a direction without an in-flight grid offset");
    std::cout << "Actual PRG0 iNES live OW77 frame passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (const int result = test_synthetic_rejections(); result != 0) return result;
    if (argc == 1) {
        std::cout << "Zelda 1 live iNES loader shape tests passed without ROM assets\n";
        return 0;
    }
    if (argc != 2) return fail("usage: test_zelda1_live_frame_loader [hash-validated-ines-path]");
    return test_actual_ines_path(argv[1]);
}
