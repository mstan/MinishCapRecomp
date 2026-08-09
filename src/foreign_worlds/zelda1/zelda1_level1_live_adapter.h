#pragma once

#include "foreign_worlds/foreign_world.h"
#include "foreign_worlds/zelda1/minish_foreign_combat_adapter.h"
#include "foreign_worlds/zelda1/zelda1_level1_session.h"
#include "foreign_worlds/zelda1/zelda1_overworld_session.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace minish::foreign_world::zelda1 {

// Pure controller result for the future plugin seam. No value here represents
// a guest register, entity pointer, or guest-world mutation.
enum class Level1LiveResult : std::uint8_t {
    kEntered, kMoved, kBlocked, kNeedsKey, kNeedsBomb, kSecretUnresolved,
    kCollected, kSwordHit, kBossDefeated, kReturned, kNoItem, kNotActive,
    kWrongOverworldContext, kNotAtSourceHotspot, kInventoryRejected, kInvalid,
};

struct Level1PlayerPresentation {
    std::int16_t x = 120;
    std::int16_t y = 149;
};

// Minimal read-only integration data for the plugin. The plugin may publish
// framebuffer/player presentation via trusted engine facilities, but this
// adapter has no guest-bus dependency and performs no guest writes or PC
// control.
struct Level1LivePresentation {
    const OwFramebuffer* framebuffer = nullptr;
    Level1PlayerPresentation player{};
    std::uint8_t room_id = 0;
    bool active = false;
};

class Zelda1Level1LiveAdapter {
public:
    inline static constexpr std::size_t kSerializedSize = 224;

    // Checks the same source warp alignment used by the OW cave entry: the
    // exact source-final warp tile plus X=$?0/Y=$?D alignment. It also checks
    // OW $37's First Quest Level 1 AttrB entrance record before constructing
    // the bounded Level 1 session.
    Level1LiveResult try_enter_from_overworld(
        const Zelda1OverworldSession& overworld);

    // Host policy while the source UW collision metasystem is not decoded:
    // move pixel-by-pixel inside the 240x160 crop, clamp at its bounds, and
    // submit a source room-edge transition only after the presentation feet
    // reach that edge. It deliberately does not claim intra-room collision.
    // The Zelda1 resource pool is staged alongside the child room state.
    // A source key door decrements it exactly when the child consumes a key;
    // Native resource pools and loadout selection are never touched.
    Level1LiveResult move_by(std::int16_t dx, std::int16_t dy,
                             foreign_world::InventoryCore* inventory,
                             foreign_world::LoadoutId loadout,
                             std::uint8_t* returned_overworld_room = nullptr);

    // Copies source raw item identities to InventoryCore as Zelda1-origin
    // records. Keys additionally update the independent Zelda1 resource pool.
    Level1LiveResult collect_room_item(foreign_world::InventoryCore* inventory,
                                       std::string* error = nullptr);
    // Processes either one source-observed Minish ItemSword rising edge or a
    // read-only selected-loadout A edge. The latter is host-only and is
    // suppressed while an ItemSword is active, so one physical swing cannot
    // apply duplicate boss damage. The host hitbox policy is the same bounded
    // rectangle already used for OW Octoroks.
    Level1LiveResult observe_sword(const MinishSwordSample& sword,
                                   foreign_world::InventoryCore* inventory,
                                   foreign_world::LoadoutId loadout,
                                   bool selected_sword_edge = false,
                                   std::uint8_t player_animation_state = 0,
                                   std::string* error = nullptr);

    [[nodiscard]] bool active() const { return session_ && session_->active(); }
    [[nodiscard]] const Zelda1Level1Session* session() const {
        return session_ ? &*session_ : nullptr;
    }
    [[nodiscard]] Level1PlayerPresentation player_presentation() const {
        return player_;
    }
    [[nodiscard]] Level1LivePresentation presentation() const;
    void reset();

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    // Requires the caller's currently loaded, already identity-validated OW
    // session. Restore stages and renders a Level 1 candidate before commit.
    bool restore(const Zelda1OverworldSession& overworld,
                 std::span<const std::uint8_t> blob,
                 std::string* error = nullptr);

private:
    [[nodiscard]] static bool source_level1_hotspot(
        const Zelda1OverworldSession& overworld);
    [[nodiscard]] static Level1LiveResult map(Level1Result result);
    [[nodiscard]] static bool overlaps_aquamentus(const ForeignSwordHitbox& hitbox);
    [[nodiscard]] bool overlaps_room_item(const Level1RoomItem& item) const;
    [[nodiscard]] bool clone_inventory(const foreign_world::InventoryCore& source,
                                       foreign_world::InventoryCore* out,
                                       std::string* error) const;
    [[nodiscard]] bool clone_session(Zelda1Level1Session* out) const;
    [[nodiscard]] Level1LiveResult move_axis(
        std::int16_t distance, bool horizontal,
        foreign_world::InventoryCore* inventory,
        foreign_world::LoadoutId loadout,
        std::uint8_t* returned_overworld_room);

    const FirstQuestData* data_ = nullptr;
    std::optional<Zelda1Level1Session> session_;
    Level1PlayerPresentation player_{};
    bool prior_sword_active_ = false;
};

}  // namespace minish::foreign_world::zelda1
