#pragma once

#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace minish::foreign_world::zelda1 {

// A deliberately small, deterministic host-side state machine.  It is not a
// NES emulator: every field whose source meaning has not been recovered stays
// marked opaque instead of gaining an invented gameplay rule.
enum class WorldArea : std::uint8_t { kOverworld, kCave, kLevel1 };
enum class Edge : std::uint8_t { kEast, kWest, kSouth, kNorth };
enum class ModelCollision : std::uint8_t { kOpaqueFinalTile, kMapBoundary };
enum class ModelResult : std::uint8_t {
    kMoved, kEntered, kReturned, kBlocked, kNeedsKey, kOpaqueCollision,
    kNoEntrance, kInvalid,
};

struct WorldPosition { WorldArea area; std::uint8_t room_id; };
struct ModelSquare { std::uint8_t primary_square; ModelCollision collision; };
struct EnemyActor {
    std::uint8_t source_template_id;
    std::uint8_t hit_points;
    bool hit_points_known;
    std::uint8_t contact_damage;
    bool contact_damage_known;
    bool defeated;
    bool drop_is_opaque;
    std::uint8_t x;
    std::uint8_t y;
};

class Zelda1WorldModel {
public:
    inline static constexpr std::size_t kSerializedSize = 340;
    inline static constexpr std::size_t kSerializedActorCountOffset = 274;
    inline static constexpr std::size_t kSerializedDropCountOffset = 275;
    inline static constexpr std::size_t kSerializedActorsOffset = 276;
    inline static constexpr std::size_t kMaxSerializedActors = 8;
    inline static constexpr std::size_t kMaxSerializedDrops = 8;
    explicit Zelda1WorldModel(const FirstQuestData& data) : data_(data) { reset(); }

    void reset() {
        overworld_flags_.fill(0); level1_flags_.fill(0); actors_.clear();
        position_ = {WorldArea::kOverworld, 0x77}; cave_return_room_ = 0x77;
        keys_ = 0; health_ = 6; inventory_bits_ = 0; tick_ = 0; load_room();
    }
    [[nodiscard]] WorldPosition position() const { return position_; }
    [[nodiscard]] std::uint8_t health() const { return health_; }
    [[nodiscard]] std::uint8_t keys() const { return keys_; }
    [[nodiscard]] const std::vector<EnemyActor>& actors() const { return actors_; }
    [[nodiscard]] const std::vector<std::uint8_t>& opaque_drops() const { return opaque_drops_; }
    [[nodiscard]] bool room_flag(std::uint8_t id) const {
        return (position_.area == WorldArea::kLevel1 ? level1_flags_ : overworld_flags_)[id] != 0;
    }

    // The source needs final tile data for intraroom collision.  This exposes
    // that limitation while still returning the source-expanded visual square.
    ModelResult square_at(std::uint8_t x, std::uint8_t y, ModelSquare* output) const {
        if (!output || x >= 16 || y >= 11 || position_.area != WorldArea::kOverworld)
            return ModelResult::kInvalid;
        OverworldRoomView room{}; OverworldSquareGrid grid{};
        if (!data_.overworld_room(position_.room_id & 15, position_.room_id >> 4, &room) ||
            !data_.overworld_square_grid(room, &grid)) return ModelResult::kInvalid;
        *output = {grid.squares[y * 16 + x].primary_square, ModelCollision::kOpaqueFinalTile};
        return ModelResult::kOpaqueCollision;
    }
    ModelResult cross_edge(Edge edge) {
        if (position_.area == WorldArea::kCave) return ModelResult::kBlocked;
        const int delta = edge == Edge::kEast ? 1 : edge == Edge::kWest ? -1 :
                          edge == Edge::kSouth ? 16 : -16;
        const int x = position_.room_id & 15, y = position_.room_id >> 4;
        if ((edge == Edge::kEast && x == 15) || (edge == Edge::kWest && x == 0) ||
            (edge == Edge::kSouth && y == 7) || (edge == Edge::kNorth && y == 0))
            return ModelResult::kBlocked;
        if (position_.area == WorldArea::kLevel1) {
            UnderworldRoomView room{};
            if (!data_.first_quest_underworld_room(1, position_.room_id, &room)) return ModelResult::kInvalid;
            const DoorType door = room.doors_e_w_s_n[static_cast<unsigned>(edge)];
            if (door == DoorType::kWall) return ModelResult::kBlocked;
            if ((door == DoorType::kKey || door == DoorType::kKey2) && keys_ == 0) return ModelResult::kNeedsKey;
            if (door == DoorType::kKey || door == DoorType::kKey2) --keys_;
        }
        position_.room_id = static_cast<std::uint8_t>(position_.room_id + delta);
        load_room(); return ModelResult::kMoved;
    }
    ModelResult enter_entrance() {
        if (position_.area != WorldArea::kOverworld) return ModelResult::kNoEntrance;
        OverworldRoomView room{};
        if (!data_.overworld_room(position_.room_id & 15, position_.room_id >> 4, &room)) return ModelResult::kInvalid;
        const auto entrance = FirstQuestData::decode_overworld_entrance(room);
        if (entrance.kind == OverworldEntranceKind::kLevel && entrance.destination_index == 1) {
            FirstQuestUnderworldLevelView level{};
            if (!data_.first_quest_underworld_level(1, &level)) return ModelResult::kInvalid;
            cave_return_room_ = position_.room_id; position_ = {WorldArea::kLevel1, level.info.start_room_id};
        } else if (entrance.kind == OverworldEntranceKind::kCave) {
            cave_return_room_ = position_.room_id; position_ = {WorldArea::kCave, entrance.destination_index};
        } else return ModelResult::kNoEntrance;
        load_room(); return ModelResult::kEntered;
    }
    ModelResult leave_cave_or_level() {
        if (position_.area != WorldArea::kCave && position_.area != WorldArea::kLevel1) return ModelResult::kInvalid;
        position_ = {WorldArea::kOverworld, cave_return_room_}; load_room(); return ModelResult::kReturned;
    }
    // Host policy supplies a key only after a source-backed pickup/drop is
    // implemented. This keeps the count serializable without pretending an
    // unknown object-list entry was a key.
    void grant_key() { if (keys_ != 255) ++keys_; }
    ModelResult collect_room_item() {
        if (position_.area != WorldArea::kLevel1 || level1_flags_[position_.room_id] & 1) return ModelResult::kInvalid;
        UnderworldRoomView room{};
        if (!data_.first_quest_underworld_room(1, position_.room_id, &room)) return ModelResult::kInvalid;
        inventory_bits_ |= (std::uint32_t{1} << room.room_item_id);
        level1_flags_[position_.room_id] |= 1; return ModelResult::kEntered;
    }
    void tick() {
        ++tick_;
        for (auto& actor : actors_) if (!actor.defeated && actor.source_template_id == 0x3d && (tick_ & 7) == 0) {
            actor.x = static_cast<std::uint8_t>(actor.x == 0xc7 ? 0x88 : actor.x + 1);
        }
    }
    ModelResult damage_actor(std::size_t index, std::uint8_t damage) {
        if (index >= actors_.size() || actors_[index].defeated || !actors_[index].hit_points_known) return ModelResult::kInvalid;
        auto& actor = actors_[index]; actor.hit_points = damage >= actor.hit_points ? 0 : actor.hit_points - damage;
        if (actor.hit_points == 0) { actor.defeated = true; level1_flags_[position_.room_id] |= 2; opaque_drops_.push_back(0xff); }
        return ModelResult::kEntered;
    }
    // Generic object lists and the ROM damage/drop tables are not imported
    // yet. These deterministic QA rules deliberately expose their fallback:
    // generic actors have one QA hit point, contact costs one QA health unit,
    // and $FF is an opaque pending drop rather than an invented item ID.
    ModelResult contact_actor(std::size_t index) {
        if (index >= actors_.size() || actors_[index].defeated || health_ == 0) return ModelResult::kInvalid;
        --health_; return ModelResult::kEntered;
    }
    ModelResult pickup_opaque_drop(std::size_t index) {
        if (index >= opaque_drops_.size()) return ModelResult::kInvalid;
        opaque_drops_.erase(opaque_drops_.begin() + static_cast<std::ptrdiff_t>(index));
        return ModelResult::kEntered;
    }
    // TEMPORARY QA route edges. They are intentionally separate from
    // cross_edge: no claim is made that this is the source's complete path.
    ModelResult qa_take_start_cave_sword() {
        if (position_.area != WorldArea::kCave || cave_return_room_ != 0x77) return ModelResult::kInvalid;
        inventory_bits_ |= 2; return ModelResult::kEntered; // QA inventory fact bit 1.
    }
    ModelResult qa_enter_level1_boss_room() {
        if (position_.area != WorldArea::kLevel1) return ModelResult::kInvalid;
        FirstQuestUnderworldLevelView level{};
        if (!data_.first_quest_underworld_level(1, &level)) return ModelResult::kInvalid;
        position_.room_id = level.info.boss_room_id; load_room(); return ModelResult::kMoved;
    }
    [[nodiscard]] std::vector<std::uint8_t> serialize() const {
        std::vector<std::uint8_t> out(kSerializedSize, 0);
        out[0]='Z'; out[1]='1'; out[2]='W'; out[3]='M'; out[4]=2; out[5]=static_cast<std::uint8_t>(position_.area); out[6]=position_.room_id; out[7]=cave_return_room_; out[8]=keys_; out[9]=health_;
        for (unsigned i=0;i<4;++i) out[10+i]=static_cast<std::uint8_t>(inventory_bits_ >> (i*8));
        for (unsigned i=0;i<128;++i) { out[14+i]=overworld_flags_[i]; out[142+i]=level1_flags_[i]; }
        for (unsigned i=0;i<4;++i) out[270+i]=static_cast<std::uint8_t>(tick_ >> (i*8));
        out[kSerializedActorCountOffset]=static_cast<std::uint8_t>(actors_.size()); out[kSerializedDropCountOffset]=static_cast<std::uint8_t>(opaque_drops_.size());
        for (std::size_t i=0;i<actors_.size();++i) { const auto& a=actors_[i]; const auto at=kSerializedActorsOffset+i*7; out[at]=a.source_template_id; out[at+1]=a.hit_points; out[at+2]=(a.hit_points_known?1:0)|(a.contact_damage_known?2:0)|(a.defeated?4:0)|(a.drop_is_opaque?8:0); out[at+3]=a.contact_damage; out[at+4]=a.x; out[at+5]=a.y; }
        for (std::size_t i=0;i<opaque_drops_.size();++i) out[kSerializedActorsOffset+kMaxSerializedActors*7+i]=opaque_drops_[i];
        return out;
    }
    // Data-independent structural gate shared by native persistence and the
    // live model.  It intentionally validates only facts encoded in the fixed
    // v2 record; room/object semantics remain the model's data-backed work.
    static bool validate_serialized(std::span<const std::uint8_t> blob) {
        if (blob.size() != kSerializedSize || blob[0] != 'Z' ||
            blob[1] != '1' || blob[2] != 'W' || blob[3] != 'M' ||
            blob[4] != 2 ||
            blob[5] > static_cast<std::uint8_t>(WorldArea::kLevel1) ||
            blob[kSerializedActorCountOffset] > kMaxSerializedActors ||
            blob[kSerializedDropCountOffset] > kMaxSerializedDrops)
            return false;
        // Current model semantics have no persisted overworld flags. Level 1
        // uses only bit 0 (room item) and bit 1 (boss defeated). Reject other
        // bits rather than treating data from a future grammar as v2 state.
        if (blob[9] > 6) return false;
        for (std::size_t i = 0; i < 128; ++i) {
            if (blob[14 + i] != 0 || (blob[142 + i] & ~std::uint8_t{3}) != 0)
                return false;
        }
        for (std::size_t i = 0; i < blob[kSerializedActorCountOffset]; ++i) {
            const auto at = kSerializedActorsOffset + i * 7;
            const auto flags = blob[at + 2];
            const bool hp_known = (flags & 1) != 0;
            const bool contact_known = (flags & 2) != 0;
            const bool defeated = (flags & 4) != 0;
            if (blob[at] > 0x7f || flags > 0x0f || blob[at + 1] > 6 ||
                blob[at + 6] != 0 || (!hp_known && blob[at + 1] != 0) ||
                (!contact_known && blob[at + 3] != 0) ||
                (defeated && (!hp_known || blob[at + 1] != 0)))
                return false;
        }
        for (std::size_t i = 0; i < blob[kSerializedDropCountOffset]; ++i) {
            if (blob[kSerializedActorsOffset + kMaxSerializedActors * 7 + i] != 0xff)
                return false;
        }
        return true;
    }
    bool restore(std::span<const std::uint8_t> blob) {
        if (!validate_serialized(blob)) return false;
        std::array<std::uint8_t,128> next_ow{}, next_l1{}; std::vector<EnemyActor> next_actors; std::vector<std::uint8_t> next_drops; std::uint32_t next_inventory=0, next_tick=0;
        for (unsigned i=0;i<4;++i) { next_inventory|=static_cast<std::uint32_t>(blob[10+i])<<(i*8); next_tick|=static_cast<std::uint32_t>(blob[270+i])<<(i*8); }
        for (unsigned i=0;i<128;++i) { next_ow[i]=blob[14+i]; next_l1[i]=blob[142+i]; }
        for (std::size_t i=0;i<blob[kSerializedActorCountOffset];++i) { const auto at=kSerializedActorsOffset+i*7; const auto flags=blob[at+2]; next_actors.push_back({blob[at],blob[at+1],(flags&1)!=0,blob[at+3],(flags&2)!=0,(flags&4)!=0,(flags&8)!=0,blob[at+4],blob[at+5]}); }
        for (std::size_t i=0;i<blob[kSerializedDropCountOffset];++i) { const auto drop=blob[kSerializedActorsOffset+kMaxSerializedActors*7+i]; next_drops.push_back(drop); }
        position_={static_cast<WorldArea>(blob[5]),blob[6]}; cave_return_room_=blob[7]; keys_=blob[8]; health_=blob[9]; inventory_bits_=next_inventory; tick_=next_tick; overworld_flags_=next_ow; level1_flags_=next_l1; actors_=std::move(next_actors); opaque_drops_=std::move(next_drops); return true;
    }
    [[nodiscard]] bool has_inventory_fact(std::uint8_t source_item_id) const { return source_item_id < 32 && (inventory_bits_ & (std::uint32_t{1} << source_item_id)) != 0; }
    [[nodiscard]] bool has_qa_start_sword() const { return has_inventory_fact(1); }
private:
    void load_room() {
        actors_.clear(); opaque_drops_.clear();
        if (position_.area != WorldArea::kLevel1) return;
        UnderworldRoomView room{};
        if (!data_.first_quest_underworld_room(1, position_.room_id, &room)) return;
        if (room.is_aquamentus && !(level1_flags_[position_.room_id] & 2))
            actors_.push_back({0x3d, 6, true, 0, false, false, true, 0xb0, 0x80});
        else if (room.object_placement.object_list_or_template_id != 0)
            actors_.push_back({room.object_placement.object_list_or_template_id, 1, true, 0, false, false, true, 0x80, 0x80});
    }
    const FirstQuestData& data_;
    std::array<std::uint8_t, 128> overworld_flags_{};
    std::array<std::uint8_t, 128> level1_flags_{};
    std::vector<EnemyActor> actors_;
    std::vector<std::uint8_t> opaque_drops_;
    WorldPosition position_{}; std::uint8_t cave_return_room_{}; std::uint8_t keys_{}; std::uint8_t health_{};
    std::uint32_t inventory_bits_{}; std::uint32_t tick_{};
};
}  // namespace minish::foreign_world::zelda1
