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
inline constexpr std::size_t kOverworldSquareWidth = 16;
inline constexpr std::size_t kOverworldSquareHeight = 11;

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
    // Z_05.asm/LayoutRoomOW performs a 16-bit `attr_d * $10` address add;
    // unlike GetUniqueRoomId, it does not mask attr_d to $3F.  This is vital
    // for OW $77, whose source record is $72 (not $32).
    std::uint8_t layout_reference;
    std::span<const std::uint8_t> layout_columns;
};

// This is the exact direction-bit ordering used by FindDoorTypeByDoorBit.
// The numeric values are part of the source data encoding, not host policy.
enum class DoorType : std::uint8_t {
    kOpen = 0,
    kWall = 1,
    kFalseWall = 2,
    kFalseWall2 = 3,
    kBombable = 4,
    kKey = 5,
    kKey2 = 6,
    kShutter = 7,
};

enum class OverworldEntranceKind : std::uint8_t {
    kNone,
    kLevel,
    kCave,
    kShortcut,
    kOpaqueLevelLoad,
};

// A primary square is the first tile selected by LayoutRoomOW for a 16x16
// square. Collision is intentionally not inferred: Zelda's collision checks
// final tiles after secret and tile-object substitutions.
enum class SquareCollision : std::uint8_t { kOpaqueFinalTile };

struct OverworldSquare {
    std::uint8_t column;
    std::uint8_t row;
    std::uint8_t raw_descriptor;
    std::uint8_t primary_square;
    SquareCollision collision = SquareCollision::kOpaqueFinalTile;
};

struct OverworldSquareGrid {
    std::array<OverworldSquare, kOverworldSquareWidth * kOverworldSquareHeight>
        squares;
};

// Normal caves use Z_05.asm:RoomLayoutOWCave0 rather than an OW level-block
// room record.  The raw three values are the exact CaveItemIds source bytes
// copied by Z_01.asm:InitCaveContinue; bit meanings beyond the no-item mask
// stay deliberately undecoded here.
struct OverworldCaveItemsView {
    std::uint8_t source_cave_slot;
    std::array<std::uint8_t, 3> raw_item_ids;
};

struct OverworldEnemySpawn {
    std::uint8_t source_type;
    std::uint8_t source_x;
    std::uint8_t source_y;
};
struct OverworldEnemyRosterView {
    std::uint8_t object_list_id;
    std::uint8_t source_spawn_direction_index;
    std::array<OverworldEnemySpawn, 8> spawns{};
    std::uint8_t count = 0;
};

struct OverworldEntranceView {
    OverworldEntranceKind kind;
    // For kLevel this is 1..9. For kCave/kShortcut it is the source cave
    // index. It is zero for kNone.
    std::uint8_t destination_index;
    // The source uses these only while returning from underground.
    std::uint8_t return_x;
    std::uint8_t return_square_row;
};

// LevelBlockAttrsF chooses one of LevelInfo's four shortcut positions.  The
// bit that says a particular overworld secret is currently visible is runtime
// world-flag state, so it is deliberately not fabricated from the PRG.
struct OverworldSecretPlacementView {
    std::uint8_t position_index;
    std::uint8_t x;
    std::uint8_t y;
};

struct ObjectPlacementView {
    // The source combines AttrC bits 0..5 and AttrD bit 7 into this 7-bit
    // object list/template ID. It is deliberately not named as an enemy.
    std::uint8_t object_list_or_template_id;
    std::uint8_t foe_count_index;
    std::uint8_t foe_count;
    bool spawns_from_screen_edges;
};

struct UnderworldRoomView {
    std::uint8_t room_id;
    RoomAttributeBytes attributes;
    std::uint8_t layout_reference;
    std::array<DoorType, 4> doors_e_w_s_n;
    std::uint8_t room_item_id;
    std::uint8_t item_position_index;
    std::uint8_t item_x;
    std::uint8_t item_y;
    std::uint8_t secret_trigger;
    bool has_push_block;
    bool is_dark;
    // Source InitObject_JumpTable maps template $3d to InitAquamentus. This
    // flag is intentionally restricted to Level 1's declared boss room.
    bool is_aquamentus;
    ObjectPlacementView object_placement;
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
    bool overworld_square_grid(const OverworldRoomView& room,
                               OverworldSquareGrid* output,
                               std::string* error = nullptr) const;
    bool overworld_normal_cave_square_grid(OverworldSquareGrid* output,
                                           std::string* error = nullptr) const;
    bool overworld_cave_items(const OverworldRoomView& room,
                              OverworldCaveItemsView* output,
                              std::string* error = nullptr) const;
    bool overworld_enemy_roster(const OverworldRoomView& room,
                                std::uint8_t source_spawn_direction_index,
                                OverworldEnemyRosterView* output,
                                std::string* error = nullptr) const;
    [[nodiscard]] static OverworldEntranceView decode_overworld_entrance(
        const OverworldRoomView& room);
    [[nodiscard]] static OverworldSecretPlacementView
    decode_overworld_secret_placement(const OverworldRoomView& room,
                                      const LevelInfoView& level_info);
    [[nodiscard]] static ObjectPlacementView decode_overworld_object_placement(
        const OverworldRoomView& room, const LevelInfoView& level_info);
    bool first_quest_underworld_level(std::uint8_t level_number,
                                      FirstQuestUnderworldLevelView* output,
                                      std::string* error = nullptr) const;
    bool first_quest_underworld_room(std::uint8_t level_number,
                                     std::uint8_t room_id,
                                     UnderworldRoomView* output,
                                     std::string* error = nullptr) const;

private:
    explicit FirstQuestData(VerifiedPrg prg) : prg_(std::move(prg)) {}

    VerifiedPrg prg_;
};

}  // namespace minish::foreign_world::zelda1
