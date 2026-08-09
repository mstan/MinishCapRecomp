#pragma once

#include "foreign_worlds/foreign_world.h"
#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"
#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace minish::foreign_world::zelda1 {

// This is a deterministic host session for the bounded First Quest Level 1
// vertical slice.  It is deliberately not an NES emulation layer: it decodes
// room records and their door/item state, while a future Minish input adapter
// supplies movement, sword damage, and cross-world inventory acquisition.
enum class Level1Edge : std::uint8_t { kEast, kWest, kSouth, kNorth };
enum class Level1DoorTool : std::uint8_t { kNone, kBomb };
enum class Level1Result : std::uint8_t {
    kEntered, kMoved, kBlocked, kNeedsKey, kNeedsBomb, kSecretUnresolved,
    kCollected, kDefeated, kContact, kReturned, kNoItem, kInvalid,
};

struct Level1RoomItem {
    std::uint8_t source_item_id;
    std::uint8_t x;
    std::uint8_t y;
    bool requires_room_clear;
};

struct Level1BossState {
    bool present;
    bool defeated;
    std::uint8_t source_template_id;
    std::uint8_t hit_points;
    std::uint8_t x;
    std::uint8_t y;
};

class Zelda1Level1Session {
public:
    inline static constexpr std::uint8_t kOverworldEntranceRoom = 0x37;
    inline static constexpr std::uint8_t kStartRoom = 0x73;
    inline static constexpr std::uint8_t kBossRoom = 0x35;
    inline static constexpr std::uint8_t kKeyItemId = 0x19;
    inline static constexpr std::uint8_t kAquamentusTemplate = 0x3d;
    inline static constexpr std::uint8_t kAquamentusHitPoints = 6;
    inline static constexpr std::size_t kSerializedSize = 204;

    explicit Zelda1Level1Session(const FirstQuestData& data);

    // Enters only an OW room whose source AttrB level index is First Quest 1.
    Level1Result enter_from_overworld(std::uint8_t overworld_room_id);
    // Reopens an already-cleared/in-progress Level 1 only after its source
    // start-room south return. This keeps dungeon flags, keys and opened
    // doors resident in the host adapter across an OW round trip.
    Level1Result resume_from_overworld(std::uint8_t overworld_room_id);
    // This is the source start-room south exit, not a generic host escape.
    Level1Result exit_to_overworld(std::uint8_t* out_overworld_room_id);
    // The raw proof token exists for a narrow adapter boundary. Production
    // callers should use the InventoryCore overload so a Bomb capability is
    // resolved from an origin-qualified, cross-world loadout provider.
    Level1Result try_transition(Level1Edge edge,
                                Level1DoorTool tool = Level1DoorTool::kNone);
    Level1Result try_transition(Level1Edge edge,
                                const foreign_world::InventoryCore& inventory,
                                foreign_world::LoadoutId loadout);
    [[nodiscard]] static Level1DoorTool resolve_door_tool(
        const foreign_world::InventoryCore& inventory,
        foreign_world::LoadoutId loadout);
    [[nodiscard]] std::optional<Level1RoomItem> available_room_item() const;
    Level1Result collect_room_item(std::uint8_t* out_source_item_id = nullptr);
    // Damage is supplied by the Minish combat adapter. Aquamentus's six HP is
    // source-derived from ObjectTypeToHpPairs for object type $3D.
    Level1Result sword_hit_boss(std::uint8_t damage);
    // Contact is reported to the adapter; this session does not invent an
    // unproven Minish health conversion for the NES object damage table.
    Level1Result contact_boss() const;

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] std::uint8_t room_id() const { return room_id_; }
    [[nodiscard]] std::uint8_t key_count() const { return keys_; }
    [[nodiscard]] Level1BossState boss() const;
    [[nodiscard]] const OwFramebuffer& framebuffer() const { return frame_; }
    [[nodiscard]] std::span<const std::uint8_t> underworld_patterns() const {
        return patterns_;
    }
    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    // Rejects malformed/cross-ROM-inconsistent input before mutating state.
    bool restore(std::span<const std::uint8_t> bytes);

private:
    static constexpr std::uint8_t kFlagItemTaken = 0x01;
    static constexpr std::uint8_t kFlagBossDefeated = 0x02;
    [[nodiscard]] bool room_view(std::uint8_t id, UnderworldRoomView* out) const;
    [[nodiscard]] bool edge_is_open(std::uint8_t room, Level1Edge edge) const;
    void set_edge_open(std::uint8_t room, Level1Edge edge);
    [[nodiscard]] bool refresh_frame();
    [[nodiscard]] bool validate_state() const;
    [[nodiscard]] bool valid_entry_room(std::uint8_t room) const;
    void copy_dynamic_state_to(Zelda1Level1Session* out) const;
    void commit_dynamic_state_from(const Zelda1Level1Session& source);
    [[nodiscard]] static std::uint8_t opposite(Level1Edge edge);
    [[nodiscard]] static int delta(Level1Edge edge);
    [[nodiscard]] static std::size_t door_bit(std::uint8_t room, Level1Edge edge);

    const FirstQuestData& data_;
    std::vector<std::uint8_t> patterns_;
    OwFramebuffer frame_{};
    std::array<std::uint8_t, 128> room_flags_{};
    std::array<std::uint8_t, 64> opened_door_bits_{};
    bool active_ = false;
    std::uint8_t entry_overworld_room_ = 0;
    std::uint8_t room_id_ = kStartRoom;
    std::uint8_t keys_ = 0;
    std::uint8_t boss_hit_points_ = kAquamentusHitPoints;
    bool boss_defeated_ = false;
};

}  // namespace minish::foreign_world::zelda1
