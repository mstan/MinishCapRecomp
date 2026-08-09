#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace minish::foreign_world {

// Stable values: persisted by InventoryCore's versioned save fragment.
enum class WorldId : std::uint8_t {
    Native = 0,
    Test = 1,
    Zelda1 = 2,
};

struct WorldPosition {
    std::uint32_t room = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;

    constexpr bool operator==(const WorldPosition&) const = default;
};

struct ReturnAnchor {
    WorldId world = WorldId::Native;
    WorldPosition position{};

    constexpr bool operator==(const ReturnAnchor&) const = default;
};

// This seam deliberately names no emulator, renderer, or guest-memory type.
// A future world adapter owns its own implementation and translates at its
// boundary to the game it hosts.
class WorldLifecycle {
public:
    virtual ~WorldLifecycle() = default;
    virtual bool enter(const WorldPosition& entry, std::string* error) = 0;
    virtual bool leave(const ReturnAnchor& destination,
                       std::string* error) = 0;
    virtual void tick() = 0;
};

class WorldRegistry {
public:
    bool register_world(WorldId id, WorldLifecycle& lifecycle,
                        std::string* error = nullptr);

    bool transition_to(WorldId destination, const WorldPosition& entry,
                       const ReturnAnchor& return_anchor,
                       std::string* error = nullptr);
    bool return_to_anchor(std::string* error = nullptr);
    // World adapters publish movement through this value; the registry does
    // not inspect guest memory to derive it.
    void set_current_position(const WorldPosition& position) {
        current_position_ = position;
    }
    void tick();

    [[nodiscard]] WorldId current_world() const { return current_world_; }
    [[nodiscard]] const WorldPosition& current_position() const {
        return current_position_;
    }
    [[nodiscard]] const std::optional<ReturnAnchor>& return_anchor() const {
        return return_anchor_;
    }

private:
    std::map<WorldId, WorldLifecycle*> worlds_;
    WorldId current_world_ = WorldId::Native;
    WorldPosition current_position_{};
    std::optional<ReturnAnchor> return_anchor_;
};

// A ROM-free contract test world. The only legal transitions are A/east -> B
// and B/west -> A, each with fixed landing coordinates.
enum class ProofDoor : std::uint8_t { East, West };

class TwoRoomProofWorld final : public WorldLifecycle {
public:
    static constexpr WorldPosition kRoomAEntry{1, 2, 3};
    static constexpr WorldPosition kRoomBEntry{2, 40, 3};

    bool enter(const WorldPosition& entry, std::string* error) override;
    bool leave(const ReturnAnchor& destination, std::string* error) override;
    void tick() override;

    bool take_door(ProofDoor door, WorldPosition* landing,
                   std::string* error = nullptr);
    [[nodiscard]] const std::optional<WorldPosition>& position() const {
        return position_;
    }
    [[nodiscard]] std::uint32_t ticks() const { return ticks_; }

private:
    std::optional<WorldPosition> position_;
    std::uint32_t ticks_ = 0;
};

struct CrossWorldItemId {
    WorldId origin = WorldId::Native;
    std::uint32_t item = 0;

    constexpr bool operator==(const CrossWorldItemId&) const = default;
    constexpr bool operator<(const CrossWorldItemId& other) const {
        return origin != other.origin ? origin < other.origin : item < other.item;
    }
};

enum class OwnershipFlags : std::uint8_t {
    None = 0,
    Owned = 1 << 0,
    Equipped = 1 << 1,
    Quest = 1 << 2,
};

constexpr OwnershipFlags operator|(OwnershipFlags left, OwnershipFlags right) {
    return static_cast<OwnershipFlags>(static_cast<std::uint8_t>(left) |
                                       static_cast<std::uint8_t>(right));
}
constexpr OwnershipFlags operator&(OwnershipFlags left, OwnershipFlags right) {
    return static_cast<OwnershipFlags>(static_cast<std::uint8_t>(left) &
                                       static_cast<std::uint8_t>(right));
}
constexpr bool has_flag(OwnershipFlags flags, OwnershipFlags test) {
    return (flags & test) == test;
}

enum class Capability : std::uint32_t {
    None = 0,
    Sword = 1 << 0,
    Shield = 1 << 1,
    Bomb = 1 << 2,
    Bow = 1 << 3,
};

constexpr Capability operator|(Capability left, Capability right) {
    return static_cast<Capability>(static_cast<std::uint32_t>(left) |
                                   static_cast<std::uint32_t>(right));
}
constexpr Capability operator&(Capability left, Capability right) {
    return static_cast<Capability>(static_cast<std::uint32_t>(left) &
                                   static_cast<std::uint32_t>(right));
}
constexpr bool has_capability(Capability flags, Capability test) {
    return (flags & test) == test;
}

enum class LoadoutId : std::uint8_t { A = 0, B = 1 };
constexpr std::size_t kLoadoutSlots = 4;

// Stable, saved traits. A tier is an ordinal rather than a category: adapters
// may distinguish any progression tier from 0 through kMaxItemTier.
using ItemTier = std::uint16_t;
constexpr ItemTier kMaxItemTier = 0xfffe;

enum class ItemUseKind : std::uint8_t {
    Passive = 0,
    Equip = 1,
    Consume = 2,
    Quest = 3,
};

enum class ResourcePoolProvenance : std::uint8_t {
    None = 0,
    Native = 1,
    Zelda1 = 2,
};

struct ItemTraits {
    ItemTier tier = 0;
    // This is an explicit behavior key, never inferred from the owned item.
    // It intentionally remains an opaque exact-origin item identity.
    CrossWorldItemId behavior_id{};
    ItemUseKind use_kind = ItemUseKind::Passive;
    ResourcePoolProvenance resource_pool = ResourcePoolProvenance::None;

    constexpr bool operator==(const ItemTraits&) const = default;
};

struct ZeldaResourcePools {
    std::uint16_t health = 0;
    std::uint16_t max_health = 0;
    std::uint16_t rupees = 0;
    std::uint16_t bombs = 0;
    std::uint16_t arrows = 0;
    std::uint16_t keys = 0;

    constexpr bool operator==(const ZeldaResourcePools&) const = default;
};

class InventoryCore {
public:
    bool set_item(CrossWorldItemId id, OwnershipFlags ownership,
                  Capability capabilities, ItemTraits traits,
                  std::string* error = nullptr);
    [[nodiscard]] OwnershipFlags ownership(CrossWorldItemId id) const;
    [[nodiscard]] std::optional<Capability> capabilities(
        CrossWorldItemId id) const;
    [[nodiscard]] std::optional<ItemTraits> traits(CrossWorldItemId id) const;

    bool set_loadout_slot(LoadoutId loadout, std::size_t slot,
                          std::optional<CrossWorldItemId> id,
                          std::string* error = nullptr);
    [[nodiscard]] std::optional<CrossWorldItemId> loadout_slot(
        LoadoutId loadout, std::size_t slot) const;

    // The query returns full identities. No item-number-only query exists,
    // which prevents Native item 7 and Zelda1 item 7 from being conflated.
    [[nodiscard]] std::vector<CrossWorldItemId> capability_providers(
        LoadoutId loadout, Capability capability) const;
    [[nodiscard]] bool has_capability(LoadoutId loadout,
                                      Capability capability) const;

    [[nodiscard]] ZeldaResourcePools& zelda1_resources() {
        return zelda1_resources_;
    }
    [[nodiscard]] const ZeldaResourcePools& zelda1_resources() const {
        return zelda1_resources_;
    }
    [[nodiscard]] ZeldaResourcePools& native_resources() {
        return native_resources_;
    }
    [[nodiscard]] const ZeldaResourcePools& native_resources() const {
        return native_resources_;
    }

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    static bool deserialize(std::span<const std::uint8_t> bytes,
                            InventoryCore* output,
                            std::string* error = nullptr);

private:
    struct ItemRecord {
        OwnershipFlags ownership = OwnershipFlags::None;
        Capability capabilities = Capability::None;
        ItemTraits traits{};
    };

    std::map<CrossWorldItemId, ItemRecord> items_;
    std::array<std::array<std::optional<CrossWorldItemId>, kLoadoutSlots>, 2>
        loadouts_{};
    ZeldaResourcePools native_resources_{};
    ZeldaResourcePools zelda1_resources_{};
};

}  // namespace minish::foreign_world
