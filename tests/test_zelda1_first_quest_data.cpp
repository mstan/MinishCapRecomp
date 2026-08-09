#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"
#include "foreign_worlds/zelda1/zelda1_world_model.h"
#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"
#include "foreign_worlds/zelda1/zelda1_ow_geometry.h"
#include "foreign_worlds/zelda1/zelda1_uw_renderer.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

namespace z1 = minish::foreign_world::zelda1;

namespace {

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

int test_synthetic_data() {
    std::string error;
    if (z1::VerifiedPrg::from_verified_bytes(std::vector<std::uint8_t>(17), &error))
        return fail("short PRG was accepted");

    std::vector<std::uint8_t> bytes(z1::kVerifiedPrgSize);
    constexpr std::size_t kOverworldBlock = 99328;
    constexpr std::size_t kOverworldLayouts = 87064;
    constexpr std::size_t kUwBlock1 = 100096;
    constexpr std::size_t kUwBlock2 = 100864;
    constexpr std::size_t kOwInfo = 103168;
    constexpr std::size_t kUwInfo1 = 103420;
    constexpr std::size_t kUwInfo7 = 104932;
    constexpr std::size_t kColumnDirectoryOw = 105743;
    constexpr std::size_t kPrimarySquaresOw = 92540;
    constexpr std::size_t kBank5 = 5 * 16 * 1024;
    const std::uint8_t room_id = 2 + 3 * z1::kOverworldWidth;
    bytes[kOverworldBlock + room_id] = 0x11;
    bytes[kOverworldBlock + z1::kRoomCount + room_id] = 0x22;
    bytes[kOverworldBlock + 2 * z1::kRoomCount + room_id] = 0x33;
    // Z_05:LayoutRoomOW masks only AttrD bit 7 while deriving the layout
    // record. Keep this ordinary synthetic layout in the source-backed span.
    bytes[kOverworldBlock + 3 * z1::kRoomCount + room_id] = 0x05;
    bytes[kOverworldBlock + 4 * z1::kRoomCount + room_id] = 0x55;
    bytes[kOverworldBlock + 5 * z1::kRoomCount + room_id] = 0x66;
    for (std::size_t column = 0; column != 16; ++column)
        bytes[kOverworldLayouts + 5 * 16 + column] =
            static_cast<std::uint8_t>(0xa0 + column);
    // AttrD's high bit controls the object-list encoding, not LayoutRoomOW's
    // layout index. These are real First Quest values used by OW58/OW38.
    const std::uint8_t high_bit_room_d3 = 4 + 2 * z1::kOverworldWidth;
    const std::uint8_t high_bit_room_b7 = 5 + 2 * z1::kOverworldWidth;
    bytes[kOverworldBlock + 3 * z1::kRoomCount + high_bit_room_d3] = 0xd3;
    bytes[kOverworldBlock + 3 * z1::kRoomCount + high_bit_room_b7] = 0xb7;
    for (std::size_t column = 0; column != 16; ++column) {
        bytes[kOverworldLayouts + 0x53 * 16 + column] =
            static_cast<std::uint8_t>(0xb0 + column);
        bytes[kOverworldLayouts + 0x37 * 16 + column] =
            static_cast<std::uint8_t>(0xc0 + column);
    }
    // The named RoomLayoutsOW source span has records $00..$78. A malformed
    // synthetic AttrD must not spill into the following cave/underworld data.
    bytes[kOverworldBlock + 3 * z1::kRoomCount] = 0xf9;
    for (std::size_t heap = 0; heap != 16; ++heap) {
        bytes[kColumnDirectoryOw + heap * 2] = 0x00;  // CPU $8000, bank 5.
        bytes[kColumnDirectoryOw + heap * 2 + 1] = 0x80;
    }
    bytes[kPrimarySquaresOw + 0] = 0xa0;
    bytes[kPrimarySquaresOw + 1] = 0xb1;
    // Sixteen high-bit-started columns, each with a repeatable first square.
    for (std::size_t column = 0; column != 16; ++column) {
        const std::size_t start = kBank5 + column * 11;
        bytes[start] = 0xc1;
        for (std::size_t row = 1; row != 11; ++row) bytes[start + row] = 0;
    }

    bytes[kUwBlock1 + 0] = 0x71;
    bytes[kUwBlock1 + z1::kRoomCount] = 0x72;
    bytes[kUwBlock2 + 0] = 0x81;
    bytes[kUwBlock2 + z1::kRoomCount] = 0x82;
    bytes[kUwInfo1 + 0x28] = 0x44;
    bytes[kUwInfo1 + 0x2F] = 0x45;
    bytes[kUwInfo1 + 0x31] = 0x34;
    bytes[kUwInfo1 + 0x32] = 0x12;
    bytes[kUwInfo1 + 0x33] = 1;
    bytes[kUwInfo1 + 0x3E] = 0x46;
    bytes[kUwInfo7 + 0x33] = 7;
    bytes[kOwInfo + 0x28] = 0x47;
    bytes[kOwInfo + 0x2F] = 0x48;
    bytes[kOwInfo + 0x31] = 0x78;
    bytes[kOwInfo + 0x32] = 0x56;

    auto prg = z1::VerifiedPrg::from_verified_bytes(std::move(bytes), &error);
    if (!prg) return fail("synthetic PRG rejected: " + error);
    z1::NamedByteSpan span;
    if (!prg->named_span("world/level_info_underworld_9", &span, &error) ||
        span.prg_offset != 105436 || span.bytes.size() != z1::kLevelInfoSize ||
        prg->named_span("world/not_in_importer", &span, &error))
        return fail("named span lookup was not bounds checked");
    auto data = z1::FirstQuestData::create(std::move(*prg), &error);
    if (!data) return fail("synthetic first-quest data rejected: " + error);

    z1::LevelInfoView overworld_info{};
    if (!data->overworld_level_info(&overworld_info, &error) ||
        overworld_info.raw.size() != z1::kLevelInfoSize ||
        overworld_info.start_y != 0x47 || overworld_info.start_room_id != 0x48 ||
        overworld_info.world_flags_address != 0x5678 ||
        data->overworld_level_info(nullptr, &error))
        return fail("overworld level-info view was not decoded safely");

    z1::OverworldRoomView overworld{};
    if (!data->overworld_room(2, 3, &overworld, &error) ||
        overworld.room_id != room_id || overworld.layout_reference != 5 ||
        overworld.attributes.attr_a != 0x11 || overworld.attributes.attr_f != 0x66 ||
        overworld.layout_columns.size() != 16 || overworld.layout_columns[0] != 0xa0 ||
        overworld.layout_columns[15] != 0xaf ||
        data->overworld_room(16, 0, &overworld, &error))
        return fail("16x8 overworld mapping or layout reference was decoded incorrectly");

    z1::OverworldRoomView high_bit_layout{};
    if (!data->overworld_room(4, 2, &high_bit_layout, &error) ||
        high_bit_layout.layout_reference != 0x53 ||
        high_bit_layout.layout_columns[0] != 0xb0 ||
        high_bit_layout.layout_columns[15] != 0xbf ||
        !data->overworld_room(5, 2, &high_bit_layout, &error) ||
        high_bit_layout.layout_reference != 0x37 ||
        high_bit_layout.layout_columns[0] != 0xc0 ||
        high_bit_layout.layout_columns[15] != 0xcf ||
        data->overworld_room(0, 0, &high_bit_layout, &error))
        return fail("AttrD bit-7 layout masking or RoomLayoutsOW span bounds were incorrect");

    z1::OverworldSquareGrid grid{};
    if (!data->overworld_square_grid(overworld, &grid, &error) ||
        grid.squares.size() != z1::kOverworldSquareWidth * z1::kOverworldSquareHeight ||
        grid.squares[0].column != 0 || grid.squares[0].row != 0 ||
        grid.squares[0].primary_square != 0xb1 ||
        grid.squares[z1::kOverworldSquareWidth].primary_square != 0xb1 ||
        grid.squares[2 * z1::kOverworldSquareWidth].primary_square != 0xa0 ||
        grid.squares.back().column != 15 || grid.squares.back().row != 10 ||
        grid.squares.back().collision != z1::SquareCollision::kOpaqueFinalTile ||
        data->overworld_square_grid(overworld, nullptr, &error))
        return fail("source-faithful overworld column expansion was decoded incorrectly");

    auto entrance = z1::FirstQuestData::decode_overworld_entrance(overworld);
    auto secret = z1::FirstQuestData::decode_overworld_secret_placement(
        overworld, overworld_info);
    auto placement = z1::FirstQuestData::decode_overworld_object_placement(
        overworld, overworld_info);
    if (entrance.kind != z1::OverworldEntranceKind::kLevel ||
        entrance.destination_index != 8 || entrance.return_x != 0x10 ||
        entrance.return_square_row != 6 || placement.object_list_or_template_id != 0x33 ||
        placement.foe_count_index != 0 || placement.foe_count != 0 ||
        placement.spawns_from_screen_edges || secret.position_index != 2 ||
        secret.x != 0 || secret.y != 0)
        return fail("overworld entrance or object placement was decoded incorrectly");

    z1::FirstQuestUnderworldLevelView level{};
    z1::RoomAttributeBytes attributes{};
    if (!data->first_quest_underworld_level(1, &level, &error) ||
        level.level_block.name != "world/level_block_underworld_1_first_quest" ||
        !level.room_attributes(0, &attributes, &error) || attributes.attr_a != 0x71 ||
        attributes.attr_b != 0x72 || level.info.start_y != 0x44 ||
        level.info.start_room_id != 0x45 || level.info.world_flags_address != 0x1234 ||
        level.info.level_number != 1 || level.info.boss_room_id != 0x46 ||
        !data->first_quest_underworld_level(7, &level, &error) ||
        level.level_block.name != "world/level_block_underworld_2_first_quest" ||
        !level.room_attributes(0, &attributes, &error) || attributes.attr_a != 0x81 ||
        level.info.level_number != 7 || level.room_attributes(128, &attributes, &error) ||
        data->first_quest_underworld_level(0, &level, &error))
        return fail("First Quest underworld block/info selection was decoded incorrectly");

    z1::UnderworldRoomView underworld{};
    if (!data->first_quest_underworld_room(1, 0, &underworld, &error) ||
        underworld.layout_reference != 0 ||
        underworld.doors_e_w_s_n[0] != z1::DoorType::kBombable ||
        underworld.room_item_id != 0 || underworld.item_position_index != 0 ||
        underworld.item_x != 0 || underworld.item_y != 0 ||
        underworld.secret_trigger != 0 || underworld.has_push_block ||
        underworld.is_dark || underworld.is_aquamentus ||
        underworld.object_placement.object_list_or_template_id != 0 ||
        data->first_quest_underworld_room(1, 128, &underworld, &error))
        return fail("underworld room structure was decoded incorrectly");
    return 0;
}

int test_explicit_prg_path(const char* path) {
    std::string error;
    auto prg = z1::VerifiedPrg::from_verified_cache_file(path, &error);
    if (!prg) return fail("explicit PRG validation failed: " + error);
    auto data = z1::FirstQuestData::create(std::move(*prg), &error);
    if (!data) return fail("explicit First Quest view failed: " + error);
    z1::LevelInfoView overworld_info{};
    z1::OverworldRoomView level_one_entrance{};
    z1::OverworldRoomView sword_cave{};
    z1::OverworldSquareGrid start_grid{};
    z1::FirstQuestUnderworldLevelView level_one{};
    z1::UnderworldRoomView aquamentus{};
    if (!data->overworld_level_info(&overworld_info, &error) ||
        overworld_info.start_room_id != 0x77 ||
        !data->overworld_room(7, 3, &level_one_entrance, &error) ||
        z1::FirstQuestData::decode_overworld_entrance(level_one_entrance).kind !=
            z1::OverworldEntranceKind::kLevel ||
        z1::FirstQuestData::decode_overworld_entrance(level_one_entrance).destination_index != 1 ||
        !data->overworld_room(7, 7, &sword_cave, &error) ||
        z1::FirstQuestData::decode_overworld_entrance(sword_cave).kind !=
            z1::OverworldEntranceKind::kCave ||
        sword_cave.layout_reference != 0x72 ||
        !data->overworld_square_grid(sword_cave, &start_grid, &error) ||
        !data->first_quest_underworld_level(1, &level_one, &error) ||
        level_one.info.start_room_id != 0x73 || level_one.info.boss_room_id != 0x35 ||
        !data->first_quest_underworld_room(1, level_one.info.boss_room_id,
                                            &aquamentus, &error) ||
        !aquamentus.is_aquamentus || aquamentus.room_item_id != 0x1a ||
        aquamentus.secret_trigger != 7 ||
        aquamentus.doors_e_w_s_n[0] != z1::DoorType::kShutter ||
        aquamentus.doors_e_w_s_n[2] != z1::DoorType::kKey) {
        return fail("explicit PRG did not match stable First Quest vertical-slice facts: " + error);
    }
    z1::Zelda1WorldModel model(*data);
    if (model.position().area != z1::WorldArea::kOverworld ||
        model.position().room_id != 0x77 ||
        model.enter_entrance() != z1::ModelResult::kEntered ||
        model.position().area != z1::WorldArea::kCave ||
        model.qa_take_start_cave_sword() != z1::ModelResult::kEntered ||
        model.leave_cave_or_level() != z1::ModelResult::kReturned ||
        model.position().room_id != 0x77 ||
        model.cross_edge(z1::Edge::kNorth) != z1::ModelResult::kMoved ||
        model.cross_edge(z1::Edge::kNorth) != z1::ModelResult::kMoved ||
        model.cross_edge(z1::Edge::kNorth) != z1::ModelResult::kMoved ||
        model.cross_edge(z1::Edge::kNorth) != z1::ModelResult::kMoved ||
        model.position().room_id != 0x37 ||
        model.enter_entrance() != z1::ModelResult::kEntered ||
        model.position().area != z1::WorldArea::kLevel1 ||
        model.qa_enter_level1_boss_room() != z1::ModelResult::kMoved ||
        model.actors().size() != 1 || !model.actors()[0].hit_points_known ||
        model.contact_actor(0) != z1::ModelResult::kEntered ||
        (model.tick(), model.tick(), model.tick(), model.tick(), model.tick(), model.tick(), model.tick(), model.tick(), false) ||
        model.damage_actor(0, 3) != z1::ModelResult::kEntered ||
        model.actors()[0].hit_points != 3 || model.actors()[0].x != 0xb1 || model.actors()[0].defeated ||
        !model.has_qa_start_sword() || model.serialize().size() != z1::Zelda1WorldModel::kSerializedSize)
        return fail("world model did not preserve the start cave and return portal");
    z1::Zelda1WorldModel restored(*data);
    if (!restored.restore(model.serialize()) || restored.position().area != z1::WorldArea::kLevel1 ||
        restored.position().room_id != 0x35 || restored.actors().size() != 1 ||
        restored.actors()[0].hit_points != 3 || restored.actors()[0].x != 0xb1 || restored.health() != model.health())
        return fail("world model bounded state did not restore at the boss route milestone");
    if (restored.damage_actor(0, 3) != z1::ModelResult::kEntered || !restored.actors()[0].defeated ||
        restored.opaque_drops().size() != 1)
        return fail("world model did not preserve boss defeat/drop state");
    z1::Zelda1WorldModel pending_drop_restore(*data);
    if (!pending_drop_restore.restore(restored.serialize()) || pending_drop_restore.actors().size() != 1 ||
        !pending_drop_restore.actors()[0].defeated || pending_drop_restore.opaque_drops().size() != 1)
        return fail("world model lost pending drops during restore");
    std::vector<std::uint8_t> patterns;
    z1::OwFramebuffer frame{};
    if (!z1::load_verified_overworld_background(*data,
            std::filesystem::path(path).parent_path().parent_path(), &patterns, &error) ||
        !z1::render_overworld_room(*data, 0x77, patterns, &frame))
        return fail("verified overworld pattern renderer failed: " + error);
    std::uint64_t pixel_hash = 1469598103934665603ull;
    for (const auto pixel : frame) { pixel_hash ^= pixel; pixel_hash *= 1099511628211ull; }
    if (pixel_hash != 10743561982003409067ull)
        return fail("overworld renderer framebuffer hash changed: " + std::to_string(pixel_hash));
    std::vector<std::uint8_t> uw_patterns;
    z1::OwFramebuffer level1_start_frame{}, aquamentus_frame{};
    if (!z1::copy_underworld_background_from_verified_prg(*data, &uw_patterns, &error) ||
        !z1::render_underworld_room(*data, 1, 0x73, uw_patterns, &level1_start_frame, &error) ||
        !z1::render_underworld_room(*data, 1, 0x35, uw_patterns, &aquamentus_frame, &error))
        return fail("verified Level 1 static renderer failed: " + error);
    const auto frame_hash = [](const z1::OwFramebuffer& pixels) {
        std::uint64_t hash = 1469598103934665603ull;
        for (const auto pixel : pixels) { hash ^= pixel; hash *= 1099511628211ull; }
        return hash;
    };
    if (frame_hash(level1_start_frame) != 11042205061152487424ull ||
        frame_hash(aquamentus_frame) != 12253611002618560876ull)
        return fail("Level 1 static framebuffer hash changed");
    const auto write_ppm = [](const char* path, const z1::OwFramebuffer& pixels) {
        std::ofstream ppm(path, std::ios::binary);
        if (!ppm) return false;
        ppm << "P6\n240 160\n255\n";
        for (const auto pixel : pixels) {
            const unsigned r = pixel & 31, g = (pixel >> 5) & 31, b = (pixel >> 10) & 31;
            ppm.put(static_cast<char>((r << 3) | (r >> 2)));
            ppm.put(static_cast<char>((g << 3) | (g >> 2)));
            ppm.put(static_cast<char>((b << 3) | (b >> 2)));
        }
        return static_cast<bool>(ppm);
    };
    if (!write_ppm("build/zelda1_l1_73_static_validation.ppm", level1_start_frame) ||
        !write_ppm("build/zelda1_l1_35_static_validation.ppm", aquamentus_frame))
        return fail("could not create Level 1 static renderer QA artifacts");
    z1::OwRoomGeometry start_geometry{};
    if (!z1::build_overworld_room_geometry(*data, 0x77, &start_geometry))
        return fail("source-backed OW77 geometry expansion failed");
    if (start_geometry.cells.size() != z1::kOwRenderWidth * z1::kOwRenderHeight ||
        start_geometry.final_tiles.size() != z1::kOwPlayfieldTileWidth * z1::kOwPlayfieldTileHeight ||
        start_geometry.at(0, 0) != z1::OwGeometryClass::kSolid ||
        start_geometry.final_tile_at(0, 0) != 0xdb ||
        start_geometry.at(120, 80) != z1::OwGeometryClass::kWalkable ||
        start_geometry.final_tile_at(120, 80) != 0x26 ||
        // These are the visible terrain openings at the north/left/right crop
        // edges. CheckScreenEdge separately enforces the source coordinates.
        start_geometry.at(120, 0) != z1::OwGeometryClass::kWalkable ||
        start_geometry.at(0, 80) != z1::OwGeometryClass::kWalkable ||
        start_geometry.at(239, 80) != z1::OwGeometryClass::kWalkable ||
        start_geometry.at(120, 159) != z1::OwGeometryClass::kSolid ||
        start_geometry.final_tile_at(120, 80) !=
            start_geometry.final_tiles[(80 / 8 + 1) * z1::kOwPlayfieldTileWidth + (120 / 8 + 1)])
        return fail("OW77 geometry did not match the renderer crop/final-tile plane");
    z1::OwCaveReturnHotspot sword_cave_return{};
    if (!z1::overworld_cave_return_hotspot(*data, 0x77, &sword_cave_return) ||
        // CheckWarps reads the $24 mouth through GetCollidableTileStill:
        // aligned Obj=($40,$4D) samples its final tile at ObjY+$0B.  The
        // mouth facts are source data, not a renderer-specific overlay.
        !z1::ow_source_still_is_warp_trigger(start_geometry, 0x40, 0x4d) ||
        z1::ow_source_still_final_tile(start_geometry, 0x40, 0x4d) != 0x24 ||
        start_geometry.final_tile_at(56, 16) != 0x24 ||
        start_geometry.at(56, 16) != z1::OwGeometryClass::kWalkable ||
        sword_cave_return.source_x != 0x40 ||
        sword_cave_return.source_target_y != 0x4d ||
        sword_cave_return.source_initial_y != 0x5d ||
        sword_cave_return.viewport_x != 56 || sword_cave_return.viewport_y != 21)
        return fail("OW77 source cave mouth or return hotspot facts changed");
    std::uint8_t neighbor = 0;
    if (z1::ow_player_screen_edge_coordinate(z1::OwScreenEdge::kNorth) != 0x3d ||
        z1::ow_player_screen_edge_coordinate(z1::OwScreenEdge::kSouth) != 0xdd ||
        z1::ow_player_screen_edge_coordinate(z1::OwScreenEdge::kWest) != 0x00 ||
        z1::ow_player_screen_edge_coordinate(z1::OwScreenEdge::kEast) != 0xf0 ||
        !z1::overworld_neighbor(0x77, z1::OwScreenEdge::kNorth, &neighbor) || neighbor != 0x67 ||
        !z1::overworld_neighbor(0x77, z1::OwScreenEdge::kWest, &neighbor) || neighbor != 0x76 ||
        !z1::overworld_neighbor(0x67, z1::OwScreenEdge::kWest, &neighbor) || neighbor != 0x66 ||
        z1::overworld_neighbor(0x77, z1::OwScreenEdge::kSouth, &neighbor) ||
        z1::overworld_neighbor(0x00, z1::OwScreenEdge::kNorth, &neighbor))
        return fail("source overworld lattice did not provide bounded 2x2 topology at OW77");
    {   // Build-only visual QA artifact; the source tree never receives pixels.
        std::ofstream ppm("build/zelda1_ow77_renderer_validation.ppm", std::ios::binary);
        if (!ppm) return fail("could not create OW77 renderer QA artifact");
        ppm << "P6\n240 160\n255\n";
        for (const auto pixel : frame) {
            const unsigned r = pixel & 31, g = (pixel >> 5) & 31, b = (pixel >> 10) & 31;
            ppm.put(static_cast<char>((r << 3) | (r >> 2)));
            ppm.put(static_cast<char>((g << 3) | (g >> 2)));
            ppm.put(static_cast<char>((b << 3) | (b >> 2)));
        }
    }
    auto corrupt = restored.serialize();
    corrupt[z1::Zelda1WorldModel::kSerializedActorCountOffset] = 9;
    const auto unchanged = restored.serialize();
    if (restored.restore(corrupt) || restored.serialize() != unchanged)
        return fail("world model accepted a corrupt actor count or mutated on failure");
    corrupt = unchanged;
    corrupt[z1::Zelda1WorldModel::kSerializedDropCountOffset] = 9;
    if (restored.restore(corrupt) || restored.serialize() != unchanged)
        return fail("world model accepted a corrupt drop count or mutated on failure");
    std::cout << "Explicit verified PRG vertical slice passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) return fail("optional argument is a verified source/prg.bin path");
    if (const int result = test_synthetic_data()) return result;
    if (argc == 2) return test_explicit_prg_path(argv[1]);
    std::cout << "Zelda 1 First Quest typed-data views passed without ROM assets\n";
    return 0;
}
