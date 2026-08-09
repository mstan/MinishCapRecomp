#pragma once

#include "foreign_worlds/foreign_world.h"
#include "foreign_worlds/zelda1/zelda1_octorok_runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace minish::foreign_world::zelda1 {

// Read-only view of TMC's active sword behavior.  The offsets are owned by
// the live plugin: gActiveItems[0].behaviorId/priority/playerAnimationState,
// gPlayerState.sword_state, and gPlayerState.attack_status.  In the pinned
// source, ItemSword is behavior IDs 1..6 and sub_08077D38 snapshots the
// player's animation state into this behavior before it starts an attack.
struct MinishSwordSample {
  std::uint8_t behavior_id{};
  std::uint8_t priority{};
  std::uint8_t player_animation_state{};
  std::uint8_t sword_state{};
  std::uint8_t attack_status{};
};

enum class ForeignSwordFacing : std::uint8_t { kNorth, kEast, kSouth, kWest };

// This rectangle is source-coordinate based (the same coordinates used by
// Zelda 1's OW actor roster). It is a deliberately small host policy rather
// than a claim that TMC and Zelda 1 have an identical hitbox.
struct ForeignSwordHitbox {
  std::int16_t left{};
  std::int16_t top{};
  std::int16_t right{};
  std::int16_t bottom{};
};

struct ForeignCombatInput {
  bool session_active{};
  bool survival_safe{};
  std::uint8_t room_id{};
  std::uint8_t link_source_x{};
  std::uint8_t link_source_y{};
  MinishSwordSample sword{};
  LoadoutId loadout = LoadoutId::A;
  // An active-low KEYINPUT A rising edge sampled by the plugin.  This is a
  // host-side use request only: it cannot cause a guest item, animation, or
  // story write.
  bool selected_sword_edge{};
  // Source: gPlayerEntity.base.animationState (Entity + 0x14).  This is used
  // only for the selected-inventory host edge when no ItemSword is active;
  // an active ItemSword retains its source-snapshotted facing instead.
  std::uint8_t player_animation_state{};
  // Entity::direction is the source 0..31 LinearMoveDirectionOLD domain.
  // It is the host sword's fallback during transient walking animations.
  std::uint8_t player_direction{};
};

struct ForeignCombatEvents {
  bool tick_octoroks{};
  std::array<std::size_t, Ow66OctorokRuntime::kActorCount> sword_hits{};
  std::size_t sword_hit_count{};
  std::array<std::size_t, Ow66OctorokRuntime::kActorCount> drop_collects{};
  std::size_t drop_collect_count{};
  bool link_contact{};
  std::uint8_t contact_damage{};
  std::uint16_t host_health{};
  std::uint32_t host_damage_events{};
  std::uint32_t collected_drop_events{};
};

// Resolve through InventoryCore's exact-origin records and selected loadout.
// This never compares an item number alone, and makes either a Native or
// Zelda1 sword usable in a Zelda frame when its behavior is Equip.
[[nodiscard]] std::optional<ResolvedItemUse> resolve_foreign_sword(
    const InventoryCore& inventory, LoadoutId loadout);
[[nodiscard]] bool is_active_minish_sword(const MinishSwordSample& sample);
[[nodiscard]] std::optional<ForeignSwordFacing> foreign_sword_facing(
    std::uint8_t player_animation_state);
[[nodiscard]] std::optional<ForeignSwordFacing> foreign_sword_facing_from_direction(
    std::uint8_t direction);
[[nodiscard]] ForeignSwordHitbox foreign_sword_hitbox(
    std::uint8_t source_x, std::uint8_t source_y, ForeignSwordFacing facing);

enum class ObservedNativeSwordImportResult : std::uint8_t {
  kNotSourceSword,
  kAlreadySelected,
  kImportedKeepingSelection,
  kImportedAndSelected,
  kImportedWithoutFreeSlot,
  kRejected,
};

// TMC's active ItemBehavior proves that a Native sword is already equipped in
// the guest. Mirror only that exact, origin-qualified acquisition into the
// host inventory; it never writes guest flags, equipment, or story state.
// An existing compatible selected item (including Zelda1) wins unchanged, but
// does not suppress mirroring the independently owned Native acquisition.
[[nodiscard]] ObservedNativeSwordImportResult import_observed_native_sword(
    InventoryCore* inventory, LoadoutId loadout, const MinishSwordSample& sample,
    std::string* error = nullptr);

// Pure host combat policy. It has no guest pointer, bus access, callback, or
// write capability. The plugin performs the resulting session operations only
// after this calculation succeeds.
class MinishForeignCombatAdapter {
 public:
  static constexpr std::uint16_t kInitialHostHealth = 6;
  static constexpr unsigned kContactIFrames = 30;

  void reset();
  [[nodiscard]] ForeignCombatEvents tick(const ForeignCombatInput& input,
                                          InventoryCore* inventory,
                                          const Ow66OctorokRuntime& octoroks);

 private:
  bool prior_sword_active_{};
  unsigned contact_iframes_{};
  std::uint16_t host_health_ = kInitialHostHealth;
  std::uint32_t host_damage_events_{};
  std::uint32_t collected_drop_events_{};
};

}  // namespace minish::foreign_world::zelda1
