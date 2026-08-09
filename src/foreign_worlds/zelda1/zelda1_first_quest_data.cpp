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
    const std::uint8_t layout_reference = attributes.attr_d & 0x3f;
    constexpr std::size_t kColumnsPerReferencedOverworldLayout = 16;
    const std::size_t layout_offset = layout_reference * kColumnsPerReferencedOverworldLayout;
    if (layout_offset > layouts.bytes.size() ||
        kColumnsPerReferencedOverworldLayout > layouts.bytes.size() - layout_offset) {
        set_error(error, "overworld layout reference exceeds RoomLayoutsOW span");
        return false;
    }
    *output = {x, y, room_id, attributes, layout_reference,
               layouts.bytes.subspan(layout_offset,
                                     kColumnsPerReferencedOverworldLayout)};
    return true;
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

}  // namespace minish::foreign_world::zelda1
