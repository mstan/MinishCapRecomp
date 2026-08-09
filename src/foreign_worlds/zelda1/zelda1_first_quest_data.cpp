#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"

#include <fstream>
#include <iterator>

namespace minish::foreign_world::zelda1 {
namespace {

constexpr std::array<NamedSpanSpec, 14> kImporterWorldSchema{{
    {"source/prg", 0, kVerifiedPrgSize},
    {"world/room_layouts_overworld", 87064, 1936},
    {"world/room_layouts_underworld", 90334, 504},
    {"world/level_block_overworld", 99328, kLevelBlockSize},
    {"world/level_block_underworld_1_first_quest", 100096, kLevelBlockSize},
    {"world/level_block_underworld_2_first_quest", 100864, kLevelBlockSize},
    {"world/level_block_underworld_1_second_quest", 101632, kLevelBlockSize},
    {"world/level_block_underworld_2_second_quest", 102400, kLevelBlockSize},
    {"world/level_info_overworld", 103168, kLevelInfoSize},
    {"world/level_info_underworld_1", 103420, kLevelInfoSize},
    {"world/level_info_underworld_2", 103672, kLevelInfoSize},
    {"world/level_info_underworld_3", 103924, kLevelInfoSize},
    {"world/level_info_underworld_4", 104176, kLevelInfoSize},
    {"world/level_info_underworld_5", 104428, kLevelInfoSize},
    // Levels 6 through 9 are contiguous at 252-byte intervals; they are
    // added below by named_span's full schema lookup table.
}};

constexpr std::array<NamedSpanSpec, 4> kImporterWorldSchemaTail{{
    {"world/level_info_underworld_6", 104680, kLevelInfoSize},
    {"world/level_info_underworld_7", 104932, kLevelInfoSize},
    {"world/level_info_underworld_8", 105184, kLevelInfoSize},
    {"world/level_info_underworld_9", 105436, kLevelInfoSize},
}};

// These are load-file offsets, not CPU addresses.  At the pinned source
// revision, Z_06.asm:ColumnDirectoryOW is copied from PRG offset $19D0F to
// RAM, and its pointers address the bank-5 ColumnHeapOW0..F data.  Keeping
// this conversion here means no copyrighted table bytes are compiled in.
constexpr std::size_t kColumnDirectoryOwPrgOffset = 105743;
constexpr std::size_t kPrimarySquaresOwPrgOffset = 92540;
constexpr std::size_t kPrimarySquaresOwCount = 56;
constexpr std::size_t kPrgBank5Offset = 5 * 16 * 1024;
constexpr std::uint16_t kSwitchableBankCpuStart = 0x8000;
constexpr std::uint16_t kSwitchableBankCpuEnd = 0xc000;

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

const NamedSpanSpec* find_spec(std::string_view name) {
    for (const auto& spec : kImporterWorldSchema)
        if (spec.name == name) return &spec;
    for (const auto& spec : kImporterWorldSchemaTail)
        if (spec.name == name) return &spec;
    return nullptr;
}

RoomAttributeBytes attributes_for(std::span<const std::uint8_t> block,
                                  std::uint8_t room_id) {
    return {block[room_id], block[kRoomCount + room_id],
            block[2 * kRoomCount + room_id], block[3 * kRoomCount + room_id],
            block[4 * kRoomCount + room_id], block[5 * kRoomCount + room_id]};
}

ObjectPlacementView decode_object_placement(const RoomAttributeBytes& attributes,
                                            const LevelInfoView& level_info,
                                            bool overworld) {
    const std::uint8_t count_index = attributes.attr_c >> 6;
    return {
        static_cast<std::uint8_t>((attributes.attr_c & 0x3f) |
                                  ((attributes.attr_d & 0x80) ? 0x40 : 0)),
        count_index,
        level_info.foe_counts[count_index],
        overworld && (attributes.attr_f & 0x08) != 0,
    };
}

std::array<DoorType, 4> decode_doors(const RoomAttributeBytes& attributes) {
    return {
        static_cast<DoorType>((attributes.attr_b >> 2) & 0x07),
        static_cast<DoorType>((attributes.attr_b >> 5) & 0x07),
        static_cast<DoorType>((attributes.attr_a >> 2) & 0x07),
        static_cast<DoorType>((attributes.attr_a >> 5) & 0x07),
    };
}

LevelInfoView decode_level_info(std::span<const std::uint8_t> raw) {
    // Offsets are the label addresses in Variables.inc minus $6B7E.
    return {
        raw,
        raw.subspan(0x00, 0x24),  // LevelInfo_PalettesTransferBuf
        raw.subspan(0x24, 0x04),  // LevelInfo_FoeCounts
        raw[0x28],                // LevelInfo_StartY
        raw.subspan(0x29, 0x04),  // LevelInfo_ShortcutOrItemPosArray
        raw[0x2D],                // LevelInfo_SubmenuMapRotation
        raw[0x2E],                // LevelInfo_StatusBarMapXOffset
        raw[0x2F],                // LevelInfo_StartRoomId
        raw[0x30],                // LevelInfo_TriforceRoomId
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(raw[0x31]) |
            (static_cast<std::uint16_t>(raw[0x32]) << 8)), // LevelInfo_WorldFlagsAddr
        raw[0x33],                // LevelInfo_LevelNumber
        raw.subspan(0x34, 0x0A),  // LevelInfo_CellarRoomIdArray
        raw[0x3E],                // LevelInfo_BossRoomId
        raw.subspan(0x3F, 0x10),  // LevelInfo_SubmenuMapMask
        raw.subspan(0x4F, 0x2D),  // LevelInfo_StatusBarMapTransferBuf
        raw.subspan(0x7C, 0x60),  // LevelInfo_PaletteCycles
        raw.subspan(0xDC, 0x20),  // LevelInfo_DeathPaletteSeries
    };
}

bool required_span(const VerifiedPrg& prg, std::string_view name,
                   std::string* error) {
    NamedByteSpan ignored;
    return prg.named_span(name, &ignored, error);
}

bool expand_overworld_columns(std::span<const std::uint8_t> layout_columns,
                              std::span<const std::uint8_t> bytes,
                              OverworldSquareGrid* output,
                              std::string* error) {
    if (!output) { set_error(error, "overworld square-grid output is null"); return false; }
    if (layout_columns.size() != kOverworldSquareWidth) {
        set_error(error, "overworld layout does not carry 16 columns"); return false;
    }
    if (kColumnDirectoryOwPrgOffset + 32 > bytes.size() ||
        kPrimarySquaresOwPrgOffset + kPrimarySquaresOwCount > bytes.size()) {
        set_error(error, "verified PRG is missing pinned overworld render tables"); return false;
    }
    for (std::size_t column = 0; column != kOverworldSquareWidth; ++column) {
        const std::uint8_t column_descriptor = layout_columns[column];
        const std::size_t directory_offset = kColumnDirectoryOwPrgOffset + ((column_descriptor >> 4) * 2);
        const std::uint16_t cpu_pointer = static_cast<std::uint16_t>(
            bytes[directory_offset] | (static_cast<std::uint16_t>(bytes[directory_offset + 1]) << 8));
        if (cpu_pointer < kSwitchableBankCpuStart || cpu_pointer >= kSwitchableBankCpuEnd) {
            set_error(error, "overworld column directory points outside bank 5"); return false;
        }
        std::size_t heap_offset = kPrgBank5Offset + (cpu_pointer - kSwitchableBankCpuStart);
        if (heap_offset >= bytes.size()) { set_error(error, "overworld column heap pointer exceeds verified PRG"); return false; }
        std::uint8_t requested_column = column_descriptor & 0x0f;
        while (true) {
            if (heap_offset >= bytes.size()) { set_error(error, "overworld column heap has no requested column"); return false; }
            if ((bytes[heap_offset] & 0x80) == 0) { ++heap_offset; continue; }
            if (requested_column-- == 0) break;
            ++heap_offset;
        }
        bool repeat_pending = false;
        for (std::size_t row = 0; row != kOverworldSquareHeight; ++row) {
            if (heap_offset >= bytes.size()) { set_error(error, "overworld column heap ends within a room column"); return false; }
            const std::uint8_t square_descriptor = bytes[heap_offset];
            const std::uint8_t square_index = square_descriptor & 0x3f;
            if (square_index >= kPrimarySquaresOwCount) { set_error(error, "overworld square index is outside PrimarySquaresOW"); return false; }
            output->squares[row * kOverworldSquareWidth + column] = {
                static_cast<std::uint8_t>(column), static_cast<std::uint8_t>(row), square_descriptor,
                bytes[kPrimarySquaresOwPrgOffset + square_index]};
            if ((square_descriptor & 0x40) == 0 || repeat_pending) { ++heap_offset; repeat_pending = false; }
            else repeat_pending = true;
        }
    }
    return true;
}

}  // namespace

std::span<const NamedSpanSpec> importer_world_schema() {
    // The two adjacent arrays make static initialization straightforward while
    // keeping the source ordering identical to the Python importer.
    static const std::array<NamedSpanSpec, 18> combined = [] {
        std::array<NamedSpanSpec, 18> result{};
        std::size_t index = 0;
        for (const auto& spec : kImporterWorldSchema) result[index++] = spec;
        for (const auto& spec : kImporterWorldSchemaTail) result[index++] = spec;
        return result;
    }();
    return combined;
}

std::optional<VerifiedPrg> VerifiedPrg::from_verified_bytes(
    std::vector<std::uint8_t> bytes, std::string* error) {
    if (bytes.size() != kVerifiedPrgSize) {
        set_error(error, "verified Zelda 1 PRG must be exactly 131072 bytes");
        return std::nullopt;
    }
    return VerifiedPrg(std::move(bytes));
}

std::optional<VerifiedPrg> VerifiedPrg::from_verified_cache_file(
    const std::filesystem::path& prg_path, std::string* error) {
    std::ifstream input(prg_path, std::ios::binary);
    if (!input) {
        set_error(error, "could not open verified Zelda 1 PRG cache file: " +
                             prg_path.string());
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    return from_verified_bytes(std::move(bytes), error);
}

bool VerifiedPrg::named_span(std::string_view name, NamedByteSpan* output,
                             std::string* error) const {
    if (!output) {
        set_error(error, "named span output is null");
        return false;
    }
    const NamedSpanSpec* spec = find_spec(name);
    if (!spec) {
        set_error(error, "unknown Zelda 1 importer span: " + std::string(name));
        return false;
    }
    if (spec->prg_offset > bytes_.size() ||
        spec->length > bytes_.size() - spec->prg_offset) {
        set_error(error, "Zelda 1 importer span exceeds verified PRG: " +
                             std::string(name));
        return false;
    }
    *output = {spec->name, spec->prg_offset,
               std::span<const std::uint8_t>(bytes_).subspan(spec->prg_offset,
                                                              spec->length)};
    return true;
}

bool FirstQuestUnderworldLevelView::room_attributes(
    std::uint8_t room_id, RoomAttributeBytes* output, std::string* error) const {
    if (!output) {
        set_error(error, "underworld room-attribute output is null");
        return false;
    }
    if (room_id >= kRoomCount) {
        set_error(error, "underworld room id is outside the 128-room level block");
        return false;
    }
    *output = attributes_for(level_block.bytes, room_id);
    return true;
}

std::optional<FirstQuestData> FirstQuestData::create(VerifiedPrg prg,
                                                      std::string* error) {
    for (const auto& spec : importer_world_schema()) {
        if (!required_span(prg, spec.name, error)) return std::nullopt;
    }
    return FirstQuestData(std::move(prg));
}

bool FirstQuestData::overworld_level_info(LevelInfoView* output,
                                          std::string* error) const {
    if (!output) {
        set_error(error, "overworld level-info output is null");
        return false;
    }
    NamedByteSpan info;
    if (!prg_.named_span("world/level_info_overworld", &info, error)) return false;
    if (info.bytes.size() != kLevelInfoSize) {
        set_error(error, "overworld level-info span has an unexpected size");
        return false;
    }
    *output = decode_level_info(info.bytes);
    return true;
}

bool FirstQuestData::overworld_room(std::uint8_t x, std::uint8_t y,
                                    OverworldRoomView* output,
                                    std::string* error) const {
    if (!output) {
        set_error(error, "overworld room output is null");
        return false;
    }
    if (x >= kOverworldWidth || y >= kOverworldHeight) {
        set_error(error, "overworld coordinates are outside the 16x8 room map");
        return false;
    }
    NamedByteSpan block, layouts;
    if (!prg_.named_span("world/level_block_overworld", &block, error) ||
        !prg_.named_span("world/room_layouts_overworld", &layouts, error)) {
        return false;
    }
    const std::uint8_t room_id = static_cast<std::uint8_t>(y * kOverworldWidth + x);
    const RoomAttributeBytes attributes = attributes_for(block.bytes, room_id);
    // Source-faithful: LayoutRoomOW (Z_05.asm) shifts the complete byte four
    // times and adds it to RoomLayoutsOWAddr.  GetUniqueRoomId masks $3F,
    // but that is a distinct helper; using it here incorrectly selects $32
    // for OW $77 instead of its actual layout record $72.
    const std::uint8_t layout_reference = attributes.attr_d;
    constexpr std::size_t kColumnsPerReferencedOverworldLayout = 16;
    const std::size_t layout_offset = layout_reference * kColumnsPerReferencedOverworldLayout;
    // LayoutRoomOW performs no data-span check after this address add. Some
    // valid OW references continue into following source-mapped PRG data;
    // validate that exact computed source address, rather than guessing a
    // six-bit mask or rejecting the original routine's address calculation.
    const std::size_t layout_prg_offset = layouts.prg_offset + layout_offset;
    if (layout_prg_offset > prg_.bytes().size() ||
        kColumnsPerReferencedOverworldLayout > prg_.bytes().size() - layout_prg_offset) {
        set_error(error, "overworld layout reference exceeds verified PRG");
        return false;
    }
    *output = {x, y, room_id, attributes, layout_reference,
               prg_.bytes().subspan(layout_prg_offset, kColumnsPerReferencedOverworldLayout)};
    return true;
}

bool FirstQuestData::overworld_square_grid(const OverworldRoomView& room,
                                           OverworldSquareGrid* output,
                                           std::string* error) const {
    return expand_overworld_columns(room.layout_columns, prg_.bytes(), output, error);
}

bool FirstQuestData::overworld_normal_cave_square_grid(OverworldSquareGrid* output,
                                                        std::string* error) const {
    // Z_05.asm:RoomLayoutOWCave0 immediately follows the 121 $10-byte OW
    // records at the named RoomLayoutsOW span. It is 16 column descriptors.
    constexpr std::size_t kNormalCaveLayoutPrgOffset = 89000;
    const auto bytes = prg_.bytes();
    if (kNormalCaveLayoutPrgOffset + kOverworldSquareWidth > bytes.size()) {
        set_error(error, "verified PRG lacks RoomLayoutOWCave0"); return false;
    }
    return expand_overworld_columns(bytes.subspan(kNormalCaveLayoutPrgOffset,
                                                   kOverworldSquareWidth),
                                   bytes, output, error);
}

bool FirstQuestData::overworld_cave_items(const OverworldRoomView& room,
                                          OverworldCaveItemsView* output,
                                          std::string* error) const {
    if (!output) { set_error(error, "overworld cave item output is null"); return false; }
    const auto entrance = decode_overworld_entrance(room);
    if (entrance.kind != OverworldEntranceKind::kCave || entrance.destination_index < 16) {
        set_error(error, "overworld room does not use a normal cave item record"); return false;
    }
    NamedByteSpan block;
    if (!prg_.named_span("world/level_block_overworld", &block, error)) return false;
    const std::size_t cave_slot = entrance.destination_index - 16;
    constexpr std::size_t kAttrEPlaneOffset = 4 * kRoomCount;
    const std::size_t item_offset = kAttrEPlaneOffset + cave_slot * 3;
    if (item_offset + 3 > block.bytes.size()) {
        set_error(error, "source cave item record exceeds LevelBlockAttrsE"); return false;
    }
    *output = {static_cast<std::uint8_t>(cave_slot), {block.bytes[item_offset],
                                                       block.bytes[item_offset + 1],
                                                       block.bytes[item_offset + 2]}};
    return true;
}

bool FirstQuestData::overworld_enemy_roster(const OverworldRoomView& room,
                                            std::uint8_t source_spawn_direction_index,
                                            OverworldEnemyRosterView* output,
                                            std::string* error) const {
    if (!output || source_spawn_direction_index >= 4) {
        set_error(error, "enemy roster output/direction is invalid"); return false;
    }
    LevelInfoView info{};
    if (!overworld_level_info(&info, error)) return false;
    const auto placement = decode_overworld_object_placement(room, info);
    // Bounded, exact decoder: ObjLists $69 is the $66 red-Octorok list.
    // Other ObjLists stay opaque until their source list boundaries are named.
    if (placement.object_list_or_template_id != 0x69 || placement.foe_count != 4) {
        set_error(error, "overworld object roster is not the decoded red-Octorok list"); return false;
    }
    constexpr std::size_t kObjListsPrgOffset = 83574;
    constexpr std::size_t kObjList69Offset = 36; // pinned ObjListAddrs.inc
    constexpr std::size_t kSpawnPosListsPrgOffset = 83534;
    const auto bytes = prg_.bytes();
    if (kObjListsPrgOffset + kObjList69Offset + 4 > bytes.size() ||
        kSpawnPosListsPrgOffset + (source_spawn_direction_index + 1) * 9 > bytes.size()) {
        set_error(error, "verified PRG lacks red-Octorok roster/spawn data"); return false;
    }
    OverworldEnemyRosterView view{};
    view.object_list_id = 0x69;
    view.source_spawn_direction_index = source_spawn_direction_index;
    view.count = 4;
    for (unsigned i = 0; i < view.count; ++i) {
        const auto cell = bytes[kSpawnPosListsPrgOffset + source_spawn_direction_index * 9 + i];
        view.spawns[i] = {bytes[kObjListsPrgOffset + kObjList69Offset + i],
                          static_cast<std::uint8_t>((cell & 0x0f) << 4),
                          static_cast<std::uint8_t>((cell & 0xf0) | 0x0d)};
    }
    *output = view;
    return true;
}

OverworldEntranceView FirstQuestData::decode_overworld_entrance(
    const OverworldRoomView& room) {
    const std::uint8_t index = room.attributes.attr_b >> 2;
    OverworldEntranceKind kind = OverworldEntranceKind::kNone;
    if (index >= 1 && index <= 9) {
        kind = OverworldEntranceKind::kLevel;
    } else if (index < 16) {
        // HandleWarpOW uses the level-loading path for these values too, but
        // the supplied First Quest data only gives semantic level names 1..9.
        kind = index == 0 ? OverworldEntranceKind::kNone
                          : OverworldEntranceKind::kOpaqueLevelLoad;
    } else if (index == 20) {
        kind = OverworldEntranceKind::kShortcut;
    } else {
        kind = OverworldEntranceKind::kCave;
    }
    return {kind, index, static_cast<std::uint8_t>(room.attributes.attr_a & 0xf0),
            static_cast<std::uint8_t>(room.attributes.attr_f & 0x07)};
}

OverworldSecretPlacementView FirstQuestData::decode_overworld_secret_placement(
    const OverworldRoomView& room, const LevelInfoView& level_info) {
    const std::uint8_t position_index = (room.attributes.attr_f >> 4) & 0x03;
    const std::uint8_t position = level_info.shortcut_or_item_positions[position_index];
    return {position_index, static_cast<std::uint8_t>(position & 0xf0),
            static_cast<std::uint8_t>((position & 0x0f) << 4)};
}

ObjectPlacementView FirstQuestData::decode_overworld_object_placement(
    const OverworldRoomView& room, const LevelInfoView& level_info) {
    return decode_object_placement(room.attributes, level_info, true);
}

bool FirstQuestData::first_quest_underworld_level(
    std::uint8_t level_number, FirstQuestUnderworldLevelView* output,
    std::string* error) const {
    if (!output) {
        set_error(error, "underworld level output is null");
        return false;
    }
    if (level_number == 0 || level_number > 9) {
        set_error(error, "First Quest underworld level must be in 1..9");
        return false;
    }
    // Z_06.asm/LevelBlockAddrsQ1: levels 1..6 use UW1Q1; 7..9 use UW2Q1.
    const std::string_view block_name = level_number <= 6
        ? "world/level_block_underworld_1_first_quest"
        : "world/level_block_underworld_2_first_quest";
    const std::string info_name = "world/level_info_underworld_" +
        std::to_string(level_number);
    NamedByteSpan block, info;
    if (!prg_.named_span(block_name, &block, error) ||
        !prg_.named_span(info_name, &info, error)) {
        return false;
    }
    *output = {level_number, block, info, decode_level_info(info.bytes)};
    return true;
}

bool FirstQuestData::first_quest_underworld_room(
    std::uint8_t level_number, std::uint8_t room_id, UnderworldRoomView* output,
    std::string* error) const {
    if (!output) {
        set_error(error, "underworld room output is null");
        return false;
    }
    FirstQuestUnderworldLevelView level{};
    if (!first_quest_underworld_level(level_number, &level, error)) return false;
    RoomAttributeBytes attributes{};
    if (!level.room_attributes(room_id, &attributes, error)) return false;
    const std::uint8_t position_index = (attributes.attr_f >> 4) & 0x03;
    const std::uint8_t position = level.info.shortcut_or_item_positions[position_index];
    *output = {
        room_id,
        attributes,
        static_cast<std::uint8_t>(attributes.attr_d & 0x3f),
        decode_doors(attributes),
        static_cast<std::uint8_t>(attributes.attr_e & 0x1f),
        position_index,
        static_cast<std::uint8_t>(position & 0xf0),
        static_cast<std::uint8_t>((position & 0x0f) << 4),
        static_cast<std::uint8_t>(attributes.attr_f & 0x07),
        (attributes.attr_d & 0x40) != 0,
        (attributes.attr_e & 0x80) != 0,
        level_number == 1 && room_id == level.info.boss_room_id &&
            (attributes.attr_c & 0x3f) == 0x3d && (attributes.attr_d & 0x80) == 0,
        decode_object_placement(attributes, level.info, false),
    };
    return true;
}

}  // namespace minish::foreign_world::zelda1
