#include "foreign_worlds/foreign_world.h"

#include <algorithm>
#include <limits>

namespace minish::foreign_world {
namespace {

constexpr std::array<std::uint8_t, 4> kInventoryMagic{'F', 'W', 'I', 'V'};
constexpr std::uint16_t kInventoryVersion = 3;
constexpr std::size_t kMaxItemRecords = 1024;

void set_error(std::string* error, const char* message) {
    if (error) *error = message;
}

bool valid_world(WorldId id) {
    return id == WorldId::Native || id == WorldId::Test || id == WorldId::Zelda1;
}

bool valid_loadout(LoadoutId id) {
    return id == LoadoutId::A || id == LoadoutId::B;
}

std::size_t loadout_index(LoadoutId id) {
    return static_cast<std::size_t>(id);
}

bool valid_capabilities(std::uint32_t capabilities) {
    constexpr auto kKnown = static_cast<std::uint32_t>(Capability::Sword) |
                            static_cast<std::uint32_t>(Capability::Shield) |
                            static_cast<std::uint32_t>(Capability::Bomb) |
                            static_cast<std::uint32_t>(Capability::Bow);
    return (capabilities & ~kKnown) == 0;
}

bool valid_traits(const ItemTraits& traits) {
    const bool valid_use_kind = traits.use_kind == ItemUseKind::Passive ||
                                traits.use_kind == ItemUseKind::Equip ||
                                traits.use_kind == ItemUseKind::Consume ||
                                traits.use_kind == ItemUseKind::Quest;
    const bool valid_pool = traits.resource_pool == ResourcePoolProvenance::None ||
                            traits.resource_pool == ResourcePoolProvenance::Native ||
                            traits.resource_pool == ResourcePoolProvenance::Zelda1;
    return traits.tier <= kMaxItemTier && valid_world(traits.behavior_id.origin) &&
           valid_use_kind && valid_pool;
}

void append_u16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
    bytes->push_back(static_cast<std::uint8_t>(value));
    bytes->push_back(static_cast<std::uint8_t>(value >> 8));
}
void append_u32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes->push_back(static_cast<std::uint8_t>(value >> shift));
}
void append_resources(std::vector<std::uint8_t>* bytes,
                      const ZeldaResourcePools& pools) {
    append_u16(bytes, pools.health);
    append_u16(bytes, pools.max_health);
    append_u16(bytes, pools.rupees);
    append_u16(bytes, pools.bombs);
    append_u16(bytes, pools.arrows);
    append_u16(bytes, pools.keys);
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    bool u8(std::uint8_t* value) {
        if (offset_ == bytes_.size()) return false;
        *value = bytes_[offset_++];
        return true;
    }
    bool u16(std::uint16_t* value) {
        std::uint8_t low, high;
        if (!u8(&low) || !u8(&high)) return false;
        *value = static_cast<std::uint16_t>(low) |
                 (static_cast<std::uint16_t>(high) << 8);
        return true;
    }
    bool u32(std::uint32_t* value) {
        *value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8) {
            std::uint8_t byte;
            if (!u8(&byte)) return false;
            *value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    [[nodiscard]] bool at_end() const { return offset_ == bytes_.size(); }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

bool read_resources(Reader* reader, ZeldaResourcePools* pools) {
    return reader->u16(&pools->health) && reader->u16(&pools->max_health) &&
           reader->u16(&pools->rupees) && reader->u16(&pools->bombs) &&
           reader->u16(&pools->arrows) && reader->u16(&pools->keys);
}

bool valid_ownership(std::uint8_t flags) {
    constexpr auto kKnown = static_cast<std::uint8_t>(OwnershipFlags::Owned) |
                            static_cast<std::uint8_t>(OwnershipFlags::Equipped) |
                            static_cast<std::uint8_t>(OwnershipFlags::Quest);
    return (flags & ~kKnown) == 0;
}

}  // namespace

bool WorldRegistry::register_world(WorldId id, WorldLifecycle& lifecycle,
                                   std::string* error) {
    if (!valid_world(id)) {
        set_error(error, "unknown world id");
        return false;
    }
    if (worlds_.contains(id)) {
        set_error(error, "world id already registered");
        return false;
    }
    worlds_.emplace(id, &lifecycle);
    return true;
}

bool WorldRegistry::transition_to(WorldId destination, const WorldPosition& entry,
                                  const ReturnAnchor& return_anchor,
                                  std::string* error) {
    const auto next = worlds_.find(destination);
    if (next == worlds_.end()) {
        set_error(error, "destination world is not registered");
        return false;
    }
    if (!valid_world(return_anchor.world)) {
        set_error(error, "return anchor has an unknown world id");
        return false;
    }
    const auto current = worlds_.find(current_world_);
    if (current != worlds_.end() && !current->second->leave(return_anchor, error)) {
        return false;
    }
    std::string destination_error;
    if (!next->second->enter(entry, &destination_error)) {
        if (destination_error.empty()) destination_error = "no detail supplied";
        if (current != worlds_.end()) {
            std::string rollback_error;
            if (!current->second->enter(current_position_, &rollback_error)) {
                if (rollback_error.empty()) rollback_error = "no detail supplied";
                if (error) {
                    *error = "destination enter failed: " + destination_error +
                             "; rollback to previous world failed: " + rollback_error;
                }
            } else if (error) {
                *error = "destination enter failed; previous world restored: " +
                         destination_error;
            }
        } else {
            set_error(error, destination_error.c_str());
        }
        return false;
    }

    current_world_ = destination;
    current_position_ = entry;
    return_anchor_ = return_anchor;
    return true;
}

bool WorldRegistry::return_to_anchor(std::string* error) {
    if (!return_anchor_) {
        set_error(error, "no return anchor is available");
        return false;
    }
    const ReturnAnchor anchor = *return_anchor_;
    const auto destination = worlds_.find(anchor.world);
    if (destination == worlds_.end()) {
        set_error(error, "return-anchor world is not registered");
        return false;
    }
    const auto current = worlds_.find(current_world_);
    if (current != worlds_.end() && !current->second->leave(anchor, error)) {
        return false;
    }
    std::string destination_error;
    if (!destination->second->enter(anchor.position, &destination_error)) {
        if (destination_error.empty()) destination_error = "no detail supplied";
        if (current != worlds_.end()) {
            std::string rollback_error;
            if (!current->second->enter(current_position_, &rollback_error)) {
                if (rollback_error.empty()) rollback_error = "no detail supplied";
                if (error) {
                    *error = "return destination enter failed: " + destination_error +
                             "; rollback to previous world failed: " + rollback_error;
                }
            } else if (error) {
                *error = "return destination enter failed; previous world restored: " +
                         destination_error;
            }
        } else {
            set_error(error, destination_error.c_str());
        }
        return false;
    }

    current_world_ = anchor.world;
    current_position_ = anchor.position;
    return_anchor_.reset();
    return true;
}

void WorldRegistry::tick() {
    if (const auto it = worlds_.find(current_world_); it != worlds_.end())
        it->second->tick();
}

bool TwoRoomProofWorld::enter(const WorldPosition& entry, std::string* error) {
    if (entry != kRoomAEntry && entry != kRoomBEntry) {
        set_error(error, "proof world entry is not a deterministic landing");
        return false;
    }
    position_ = entry;
    return true;
}

bool TwoRoomProofWorld::leave(const ReturnAnchor&, std::string*) {
    position_.reset();
    return true;
}

void TwoRoomProofWorld::tick() {
    if (position_) ++ticks_;
}

bool TwoRoomProofWorld::take_door(ProofDoor door, WorldPosition* landing,
                                  std::string* error) {
    if (!position_) {
        set_error(error, "proof world is not active");
        return false;
    }
    if (*position_ == kRoomAEntry && door == ProofDoor::East) {
        *landing = kRoomBEntry;
    } else if (*position_ == kRoomBEntry && door == ProofDoor::West) {
        *landing = kRoomAEntry;
    } else {
        set_error(error, "proof world door has no transition");
        return false;
    }
    position_ = *landing;
    return true;
}

bool InventoryCore::set_item(CrossWorldItemId id, OwnershipFlags ownership,
                             Capability capabilities, ItemTraits traits,
                             std::string* error) {
    if (!valid_world(id.origin)) {
        set_error(error, "item has an unknown origin world");
        return false;
    }
    if (!has_flag(ownership, OwnershipFlags::Owned) &&
        has_flag(ownership, OwnershipFlags::Equipped)) {
        set_error(error, "an equipped item must be owned");
        return false;
    }
    if (!valid_ownership(static_cast<std::uint8_t>(ownership)) ||
        !valid_capabilities(static_cast<std::uint32_t>(capabilities)) ||
        !valid_traits(traits)) {
        set_error(error, "item has invalid saved traits or capability bits");
        return false;
    }
    if (!items_.contains(id) && items_.size() >= kMaxItemRecords) {
        set_error(error, "foreign inventory has too many item records");
        return false;
    }
    items_[id] = ItemRecord{ownership, capabilities, traits};
    return true;
}

OwnershipFlags InventoryCore::ownership(CrossWorldItemId id) const {
    const auto it = items_.find(id);
    return it == items_.end() ? OwnershipFlags::None : it->second.ownership;
}

std::optional<Capability> InventoryCore::capabilities(CrossWorldItemId id) const {
    const auto it = items_.find(id);
    if (it == items_.end()) return std::nullopt;
    return it->second.capabilities;
}

std::optional<ItemTraits> InventoryCore::traits(CrossWorldItemId id) const {
    const auto it = items_.find(id);
    if (it == items_.end()) return std::nullopt;
    return it->second.traits;
}

bool InventoryCore::set_loadout_slot(LoadoutId loadout, std::size_t slot,
                                     std::optional<CrossWorldItemId> id,
                                     std::string* error) {
    if (!valid_loadout(loadout)) {
        set_error(error, "loadout id is invalid");
        return false;
    }
    if (slot >= kLoadoutSlots) {
        set_error(error, "loadout slot is out of range");
        return false;
    }
    if (id && !has_flag(ownership(*id), OwnershipFlags::Owned)) {
        set_error(error, "loadout items must be owned by their exact origin");
        return false;
    }
    loadouts_[loadout_index(loadout)][slot] = id;
    return true;
}

std::optional<CrossWorldItemId> InventoryCore::loadout_slot(
    LoadoutId loadout, std::size_t slot) const {
    if (!valid_loadout(loadout) || slot >= kLoadoutSlots) return std::nullopt;
    return loadouts_[loadout_index(loadout)][slot];
}

std::vector<CrossWorldItemId> InventoryCore::capability_providers(
    LoadoutId loadout, Capability capability) const {
    std::vector<CrossWorldItemId> providers;
    if (!valid_loadout(loadout) ||
        !valid_capabilities(static_cast<std::uint32_t>(capability))) {
        return providers;
    }
    for (const auto& slot : loadouts_[loadout_index(loadout)]) {
        if (!slot) continue;
        const auto it = items_.find(*slot);
        if (it != items_.end() &&
            minish::foreign_world::has_capability(it->second.capabilities,
                                                  capability))
            providers.push_back(*slot);
    }
    return providers;
}

bool InventoryCore::has_capability(LoadoutId loadout,
                                   Capability capability) const {
    return !capability_providers(loadout, capability).empty();
}

std::vector<std::uint8_t> InventoryCore::serialize() const {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(4 + 2 + 2 + items_.size() * 19 +
                  2 * (1 + kLoadoutSlots * 6) + 24);
    bytes.insert(bytes.end(), kInventoryMagic.begin(), kInventoryMagic.end());
    append_u16(&bytes, kInventoryVersion);
    append_u16(&bytes, static_cast<std::uint16_t>(items_.size()));
    for (const auto& [id, record] : items_) {
        bytes.push_back(static_cast<std::uint8_t>(id.origin));
        append_u32(&bytes, id.item);
        bytes.push_back(static_cast<std::uint8_t>(record.ownership));
        append_u32(&bytes, static_cast<std::uint32_t>(record.capabilities));
        append_u16(&bytes, record.traits.tier);
        bytes.push_back(static_cast<std::uint8_t>(record.traits.behavior_id.origin));
        append_u32(&bytes, record.traits.behavior_id.item);
        bytes.push_back(static_cast<std::uint8_t>(record.traits.use_kind));
        bytes.push_back(static_cast<std::uint8_t>(record.traits.resource_pool));
    }
    for (std::size_t index = 0; index < loadouts_.size(); ++index) {
        const auto& loadout = loadouts_[index];
        bytes.push_back(static_cast<std::uint8_t>(index));
        for (const auto& slot : loadout) {
            bytes.push_back(slot ? 1 : 0);
            if (slot) {
                bytes.push_back(static_cast<std::uint8_t>(slot->origin));
                append_u32(&bytes, slot->item);
            }
        }
    }
    append_resources(&bytes, native_resources_);
    append_resources(&bytes, zelda1_resources_);
    return bytes;
}

bool InventoryCore::deserialize(std::span<const std::uint8_t> bytes,
                                InventoryCore* output, std::string* error) {
    if (!output) {
        set_error(error, "deserialize output is null");
        return false;
    }
    Reader reader(bytes);
    for (const auto expected : kInventoryMagic) {
        std::uint8_t actual;
        if (!reader.u8(&actual) || actual != expected) {
            set_error(error, "foreign inventory magic is invalid");
            return false;
        }
    }
    std::uint16_t version, count;
    if (!reader.u16(&version) || version != kInventoryVersion) {
        set_error(error, "foreign inventory version is unsupported");
        return false;
    }
    if (!reader.u16(&count) || count > kMaxItemRecords) {
        set_error(error, "foreign inventory item count is invalid");
        return false;
    }

    InventoryCore parsed;
    for (std::uint16_t index = 0; index < count; ++index) {
        std::uint8_t origin, flags, behavior_origin, use_kind, resource_pool;
        std::uint16_t tier;
        std::uint32_t item, capabilities, behavior_item;
        if (!reader.u8(&origin) || !reader.u32(&item) || !reader.u8(&flags) ||
            !reader.u32(&capabilities) || !reader.u16(&tier) ||
            !reader.u8(&behavior_origin) || !reader.u32(&behavior_item) ||
            !reader.u8(&use_kind) || !reader.u8(&resource_pool) ||
            !valid_world(static_cast<WorldId>(origin)) || !valid_ownership(flags) ||
            !valid_capabilities(capabilities)) {
            set_error(error, "foreign inventory item record is invalid");
            return false;
        }
        const ItemTraits traits{tier,
                                CrossWorldItemId{
                                    static_cast<WorldId>(behavior_origin), behavior_item},
                                static_cast<ItemUseKind>(use_kind),
                                static_cast<ResourcePoolProvenance>(resource_pool)};
        const CrossWorldItemId id{static_cast<WorldId>(origin), item};
        if (parsed.items_.contains(id) ||
            !valid_traits(traits) ||
            (!has_flag(static_cast<OwnershipFlags>(flags), OwnershipFlags::Owned) &&
             has_flag(static_cast<OwnershipFlags>(flags), OwnershipFlags::Equipped))) {
            set_error(error, "foreign inventory item record violates ownership rules");
            return false;
        }
        parsed.items_.emplace(id, ItemRecord{static_cast<OwnershipFlags>(flags),
                                              static_cast<Capability>(capabilities),
                                              traits});
    }
    for (std::size_t expected = 0; expected < parsed.loadouts_.size(); ++expected) {
        std::uint8_t encoded_loadout;
        if (!reader.u8(&encoded_loadout) ||
            !valid_loadout(static_cast<LoadoutId>(encoded_loadout)) ||
            encoded_loadout != expected) {
            set_error(error, "foreign inventory loadout id is invalid or out of order");
            return false;
        }
        auto& loadout = parsed.loadouts_[expected];
        for (auto& slot : loadout) {
            std::uint8_t present;
            if (!reader.u8(&present) || present > 1) {
                set_error(error, "foreign inventory loadout marker is invalid");
                return false;
            }
            if (!present) continue;
            std::uint8_t origin;
            std::uint32_t item;
            if (!reader.u8(&origin) || !reader.u32(&item) ||
                !valid_world(static_cast<WorldId>(origin))) {
                set_error(error, "foreign inventory loadout item is invalid");
                return false;
            }
            const CrossWorldItemId id{static_cast<WorldId>(origin), item};
            if (!has_flag(parsed.ownership(id), OwnershipFlags::Owned)) {
                set_error(error, "foreign inventory loadout references an unowned item");
                return false;
            }
            slot = id;
        }
    }
    if (!read_resources(&reader, &parsed.native_resources_) ||
        !read_resources(&reader, &parsed.zelda1_resources_) || !reader.at_end()) {
        set_error(error, "foreign inventory payload is truncated or has trailing data");
        return false;
    }
    *output = std::move(parsed);
    return true;
}

}  // namespace minish::foreign_world
