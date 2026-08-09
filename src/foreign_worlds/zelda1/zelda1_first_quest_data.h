#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace minish::foreign_world::zelda1 {

inline constexpr std::size_t kVerifiedPrgSize = 128 * 1024;
inline constexpr std::size_t kOverworldWidth = 16;
inline constexpr std::size_t kOverworldHeight = 8;
inline constexpr std::size_t kRoomCount = kOverworldWidth * kOverworldHeight;
inline constexpr std::size_t kLevelBlockSize = 6 * kRoomCount;
inline constexpr std::size_t kLevelInfoSize = 252;

// These names and offsets deliberately match tools/z1_world_importer exactly.
struct NamedSpanSpec {
    std::string_view name;
    std::size_t prg_offset;
    std::size_t length;
};

struct NamedByteSpan {
    std::string_view name;
    std::size_t prg_offset;
    std::span<const std::uint8_t> bytes;
};

[[nodiscard]] std::span<const NamedSpanSpec> importer_world_schema();

// The byte storage is private and only const spans escape it. This class only
// verifies the 128 KiB size/layout and named-span bounds; the required asset
// importer/cache hash gate establishes the PRG's content identity.
class VerifiedPrg {
public:
    static std::optional<VerifiedPrg> from_verified_bytes(
        std::vector<std::uint8_t> bytes, std::string* error = nullptr);
    static std::optional<VerifiedPrg> from_verified_cache_file(
        const std::filesystem::path& prg_path, std::string* error = nullptr);

    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return bytes_; }
    bool named_span(std::string_view name, NamedByteSpan* output,
                    std::string* error = nullptr) const;

private:
    explicit VerifiedPrg(std::vector<std::uint8_t> bytes)
        : bytes_(std::move(bytes)) {}

    std::vector<std::uint8_t> bytes_;
};

// The six 128-byte attribute planes are label-preserving values. Their byte
// meanings are intentionally not generalized beyond the source labels.
struct RoomAttributeBytes {
    std::uint8_t attr_a;
    std::uint8_t attr_b;
    std::uint8_t attr_c;
    std::uint8_t attr_d;
    std::uint8_t attr_e;
    std::uint8_t attr_f;
};

struct OverworldRoomView {
    std::uint8_t x;
    std::uint8_t y;
    std::uint8_t room_id;
    RoomAttributeBytes attributes;
    // Z_05.asm/LayoutRoomOW masks LevelBlockAttrsD with $3F and selects a
    // 16-byte column-descriptor record from RoomLayoutsOW.
    std::uint8_t layout_reference;
    std::span<const std::uint8_t> layout_columns;
};

struct LevelInfoView {
    std::span<const std::uint8_t> raw;
    std::span<const std::uint8_t> palettes_transfer_buffer;
    std::span<const std::uint8_t> foe_counts;
    std::uint8_t start_y;
    std::span<const std::uint8_t> shortcut_or_item_positions;
    std::uint8_t submenu_map_rotation;
    std::uint8_t status_bar_map_x_offset;
    std::uint8_t start_room_id;
    std::uint8_t triforce_room_id;
    std::uint16_t world_flags_address;
    std::uint8_t level_number;
    std::span<const std::uint8_t> cellar_room_ids;
    std::uint8_t boss_room_id;
    std::span<const std::uint8_t> submenu_map_mask;
    std::span<const std::uint8_t> status_bar_map_transfer_buffer;
    std::span<const std::uint8_t> palette_cycles;
    std::span<const std::uint8_t> death_palette_series;
};

struct FirstQuestUnderworldLevelView {
    std::uint8_t level_number;
    NamedByteSpan level_block;
    NamedByteSpan level_info;
    LevelInfoView info;

    bool room_attributes(std::uint8_t room_id, RoomAttributeBytes* output,
                         std::string* error = nullptr) const;
};

class FirstQuestData {
public:
    static std::optional<FirstQuestData> create(VerifiedPrg prg,
                                                std::string* error = nullptr);

    [[nodiscard]] const VerifiedPrg& prg() const { return prg_; }
    bool overworld_level_info(LevelInfoView* output,
                              std::string* error = nullptr) const;
    bool overworld_room(std::uint8_t x, std::uint8_t y, OverworldRoomView* output,
                        std::string* error = nullptr) const;
    bool first_quest_underworld_level(std::uint8_t level_number,
                                      FirstQuestUnderworldLevelView* output,
                                      std::string* error = nullptr) const;

private:
    explicit FirstQuestData(VerifiedPrg prg) : prg_(std::move(prg)) {}

    VerifiedPrg prg_;
};

}  // namespace minish::foreign_world::zelda1
