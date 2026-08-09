#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"

#include <iostream>
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
    const std::uint8_t room_id = 2 + 3 * z1::kOverworldWidth;
    bytes[kOverworldBlock + room_id] = 0x11;
    bytes[kOverworldBlock + z1::kRoomCount + room_id] = 0x22;
    bytes[kOverworldBlock + 2 * z1::kRoomCount + room_id] = 0x33;
    bytes[kOverworldBlock + 3 * z1::kRoomCount + room_id] = 0xc5;
    bytes[kOverworldBlock + 4 * z1::kRoomCount + room_id] = 0x55;
    bytes[kOverworldBlock + 5 * z1::kRoomCount + room_id] = 0x66;
    for (std::size_t column = 0; column != 16; ++column)
        bytes[kOverworldLayouts + 5 * 16 + column] =
            static_cast<std::uint8_t>(0xa0 + column);

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
    return 0;
}

int test_explicit_prg_path(const char* path) {
    std::string error;
    auto prg = z1::VerifiedPrg::from_verified_cache_file(path, &error);
    if (!prg) return fail("explicit PRG validation failed: " + error);
    auto data = z1::FirstQuestData::create(std::move(*prg), &error);
    if (!data) return fail("explicit First Quest view failed: " + error);
    std::cout << "Explicit verified PRG path accepted\n";
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
