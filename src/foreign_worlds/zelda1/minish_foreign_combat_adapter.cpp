#include "foreign_worlds/zelda1/minish_foreign_combat_adapter.h"

#include <algorithm>
#include <string>

namespace minish::foreign_world::zelda1 {
namespace {

bool overlaps(const ForeignSwordHitbox& hitbox, const OctorokActorState& actor) {
  const int actor_left = actor.x;
  const int actor_top = actor.y;
  return hitbox.left <= actor_left + 15 && hitbox.right >= actor_left &&
         hitbox.top <= actor_top + 15 && hitbox.bottom >= actor_top;
}

}  // namespace

std::optional<ResolvedItemUse> resolve_foreign_sword(
    const InventoryCore& inventory, LoadoutId loadout) {
  // Resolve the selected loadout slot, then confirm its exact acquired record
  // has the Sword capability.  A numeric behavior ID is never used here.
  for (std::size_t slot = 0; slot < kLoadoutSlots; ++slot) {
    const auto use = inventory.resolve_loadout_use(loadout, slot);
    if (!use || use->use_kind != ItemUseKind::Equip)
      continue;
    const auto capabilities = inventory.capabilities(use->acquired_id);
    if (capabilities && has_capability(*capabilities, Capability::Sword))
      return use;
  }
  return std::nullopt;
}

bool is_active_minish_sword(const MinishSwordSample& sample) {
  // Source: itemDefinitions.c maps IDs 1..6 to ItemSword; the active behavior
  // is live only while priority is nonzero. ItemSword / playerUtils.c keep one
  // or both state bytes nonzero during a thrust, charge, or spin.
  return sample.behavior_id >= 1 && sample.behavior_id <= 6 &&
         sample.priority != 0 &&
         (sample.sword_state != 0 || sample.attack_status != 0);
}

std::optional<ForeignSwordFacing> foreign_sword_facing(
    std::uint8_t player_animation_state) {
  // Source: entity.h AnimationState and playerUtils.c sub_08077D38. The
  // snapshot is IdleNorth/East/South/West (0,2,4,6), including walking forms.
  switch (player_animation_state) {
  case 0: return ForeignSwordFacing::kNorth;
  case 2: return ForeignSwordFacing::kEast;
  case 4: return ForeignSwordFacing::kSouth;
  case 6: return ForeignSwordFacing::kWest;
  default: return std::nullopt;
  }
}

ForeignSwordHitbox foreign_sword_hitbox(std::uint8_t source_x,
                                        std::uint8_t source_y,
                                        ForeignSwordFacing facing) {
  const int x = source_x, y = source_y;
  // Host policy: a 17-pixel-wide by 24-pixel-long forward rectangle. It is
  // bounded, deterministic, and deliberately separate from TMC's native
  // PlayerItemSword hitbox data (which does not describe Zelda actors).
  switch (facing) {
  case ForeignSwordFacing::kNorth: return {static_cast<std::int16_t>(x - 8), static_cast<std::int16_t>(y - 24), static_cast<std::int16_t>(x + 8), static_cast<std::int16_t>(y - 1)};
  case ForeignSwordFacing::kEast: return {static_cast<std::int16_t>(x + 1), static_cast<std::int16_t>(y - 8), static_cast<std::int16_t>(x + 24), static_cast<std::int16_t>(y + 8)};
  case ForeignSwordFacing::kSouth: return {static_cast<std::int16_t>(x - 8), static_cast<std::int16_t>(y + 1), static_cast<std::int16_t>(x + 8), static_cast<std::int16_t>(y + 24)};
  case ForeignSwordFacing::kWest: return {static_cast<std::int16_t>(x - 24), static_cast<std::int16_t>(y - 8), static_cast<std::int16_t>(x - 1), static_cast<std::int16_t>(y + 8)};
  }
  return {};
}

ObservedNativeSwordImportResult import_observed_native_sword(
    InventoryCore* inventory, LoadoutId loadout, const MinishSwordSample& sample,
    std::string* error) {
  if (!inventory || !is_active_minish_sword(sample))
    return ObservedNativeSwordImportResult::kNotSourceSword;
  const bool selected_sword = resolve_foreign_sword(*inventory, loadout).has_value();

  // Source: include/item.h defines IDs 1..6 as the six TMC sword entries,
  // and itemDefinitions.c maps each one to ItemSword. The stable host tier is
  // intentionally that observed source ordinal; it is not a guest address.
  const CrossWorldItemId id{WorldId::Native, sample.behavior_id};
  const bool already_owned = has_flag(inventory->ownership(id), OwnershipFlags::Owned);
  const ItemTraits traits{sample.behavior_id, id, ItemUseKind::Equip,
                          ResourcePoolProvenance::None};
  if (!inventory->acquire_item(id, OwnershipFlags::Owned, Capability::Sword,
                               traits, error))
    return ObservedNativeSwordImportResult::kRejected;
  if (selected_sword) {
    return already_owned ? ObservedNativeSwordImportResult::kAlreadySelected
                         : ObservedNativeSwordImportResult::kImportedKeepingSelection;
  }

  // A full loadout is not rewritten. This import mirrors guest ownership but
  // keeps a user's other selected items intact until an explicit UI exists.
  for (std::size_t slot = 0; slot < kLoadoutSlots; ++slot) {
    if (inventory->loadout_slot(loadout, slot)) continue;
    if (!inventory->set_loadout_slot(loadout, slot, id, error))
      return ObservedNativeSwordImportResult::kRejected;
    return ObservedNativeSwordImportResult::kImportedAndSelected;
  }
  return ObservedNativeSwordImportResult::kImportedWithoutFreeSlot;
}

void MinishForeignCombatAdapter::reset() {
  prior_sword_active_ = false;
  contact_iframes_ = 0;
  host_health_ = kInitialHostHealth;
  host_damage_events_ = 0;
  collected_drop_events_ = 0;
}

ForeignCombatEvents MinishForeignCombatAdapter::tick(
    const ForeignCombatInput& input, InventoryCore* inventory,
    const Ow66OctorokRuntime& octoroks) {
  ForeignCombatEvents out{};
  out.host_health = host_health_;
  out.host_damage_events = host_damage_events_;
  out.collected_drop_events = collected_drop_events_;
  const bool active = input.session_active && input.survival_safe &&
                      input.room_id == 0x66 && octoroks.initialized();
  if (!active || !inventory) {
    prior_sword_active_ = false;
    return out;
  }
  out.tick_octoroks = true;
  const bool sword_active = is_active_minish_sword(input.sword);
  const bool sword_edge = sword_active && !prior_sword_active_;
  prior_sword_active_ = sword_active;

  if (sword_edge) {
    (void)import_observed_native_sword(inventory, input.loadout, input.sword);
  }
  // A real source ItemSword edge is authoritative for its own swing.  A
  // selected Zelda/Native inventory sword can otherwise use a read-only A
  // edge even when TMC has no native ItemSword active.  Suppressing the host
  // edge throughout an active source swing prevents two host hits for one
  // physical A press.
  const bool selected_sword_edge = input.selected_sword_edge && !sword_active;
  const bool attack_edge = sword_edge || selected_sword_edge;
  if (attack_edge && resolve_foreign_sword(*inventory, input.loadout)) {
    const std::uint8_t animation_state = sword_edge
        ? input.sword.player_animation_state
        : input.player_animation_state;
    if (const auto facing = foreign_sword_facing(animation_state)) {
      const auto hitbox = foreign_sword_hitbox(input.link_source_x,
                                                input.link_source_y, *facing);
      const auto& actors = octoroks.actors();
      for (std::size_t i = 0; i < actors.size(); ++i)
        if (!actors[i].defeated && overlaps(hitbox, actors[i]))
          out.sword_hits[out.sword_hit_count++] = i;
    }
  }

  const auto& actors = octoroks.actors();
  for (std::size_t i = 0; i < actors.size(); ++i) {
    std::uint8_t damage{};
    if (!out.link_contact && octoroks.contact(i, input.link_source_x,
                                               input.link_source_y, &damage) &&
        contact_iframes_ == 0) {
      out.link_contact = true;
      out.contact_damage = damage;
      host_health_ = static_cast<std::uint16_t>(
          damage >= host_health_ ? 0 : host_health_ - damage);
      ++host_damage_events_;
      contact_iframes_ = kContactIFrames;
    }
    // A pending drop has no source-backed item RNG/type yet. Proximity still
    // collects the session drop and records an observable host QA event.
    if (actors[i].pending_drop && input.link_source_x >= actors[i].x &&
        input.link_source_x <= actors[i].x + 15 && input.link_source_y >= actors[i].y &&
        input.link_source_y <= actors[i].y + 15)
      out.drop_collects[out.drop_collect_count++] = i;
  }
  if (contact_iframes_ != 0) --contact_iframes_;
  collected_drop_events_ += static_cast<std::uint32_t>(out.drop_collect_count);
  out.host_health = host_health_;
  out.host_damage_events = host_damage_events_;
  out.collected_drop_events = collected_drop_events_;
  return out;
}

}  // namespace minish::foreign_world::zelda1
