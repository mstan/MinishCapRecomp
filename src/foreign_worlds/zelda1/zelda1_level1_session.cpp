#include "foreign_worlds/zelda1/zelda1_level1_session.h"

#include "foreign_worlds/zelda1/zelda1_uw_renderer.h"

#include <algorithm>

namespace minish::foreign_world::zelda1 {
namespace {

constexpr std::uint16_t kQaMarkerMagenta = 0x7c1f;
constexpr std::uint16_t kQaMarkerGold = 0x03ff;

void draw_marker(OwFramebuffer* frame, int x, int y, int width, int height,
                 std::uint16_t colour) {
    // Metasprite CHR decode is intentionally not part of this first host
    // session. These conspicuous outlined boxes are QA-only markers, never a
    // claim about Zelda 1 sprite pixels.
    for (int py = 0; py < height; ++py) for (int px = 0; px < width; ++px) {
        if (px != 0 && py != 0 && px != width - 1 && py != height - 1) continue;
        const int sx = x + px, sy = y + py;
        if (sx >= 0 && sx < static_cast<int>(kOwRenderWidth) && sy >= 0 && sy < static_cast<int>(kOwRenderHeight))
            (*frame)[sy * kOwRenderWidth + sx] = colour;
    }
}

bool traversable_door(DoorType type) {
    return type == DoorType::kOpen || type == DoorType::kKey ||
           type == DoorType::kKey2 || type == DoorType::kBombable;
}

}  // namespace

Zelda1Level1Session::Zelda1Level1Session(const FirstQuestData& data) : data_(data) {
    (void)copy_underworld_background_from_verified_prg(data_, &patterns_);
    (void)refresh_frame();
}

std::uint8_t Zelda1Level1Session::opposite(Level1Edge edge) {
    return static_cast<std::uint8_t>(static_cast<unsigned>(edge) ^ 1u);
}

int Zelda1Level1Session::delta(Level1Edge edge) {
    switch (edge) {
    case Level1Edge::kEast: return 1;
    case Level1Edge::kWest: return -1;
    case Level1Edge::kSouth: return 16;
    case Level1Edge::kNorth: return -16;
    }
    return 0;
}

std::size_t Zelda1Level1Session::door_bit(std::uint8_t room, Level1Edge edge) {
    return static_cast<std::size_t>(room) * 4 + static_cast<unsigned>(edge);
}

bool Zelda1Level1Session::room_view(std::uint8_t id, UnderworldRoomView* out) const {
    return out && data_.first_quest_underworld_room(1, id, out);
}

bool Zelda1Level1Session::valid_entry_room(std::uint8_t room) const {
    OverworldRoomView view{};
    if (!data_.overworld_room(room & 15, room >> 4, &view)) return false;
    const auto entrance = FirstQuestData::decode_overworld_entrance(view);
    return entrance.kind == OverworldEntranceKind::kLevel && entrance.destination_index == 1;
}

bool Zelda1Level1Session::edge_is_open(std::uint8_t room, Level1Edge edge) const {
    const auto bit = door_bit(room, edge);
    return (opened_door_bits_[bit / 8] & (1u << (bit % 8))) != 0;
}

void Zelda1Level1Session::set_edge_open(std::uint8_t room, Level1Edge edge) {
    const auto bit = door_bit(room, edge);
    opened_door_bits_[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8));
    const int next = static_cast<int>(room) + delta(edge);
    if (next >= 0 && next < 128) {
        const auto other = door_bit(static_cast<std::uint8_t>(next),
                                    static_cast<Level1Edge>(opposite(edge)));
        opened_door_bits_[other / 8] |= static_cast<std::uint8_t>(1u << (other % 8));
    }
}

bool Zelda1Level1Session::refresh_frame() {
    OwFramebuffer candidate{};
    if (!render_underworld_room(data_, 1, room_id_, patterns_, &candidate)) return false;
    UnderworldRoomView room{};
    if (!room_view(room_id_, &room)) return false;
    if (active_) {
        if (room.is_aquamentus && !boss_defeated_)
            draw_marker(&candidate, 168, 56, 32, 24, kQaMarkerMagenta);
        if (available_room_item())
            draw_marker(&candidate, static_cast<int>(room.item_x) - 8,
                        static_cast<int>(room.item_y) - 56, 10, 10, kQaMarkerGold);
    }
    frame_ = candidate;
    return true;
}

void Zelda1Level1Session::copy_dynamic_state_to(Zelda1Level1Session* out) const {
    if (!out) return;
    out->active_ = active_;
    out->entry_overworld_room_ = entry_overworld_room_;
    out->room_id_ = room_id_;
    out->keys_ = keys_;
    out->boss_hit_points_ = boss_hit_points_;
    out->boss_defeated_ = boss_defeated_;
    out->room_flags_ = room_flags_;
    out->opened_door_bits_ = opened_door_bits_;
    out->frame_ = frame_;
}

void Zelda1Level1Session::commit_dynamic_state_from(const Zelda1Level1Session& source) {
    active_ = source.active_;
    entry_overworld_room_ = source.entry_overworld_room_;
    room_id_ = source.room_id_;
    keys_ = source.keys_;
    boss_hit_points_ = source.boss_hit_points_;
    boss_defeated_ = source.boss_defeated_;
    room_flags_ = source.room_flags_;
    opened_door_bits_ = source.opened_door_bits_;
    frame_ = source.frame_;
}

Level1Result Zelda1Level1Session::enter_from_overworld(std::uint8_t overworld_room_id) {
    if (!valid_entry_room(overworld_room_id)) return Level1Result::kInvalid;
    FirstQuestUnderworldLevelView level{};
    if (!data_.first_quest_underworld_level(1, &level)) return Level1Result::kInvalid;
    Zelda1Level1Session candidate(data_);
    candidate.active_ = true;
    candidate.entry_overworld_room_ = overworld_room_id;
    candidate.room_id_ = level.info.start_room_id;
    candidate.keys_ = 0;
    candidate.room_flags_.fill(0);
    candidate.opened_door_bits_.fill(0);
    candidate.boss_hit_points_ = kAquamentusHitPoints;
    candidate.boss_defeated_ = false;
    if (!candidate.refresh_frame()) return Level1Result::kInvalid;
    commit_dynamic_state_from(candidate);
    return Level1Result::kEntered;
}

Level1Result Zelda1Level1Session::exit_to_overworld(std::uint8_t* out_overworld_room_id) {
    if (!active_ || !out_overworld_room_id || room_id_ != kStartRoom) return Level1Result::kInvalid;
    *out_overworld_room_id = entry_overworld_room_;
    active_ = false;
    return Level1Result::kReturned;
}

Level1Result Zelda1Level1Session::resume_from_overworld(std::uint8_t overworld_room_id) {
    if (active_ || room_id_ != kStartRoom || entry_overworld_room_ != overworld_room_id ||
        !valid_entry_room(overworld_room_id)) return Level1Result::kInvalid;
    Zelda1Level1Session candidate(data_);
    copy_dynamic_state_to(&candidate);
    candidate.active_ = true;
    if (!candidate.refresh_frame()) return Level1Result::kInvalid;
    commit_dynamic_state_from(candidate);
    return Level1Result::kEntered;
}

Level1Result Zelda1Level1Session::try_transition(Level1Edge edge, Level1DoorTool tool) {
    if (!active_) return Level1Result::kInvalid;
    UnderworldRoomView room{};
    if (!room_view(room_id_, &room)) return Level1Result::kInvalid;
    const int next = static_cast<int>(room_id_) + delta(edge);
    if (next < 0 || next >= 128 ||
        ((edge == Level1Edge::kEast || edge == Level1Edge::kWest) &&
         ((room_id_ & 15) == (edge == Level1Edge::kEast ? 15 : 0))))
        return Level1Result::kBlocked;
    const DoorType door = room.doors_e_w_s_n[static_cast<unsigned>(edge)];
    if (door == DoorType::kWall) return Level1Result::kBlocked;
    if (door == DoorType::kFalseWall || door == DoorType::kFalseWall2 ||
        door == DoorType::kShutter)
        return Level1Result::kSecretUnresolved;
    if (!traversable_door(door)) return Level1Result::kBlocked;
    Zelda1Level1Session candidate(data_);
    copy_dynamic_state_to(&candidate);
    if ((door == DoorType::kKey || door == DoorType::kKey2) && !candidate.edge_is_open(candidate.room_id_, edge)) {
        if (candidate.keys_ == 0) return Level1Result::kNeedsKey;
        --candidate.keys_;
        candidate.set_edge_open(candidate.room_id_, edge);
    }
    if (door == DoorType::kBombable && !candidate.edge_is_open(candidate.room_id_, edge)) {
        if (tool != Level1DoorTool::kBomb) return Level1Result::kNeedsBomb;
        candidate.set_edge_open(candidate.room_id_, edge);
    }
    candidate.room_id_ = static_cast<std::uint8_t>(next);
    if (!candidate.refresh_frame()) return Level1Result::kInvalid;
    commit_dynamic_state_from(candidate);
    return Level1Result::kMoved;
}

Level1DoorTool Zelda1Level1Session::resolve_door_tool(
    const foreign_world::InventoryCore& inventory,
    foreign_world::LoadoutId loadout) {
    return inventory.has_capability(loadout, foreign_world::Capability::Bomb)
        ? Level1DoorTool::kBomb : Level1DoorTool::kNone;
}

Level1Result Zelda1Level1Session::try_transition(
    Level1Edge edge, const foreign_world::InventoryCore& inventory,
    foreign_world::LoadoutId loadout) {
    return try_transition(edge, resolve_door_tool(inventory, loadout));
}

std::optional<Level1RoomItem> Zelda1Level1Session::available_room_item() const {
    if (!active_ || (room_flags_[room_id_] & kFlagItemTaken)) return std::nullopt;
    UnderworldRoomView room{};
    if (!room_view(room_id_, &room) || room.room_item_id == 0x03) return std::nullopt;
    // CreateRoomObjects suppresses secret action 3 (last boss) and 7 (foes
    // for item). For Level 1's boss room, defeating $3D is that exact action.
    if (room.secret_trigger == 3) return std::nullopt;
    if (room.secret_trigger == 7 && !(room.is_aquamentus && boss_defeated_)) return std::nullopt;
    return Level1RoomItem{room.room_item_id, room.item_x, room.item_y,
                          room.secret_trigger == 7};
}

Level1Result Zelda1Level1Session::collect_room_item(std::uint8_t* out_source_item_id) {
    const auto item = available_room_item();
    if (!item) return Level1Result::kNoItem;
    Zelda1Level1Session candidate(data_);
    copy_dynamic_state_to(&candidate);
    candidate.room_flags_[candidate.room_id_] |= kFlagItemTaken;
    if (item->source_item_id == kKeyItemId && candidate.keys_ != 255) ++candidate.keys_;
    if (!candidate.refresh_frame()) return Level1Result::kInvalid;
    commit_dynamic_state_from(candidate);
    if (out_source_item_id) *out_source_item_id = item->source_item_id;
    return Level1Result::kCollected;
}

Level1Result Zelda1Level1Session::sword_hit_boss(std::uint8_t damage) {
    if (!active_ || room_id_ != kBossRoom || boss_defeated_ || damage == 0) return Level1Result::kInvalid;
    Zelda1Level1Session candidate(data_);
    copy_dynamic_state_to(&candidate);
    candidate.boss_hit_points_ = damage >= candidate.boss_hit_points_ ? 0 :
        static_cast<std::uint8_t>(candidate.boss_hit_points_ - damage);
    if (candidate.boss_hit_points_ == 0) {
        candidate.boss_defeated_ = true;
        candidate.room_flags_[kBossRoom] |= kFlagBossDefeated;
        if (!candidate.refresh_frame()) return Level1Result::kInvalid;
        commit_dynamic_state_from(candidate);
        return Level1Result::kDefeated;
    }
    commit_dynamic_state_from(candidate);
    return Level1Result::kContact;
}

Level1Result Zelda1Level1Session::contact_boss() const {
    return active_ && room_id_ == kBossRoom && !boss_defeated_
        ? Level1Result::kContact : Level1Result::kInvalid;
}

Level1BossState Zelda1Level1Session::boss() const {
    return {active_ && room_id_ == kBossRoom && !boss_defeated_, boss_defeated_,
            kAquamentusTemplate, boss_hit_points_, 0xb0, 0x80};
}

std::vector<std::uint8_t> Zelda1Level1Session::serialize() const {
    std::vector<std::uint8_t> out(kSerializedSize, 0);
    out[0] = 'Z'; out[1] = '1'; out[2] = 'L'; out[3] = '1'; out[4] = 1;
    out[5] = active_ ? 1 : 0; out[6] = entry_overworld_room_; out[7] = room_id_;
    out[8] = keys_; out[9] = boss_hit_points_; out[10] = boss_defeated_ ? 1 : 0;
    std::copy(room_flags_.begin(), room_flags_.end(), out.begin() + 12);
    std::copy(opened_door_bits_.begin(), opened_door_bits_.end(), out.begin() + 140);
    return out;
}

bool Zelda1Level1Session::validate_state() const {
    if (boss_hit_points_ > kAquamentusHitPoints ||
        (boss_defeated_ && boss_hit_points_ != 0) || (!boss_defeated_ && boss_hit_points_ == 0)) return false;
    UnderworldRoomView current_room{};
    if (active_ && (!valid_entry_room(entry_overworld_room_) || !room_view(room_id_, &current_room))) return false;
    for (unsigned i = 0; i < room_flags_.size(); ++i) {
        const std::uint8_t allowed = i == kBossRoom ? (kFlagItemTaken | kFlagBossDefeated) : kFlagItemTaken;
        if ((room_flags_[i] & ~allowed) != 0) return false;
        if ((room_flags_[i] & kFlagItemTaken) != 0) {
            UnderworldRoomView item_room{};
            if (!room_view(static_cast<std::uint8_t>(i), &item_room) ||
                item_room.room_item_id == 0x03 || item_room.secret_trigger == 3)
                return false;
            // The bounded session implements only Aquamentus's source
            // foes-for-item action. Other action-7 room rewards cannot be
            // marked collected until their source object runtime exists.
            if (item_room.secret_trigger == 7 &&
                (!item_room.is_aquamentus || !boss_defeated_)) return false;
        }
    }
    if (((room_flags_[kBossRoom] & kFlagBossDefeated) != 0) != boss_defeated_) return false;
    for (unsigned room = 0; room < 128; ++room) for (unsigned direction = 0; direction < 4; ++direction) {
        const auto edge = static_cast<Level1Edge>(direction);
        if (!edge_is_open(static_cast<std::uint8_t>(room), edge)) continue;
        UnderworldRoomView view{};
        const int next = static_cast<int>(room) + delta(edge);
        if (!room_view(static_cast<std::uint8_t>(room), &view) || next < 0 || next >= 128 ||
            ((edge == Level1Edge::kEast || edge == Level1Edge::kWest) &&
             ((room & 15) == (edge == Level1Edge::kEast ? 15 : 0)))) return false;
        const auto type = view.doors_e_w_s_n[direction];
        if (type != DoorType::kKey && type != DoorType::kKey2 && type != DoorType::kBombable) return false;
        if (!edge_is_open(static_cast<std::uint8_t>(next),
                          static_cast<Level1Edge>(opposite(edge)))) return false;
    }
    return true;
}

bool Zelda1Level1Session::restore(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kSerializedSize || bytes[0] != 'Z' || bytes[1] != '1' ||
        bytes[2] != 'L' || bytes[3] != '1' || bytes[4] != 1 || bytes[5] > 1 || bytes[10] > 1 || bytes[11] != 0)
        return false;
    Zelda1Level1Session candidate(data_);
    candidate.active_ = bytes[5] != 0;
    candidate.entry_overworld_room_ = bytes[6]; candidate.room_id_ = bytes[7];
    candidate.keys_ = bytes[8]; candidate.boss_hit_points_ = bytes[9]; candidate.boss_defeated_ = bytes[10] != 0;
    std::copy(bytes.begin() + 12, bytes.begin() + 140, candidate.room_flags_.begin());
    std::copy(bytes.begin() + 140, bytes.begin() + 204, candidate.opened_door_bits_.begin());
    if (!candidate.validate_state() || (candidate.active_ && !candidate.refresh_frame())) return false;
    active_ = candidate.active_;
    entry_overworld_room_ = candidate.entry_overworld_room_;
    room_id_ = candidate.room_id_;
    keys_ = candidate.keys_;
    boss_hit_points_ = candidate.boss_hit_points_;
    boss_defeated_ = candidate.boss_defeated_;
    room_flags_ = candidate.room_flags_;
    opened_door_bits_ = candidate.opened_door_bits_;
    frame_ = candidate.frame_;
    return true;
}

}  // namespace minish::foreign_world::zelda1
