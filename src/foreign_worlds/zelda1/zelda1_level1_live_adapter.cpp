#include "foreign_worlds/zelda1/zelda1_level1_live_adapter.h"

#include <algorithm>
#include <limits>

namespace minish::foreign_world::zelda1 {
namespace {
constexpr std::size_t kHeaderSize = 20;
constexpr std::uint8_t kSourceStartX = 0x80;
// CreateRoomObjects positions the pickup in source screen coordinates.  The
// host player presentation is feet-originated, so accept an 8px interaction
// square around the corresponding feet coordinate until the source item
// metasprite collision routine is decoded.
constexpr std::int16_t kItemInteractionRadius = 8;

Level1Edge edge_for(bool horizontal, bool positive) {
    if (horizontal) return positive ? Level1Edge::kEast : Level1Edge::kWest;
    return positive ? Level1Edge::kSouth : Level1Edge::kNorth;
}

}  // namespace

bool Zelda1Level1LiveAdapter::source_level1_hotspot(
    const Zelda1OverworldSession& overworld) {
    if (!overworld.loaded() || overworld.area() != OverworldSessionArea::kOverworld ||
        !overworld.at_source_grid_point())
        return false;
    const auto position = overworld.source_position();
    if (position.room_id != Zelda1Level1Session::kOverworldEntranceRoom ||
        !overworld.loader().first_quest_data()) return false;
    const unsigned source_x = static_cast<unsigned>(position.obj_x);
    const unsigned source_y = static_cast<unsigned>(position.obj_y);
    // CheckWarps reaches this OW level path through GetCollidableTileStill.
    // Its tile fetch uses ObjY + $0b before subtracting the status-bar
    // origin, while the warp-grid alignment remains the unadjusted ObjY.
    // Sampling raw ObjY made the adapter accept a point eleven pixels above
    // the visible Level 1 entrance and reject the actual doorway.
    if ((source_x & 0x0f) != 0 || (source_y & 0x0f) != 0x0d ||
        !ow_source_still_is_warp_trigger(overworld.geometry(), position.obj_x,
                                          position.obj_y)) return false;
    OverworldRoomView room{};
    if (!overworld.loader().first_quest_data()->overworld_room(
            position.room_id & 15, position.room_id >> 4, &room)) return false;
    const auto entrance = FirstQuestData::decode_overworld_entrance(room);
    return entrance.kind == OverworldEntranceKind::kLevel && entrance.destination_index == 1;
}

Level1LiveResult Zelda1Level1LiveAdapter::try_enter_from_overworld(
    const Zelda1OverworldSession& overworld) {
    if (session_ && session_->active()) return Level1LiveResult::kInvalid;
    if (!overworld.loaded() || overworld.area() != OverworldSessionArea::kOverworld ||
        overworld.position().room_id != Zelda1Level1Session::kOverworldEntranceRoom)
        return Level1LiveResult::kWrongOverworldContext;
    if (!source_level1_hotspot(overworld)) return Level1LiveResult::kNotAtSourceHotspot;
    const auto* data = overworld.loader().first_quest_data();
    if (!data) return Level1LiveResult::kInvalid;
    Zelda1Level1Session candidate(*data);
    const auto enter = session_
        ? ((data_ == data && clone_session(&candidate))
            ? candidate.resume_from_overworld(Zelda1Level1Session::kOverworldEntranceRoom)
            : Level1Result::kInvalid)
        : candidate.enter_from_overworld(Zelda1Level1Session::kOverworldEntranceRoom);
    if (enter != Level1Result::kEntered) return map(enter);
    FirstQuestUnderworldLevelView level{};
    if (!data->first_quest_underworld_level(1, &level)) return Level1LiveResult::kInvalid;
    // `StartY` is source data; the x center and static framebuffer crop are
    // explicit host presentation policy (source X $80 -> crop X 120).
    const Level1PlayerPresentation player{
        static_cast<std::int16_t>(kSourceStartX - 8),
        // Level object Y coordinates include the NES status-bar origin just
        // like the OW/cave source positions, so use the renderer's -72 crop.
        static_cast<std::int16_t>(level.info.start_y - 72)};
    if (player.x < 0 || player.x >= static_cast<std::int16_t>(kOwRenderWidth) ||
        player.y < 0 || player.y >= static_cast<std::int16_t>(kOwRenderHeight))
        return Level1LiveResult::kInvalid;
    data_ = data;
    session_.reset();
    session_.emplace(std::move(candidate));
    player_ = player;
    prior_sword_active_ = false;
    return Level1LiveResult::kEntered;
}

Level1LiveResult Zelda1Level1LiveAdapter::map(Level1Result result) {
    switch (result) {
    case Level1Result::kMoved: return Level1LiveResult::kMoved;
    case Level1Result::kNeedsKey: return Level1LiveResult::kNeedsKey;
    case Level1Result::kNeedsBomb: return Level1LiveResult::kNeedsBomb;
    case Level1Result::kSecretUnresolved: return Level1LiveResult::kSecretUnresolved;
    case Level1Result::kCollected: return Level1LiveResult::kCollected;
    case Level1Result::kDefeated: return Level1LiveResult::kBossDefeated;
    case Level1Result::kContact: return Level1LiveResult::kSwordHit;
    case Level1Result::kReturned: return Level1LiveResult::kReturned;
    case Level1Result::kNoItem: return Level1LiveResult::kNoItem;
    case Level1Result::kBlocked: return Level1LiveResult::kBlocked;
    default: return Level1LiveResult::kInvalid;
    }
}

bool Zelda1Level1LiveAdapter::clone_session(Zelda1Level1Session* out) const {
    if (!out || !session_ || !data_) return false;
    return out->restore(session_->serialize());
}

bool Zelda1Level1LiveAdapter::clone_inventory(
    const foreign_world::InventoryCore& source,
    foreign_world::InventoryCore* out, std::string* error) const {
    return out && foreign_world::InventoryCore::deserialize(source.serialize(), out, error);
}

Level1LiveResult Zelda1Level1LiveAdapter::move_axis(
    std::int16_t distance, bool horizontal,
    foreign_world::InventoryCore* inventory,
    foreign_world::LoadoutId loadout,
    std::uint8_t* returned_overworld_room) {
    if (!session_) return Level1LiveResult::kNotActive;
    if (!inventory) return Level1LiveResult::kInventoryRejected;
    if (distance == std::numeric_limits<std::int16_t>::min()) return Level1LiveResult::kInvalid;
    const bool positive = distance > 0;
    unsigned steps = static_cast<unsigned>(positive ? distance : -distance);
    if (steps == 0) return Level1LiveResult::kMoved;
    auto next_player = player_;
    for (unsigned step = 0; step < steps; ++step) {
        std::int16_t& coordinate = horizontal ? next_player.x : next_player.y;
        const std::int16_t boundary = positive
            ? static_cast<std::int16_t>((horizontal ? kOwRenderWidth : kOwRenderHeight) - 1) : 0;
        if (coordinate != boundary) {
            coordinate = static_cast<std::int16_t>(coordinate + (positive ? 1 : -1));
            player_ = next_player;
            continue;
        }
        const auto edge = edge_for(horizontal, positive);
        // Source start room $73's south door is the only cross-world return;
        // the lower underworld lattice has no room $83.
        if (!horizontal && positive && session_->room_id() == Zelda1Level1Session::kStartRoom) {
            std::uint8_t exit_room{};
            if (session_->exit_to_overworld(&exit_room) != Level1Result::kReturned)
                return Level1LiveResult::kInvalid;
            if (returned_overworld_room) *returned_overworld_room = exit_room;
            // Keep the inactive child session resident. Re-entering the exact
            // OW37 source hotspot resumes this dungeon state; lifecycle reset
            // remains the explicit way to discard it.
            return Level1LiveResult::kReturned;
        }
        // The child session owns source door rendering/state; stage it and the
        // independent Zelda resource pool together so a render failure cannot
        // consume either representation of a key.
        Zelda1Level1Session session_candidate(*data_);
        foreign_world::InventoryCore inventory_candidate;
        if (!clone_session(&session_candidate) ||
            !clone_inventory(*inventory, &inventory_candidate, nullptr))
            return Level1LiveResult::kInvalid;
        const auto keys_before = session_candidate.key_count();
        const auto result = session_candidate.try_transition(edge, inventory_candidate, loadout);
        if (result != Level1Result::kMoved) return map(result);
        const auto keys_after = session_candidate.key_count();
        if (keys_after > keys_before) return Level1LiveResult::kInvalid;
        const auto consumed = static_cast<std::uint16_t>(keys_before - keys_after);
        if (consumed > inventory_candidate.zelda1_resources().keys)
            return Level1LiveResult::kNeedsKey;
        inventory_candidate.zelda1_resources().keys = static_cast<std::uint16_t>(
            inventory_candidate.zelda1_resources().keys - consumed);
        *inventory = std::move(inventory_candidate);
        session_.reset();
        session_.emplace(std::move(session_candidate));
        coordinate = positive ? 0 : static_cast<std::int16_t>(
            (horizontal ? kOwRenderWidth : kOwRenderHeight) - 1);
        player_ = next_player;
    }
    return Level1LiveResult::kMoved;
}

Level1LiveResult Zelda1Level1LiveAdapter::move_by(
    std::int16_t dx, std::int16_t dy, foreign_world::InventoryCore* inventory,
    foreign_world::LoadoutId loadout, std::uint8_t* returned_overworld_room) {
    if (!session_) return Level1LiveResult::kNotActive;
    if (!inventory) return Level1LiveResult::kInventoryRejected;
    if (dx == std::numeric_limits<std::int16_t>::min() ||
        dy == std::numeric_limits<std::int16_t>::min()) return Level1LiveResult::kInvalid;
    if (const auto x = move_axis(dx, true, inventory, loadout, returned_overworld_room);
        x != Level1LiveResult::kMoved) return x;
    return move_axis(dy, false, inventory, loadout, returned_overworld_room);
}

Level1LiveResult Zelda1Level1LiveAdapter::collect_room_item(
    foreign_world::InventoryCore* inventory, std::string* error) {
    if (!session_) return Level1LiveResult::kNotActive;
    if (!inventory) return Level1LiveResult::kInventoryRejected;
    const auto item = session_->available_room_item();
    if (!item) return Level1LiveResult::kNoItem;
    if (!overlaps_room_item(*item)) return Level1LiveResult::kBlocked;
    Zelda1Level1Session session_candidate(*data_);
    foreign_world::InventoryCore inventory_candidate;
    if (!clone_session(&session_candidate) || !clone_inventory(*inventory, &inventory_candidate, error))
        return Level1LiveResult::kInvalid;
    using namespace foreign_world;
    const CrossWorldItemId id{WorldId::Zelda1, item->source_item_id};
    const ItemTraits traits{1, id, ItemUseKind::Passive,
                            ResourcePoolProvenance::Zelda1};
    if (!inventory_candidate.acquire_item(id, OwnershipFlags::Owned,
                                          Capability::None, traits, error))
        return Level1LiveResult::kInventoryRejected;
    if (item->source_item_id == Zelda1Level1Session::kKeyItemId &&
        inventory_candidate.zelda1_resources().keys != 255)
        ++inventory_candidate.zelda1_resources().keys;
    std::uint8_t collected{};
    if (session_candidate.collect_room_item(&collected) != Level1Result::kCollected ||
        collected != item->source_item_id) return Level1LiveResult::kInvalid;
    *inventory = std::move(inventory_candidate);
    session_.reset();
    session_.emplace(std::move(session_candidate));
    return Level1LiveResult::kCollected;
}

bool Zelda1Level1LiveAdapter::overlaps_aquamentus(const ForeignSwordHitbox& hitbox) {
    // InitAquamentus starts at ($B0,$80). Its six source sprites occupy a
    // 24x16 body; retain one-pixel-inclusive host hitbox convention.
    return hitbox.left <= 0xc7 && hitbox.right >= 0xb0 &&
           hitbox.top <= 0x8f && hitbox.bottom >= 0x80;
}

bool Zelda1Level1LiveAdapter::overlaps_room_item(const Level1RoomItem& item) const {
    const auto item_player_x = static_cast<std::int16_t>(item.x - 8);
    const auto item_player_y = static_cast<std::int16_t>(item.y - 72);
    return std::abs(player_.x - item_player_x) <= kItemInteractionRadius &&
           std::abs(player_.y - item_player_y) <= kItemInteractionRadius;
}

Level1LiveResult Zelda1Level1LiveAdapter::observe_sword(
    const MinishSwordSample& sword, foreign_world::InventoryCore* inventory,
    foreign_world::LoadoutId loadout, bool selected_sword_edge,
    std::uint8_t player_animation_state, std::string* error) {
    if (!session_) return Level1LiveResult::kNotActive;
    if (!inventory) return Level1LiveResult::kInventoryRejected;
    const bool active_sword = is_active_minish_sword(sword);
    const bool source_edge = active_sword && !prior_sword_active_;
    prior_sword_active_ = active_sword;
    // A selected exact-origin sword may drive host combat only when no
    // source ItemSword is live. This deliberately preserves native attack
    // ownership/animation and prevents a concurrent A edge from double-hit.
    const bool host_edge = selected_sword_edge && !active_sword;
    if ((!source_edge && !host_edge) ||
        session_->room_id() != Zelda1Level1Session::kBossRoom)
        return Level1LiveResult::kBlocked;
    auto inventory_candidate = foreign_world::InventoryCore{};
    if (!clone_inventory(*inventory, &inventory_candidate, error)) return Level1LiveResult::kInvalid;
    if (source_edge)
        (void)import_observed_native_sword(&inventory_candidate, loadout, sword, error);
    if (!resolve_foreign_sword(inventory_candidate, loadout)) return Level1LiveResult::kInventoryRejected;
    const auto facing = foreign_sword_facing(source_edge
        ? sword.player_animation_state : player_animation_state);
    if (!facing) return Level1LiveResult::kBlocked;
    const auto hitbox = foreign_sword_hitbox(static_cast<std::uint8_t>(player_.x + 8),
                                              static_cast<std::uint8_t>(player_.y + 72), *facing);
    if (!overlaps_aquamentus(hitbox)) return Level1LiveResult::kBlocked;
    Zelda1Level1Session session_candidate(*data_);
    if (!clone_session(&session_candidate)) return Level1LiveResult::kInvalid;
    const auto hit = session_candidate.sword_hit_boss(1);
    if (hit != Level1Result::kContact && hit != Level1Result::kDefeated) return Level1LiveResult::kInvalid;
    *inventory = std::move(inventory_candidate);
    session_.reset();
    session_.emplace(std::move(session_candidate));
    return hit == Level1Result::kDefeated ? Level1LiveResult::kBossDefeated : Level1LiveResult::kSwordHit;
}

Level1LivePresentation Zelda1Level1LiveAdapter::presentation() const {
    return {active() ? &session_->framebuffer() : nullptr, player_,
            static_cast<std::uint8_t>(session_ ? session_->room_id() : 0), active()};
}

void Zelda1Level1LiveAdapter::reset() {
    session_.reset(); data_ = nullptr; player_ = {}; prior_sword_active_ = false;
}

std::vector<std::uint8_t> Zelda1Level1LiveAdapter::serialize() const {
    std::vector<std::uint8_t> out(kSerializedSize, 0);
    out[0]='Z'; out[1]='1'; out[2]='L'; out[3]='A'; out[4]=1; out[5]=session_ ? 1 : 0;
    out[6]=static_cast<std::uint8_t>(player_.x); out[7]=static_cast<std::uint8_t>(player_.x >> 8);
    out[8]=static_cast<std::uint8_t>(player_.y); out[9]=static_cast<std::uint8_t>(player_.y >> 8);
    out[10]=prior_sword_active_ ? 1 : 0;
    if (session_) {
        const auto child = session_->serialize();
        std::copy(child.begin(), child.end(), out.begin() + kHeaderSize);
    }
    return out;
}

bool Zelda1Level1LiveAdapter::restore(const Zelda1OverworldSession& overworld,
                                      std::span<const std::uint8_t> blob,
                                      std::string* error) {
    if (blob.size() != kSerializedSize || blob[0]!='Z' || blob[1]!='1' || blob[2]!='L' ||
        blob[3]!='A' || blob[4]!=1 || blob[5]>1 || blob[10]>1 ||
        !std::all_of(blob.begin()+11, blob.begin()+kHeaderSize, [](std::uint8_t b){ return b==0; })) return false;
    if (blob[5] == 0) {
        if (!std::all_of(blob.begin()+kHeaderSize, blob.end(), [](std::uint8_t b){ return b==0; })) return false;
        reset(); return true;
    }
    if (!overworld.loaded() || !overworld.loader().first_quest_data() ||
        (data_ != nullptr && overworld.loader().first_quest_data() != data_)) return false;
    const auto* data = overworld.loader().first_quest_data();
    const auto x = static_cast<std::int16_t>(blob[6] | (static_cast<std::uint16_t>(blob[7]) << 8));
    const auto y = static_cast<std::int16_t>(blob[8] | (static_cast<std::uint16_t>(blob[9]) << 8));
    if (x < 0 || x >= static_cast<std::int16_t>(kOwRenderWidth) || y < 0 || y >= static_cast<std::int16_t>(kOwRenderHeight)) return false;
    Zelda1Level1Session candidate(*data);
    if (!candidate.restore(blob.subspan(kHeaderSize, Zelda1Level1Session::kSerializedSize))) return false;
    data_ = data; session_.emplace(std::move(candidate)); player_ = {x,y}; prior_sword_active_ = blob[10] != 0;
    return true;
}

}  // namespace minish::foreign_world::zelda1
