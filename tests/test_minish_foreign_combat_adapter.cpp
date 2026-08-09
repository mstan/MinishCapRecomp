#include "foreign_worlds/foreign_world.h"
#include "foreign_worlds/zelda1/minish_foreign_combat_adapter.h"

#include <array>
#include <iostream>
#include <string>

namespace fw = minish::foreign_world;
namespace z1 = minish::foreign_world::zelda1;

namespace {
int fail(const std::string& message) { std::cerr << "FAIL: " << message << "\n"; return 1; }

bool add_sword(fw::InventoryCore* inventory, fw::CrossWorldItemId id,
               fw::LoadoutId loadout, std::size_t slot) {
  std::string error;
  const fw::ItemTraits traits{1, id, fw::ItemUseKind::Equip,
                              fw::ResourcePoolProvenance::None};
  return inventory->set_item(id, fw::OwnershipFlags::Owned,
                             fw::Capability::Sword, traits, &error) &&
         inventory->set_loadout_slot(loadout, slot, id, &error);
}

bool restore_hit_test_octoroks(z1::Ow66OctorokRuntime* octoroks) {
  if (!octoroks) return false;
  // Actor zero is a one-hit red Octorok directly north of source (100,124).
  const std::array<std::uint8_t, z1::Ow66OctorokRuntime::kSerializedSize> bytes{
      'Z','1','O','R',1,0,0,0,0,
      7,1,100,100,0, 7,1,180,100,0, 8,3,180,120,0, 8,3,200,130,0, 0,0,0};
  return octoroks->restore(bytes);
}
}

int main() {
  fw::InventoryCore inventory;
  const fw::CrossWorldItemId native_sword{fw::WorldId::Native, 7};
  const fw::CrossWorldItemId zelda_sword{fw::WorldId::Zelda1, 1};
  if (!add_sword(&inventory, native_sword, fw::LoadoutId::A, 0) ||
      !add_sword(&inventory, zelda_sword, fw::LoadoutId::B, 1))
    return fail("could not create exact-origin sword inventory");
  const auto native_use = z1::resolve_foreign_sword(inventory, fw::LoadoutId::A);
  const auto zelda_use = z1::resolve_foreign_sword(inventory, fw::LoadoutId::B);
  if (!native_use || !zelda_use || native_use->acquired_id != native_sword ||
      zelda_use->acquired_id != zelda_sword)
    return fail("sword resolver collapsed origin-qualified loadout items");

  fw::InventoryCore zelda_selected;
  if (!add_sword(&zelda_selected, zelda_sword, fw::LoadoutId::A, 0) ||
      z1::import_observed_native_sword(
          &zelda_selected, fw::LoadoutId::A, {1, 7, 0, 1, 0}) !=
      z1::ObservedNativeSwordImportResult::kImportedKeepingSelection ||
      zelda_selected.loadout_slot(fw::LoadoutId::A, 0) != zelda_sword ||
      zelda_selected.ownership({fw::WorldId::Native, 1}) !=
          fw::OwnershipFlags::Owned ||
      !zelda_selected.traits(zelda_sword) ||
      !zelda_selected.traits({fw::WorldId::Native, 1}) ||
      zelda_selected.traits(zelda_sword)->behavior_id != zelda_sword ||
      zelda_selected.traits({fw::WorldId::Native, 1})->behavior_id !=
          fw::CrossWorldItemId{fw::WorldId::Native, 1} ||
      zelda_selected.traits({fw::WorldId::Native, 1})->tier != 1)
    return fail("observed Native sword overwrote a selected Zelda sword");
  fw::InventoryCore native_selected;
  if (!add_sword(&native_selected, {fw::WorldId::Native, 1},
                 fw::LoadoutId::A, 0) ||
      z1::import_observed_native_sword(
          &native_selected, fw::LoadoutId::A, {6, 7, 0, 1, 0}) !=
          z1::ObservedNativeSwordImportResult::kImportedKeepingSelection ||
      native_selected.loadout_slot(fw::LoadoutId::A, 0) !=
          fw::CrossWorldItemId{fw::WorldId::Native, 1} ||
      native_selected.ownership({fw::WorldId::Native, 6}) !=
          fw::OwnershipFlags::Owned ||
      !native_selected.traits({fw::WorldId::Native, 6}) ||
      native_selected.traits({fw::WorldId::Native, 6})->tier != 6)
    return fail("new observed Native sword tier was not acquired distinctly");
  fw::InventoryCore rejected_import;
  if (z1::import_observed_native_sword(
          &rejected_import, fw::LoadoutId::A, {7, 7, 0, 1, 0}) !=
          z1::ObservedNativeSwordImportResult::kNotSourceSword ||
      rejected_import.ownership({fw::WorldId::Native, 7}) !=
          fw::OwnershipFlags::None)
    return fail("non-sword guest behavior was imported as a Native sword");

  z1::Ow66OctorokRuntime octoroks;
  if (!restore_hit_test_octoroks(&octoroks)) return fail("could not restore test Octoroks");

  z1::MinishForeignCombatAdapter adapter;
  fw::InventoryCore observed_native_inventory;
  z1::ForeignCombatInput input{true, true, 0x66, 100, 124,
      {1, 7, 0, 1, 0}, fw::LoadoutId::A, false, 0};
  const auto first = adapter.tick(input, &observed_native_inventory, octoroks);
  if (!first.tick_octoroks || first.sword_hit_count != 1 ||
      first.sword_hits[0] != 0 ||
      !z1::resolve_foreign_sword(observed_native_inventory, fw::LoadoutId::A) ||
      observed_native_inventory.ownership({fw::WorldId::Native, 1}) !=
          fw::OwnershipFlags::Owned)
    return fail("source sword edge did not produce one bounded north hit");
  const auto held = adapter.tick(input, &observed_native_inventory, octoroks);
  if (held.sword_hit_count != 0)
    return fail("held sword state applied more than one hit per swing");
  input.sword = {};
  (void)adapter.tick(input, &observed_native_inventory, octoroks);
  input.sword = {1, 7, 2, 1, 0};
  input.link_source_x = 76;
  input.link_source_y = 100;
  const auto east = adapter.tick(input, &observed_native_inventory, octoroks);
  if (east.sword_hit_count != 1 || east.sword_hits[0] != 0)
    return fail("east-facing hitbox did not remain deterministic");

  // A selected Zelda sword is usable through a host-only A edge even when
  // TMC has no ItemSword behavior active. Its record remains origin-distinct
  // from an observed Native/1, despite the shared numeric item number.
  fw::InventoryCore host_zelda_inventory;
  if (!add_sword(&host_zelda_inventory, zelda_sword, fw::LoadoutId::A, 0))
    return fail("could not create selected Zelda sword fixture");
  z1::Ow66OctorokRuntime host_edge_octoroks;
  if (!restore_hit_test_octoroks(&host_edge_octoroks))
    return fail("could not restore host-edge Octoroks");
  z1::MinishForeignCombatAdapter host_edge_adapter;
  z1::ForeignCombatInput host_edge{true, true, 0x66, 100, 124,
      {}, fw::LoadoutId::A, true, 0};
  const auto host_hit = host_edge_adapter.tick(host_edge, &host_zelda_inventory,
                                               host_edge_octoroks);
  if (host_hit.sword_hit_count != 1 || host_hit.sword_hits[0] != 0 ||
      !z1::resolve_foreign_sword(host_zelda_inventory, fw::LoadoutId::A) ||
      host_zelda_inventory.loadout_slot(fw::LoadoutId::A, 0) != zelda_sword ||
      host_zelda_inventory.ownership({fw::WorldId::Native, 1}) !=
          fw::OwnershipFlags::None)
    return fail("selected Zelda sword was not host-usable without a native ItemSword");
  host_edge.selected_sword_edge = false;
  if (host_edge_adapter.tick(host_edge, &host_zelda_inventory, host_edge_octoroks)
          .sword_hit_count != 0)
    return fail("held host A state applied more than one Zelda sword hit");
  z1::Ow66OctorokRuntime overlapping_source_octoroks;
  if (!restore_hit_test_octoroks(&overlapping_source_octoroks))
    return fail("could not restore overlapping source Octoroks");
  host_edge.selected_sword_edge = true;
  host_edge.sword = {1, 7, 0, 1, 0};
  const auto source_over_host = host_edge_adapter.tick(
      host_edge, &host_zelda_inventory, overlapping_source_octoroks);
  if (source_over_host.sword_hit_count != 1 ||
      host_zelda_inventory.ownership({fw::WorldId::Native, 1}) !=
          fw::OwnershipFlags::Owned ||
      host_zelda_inventory.ownership(zelda_sword) != fw::OwnershipFlags::Owned ||
      host_zelda_inventory.loadout_slot(fw::LoadoutId::A, 0) != zelda_sword ||
      !host_zelda_inventory.traits({fw::WorldId::Native, 1}) ||
      !host_zelda_inventory.traits(zelda_sword) ||
      host_zelda_inventory.traits({fw::WorldId::Native, 1})->behavior_id ==
          host_zelda_inventory.traits(zelda_sword)->behavior_id)
    return fail("source sword did not suppress duplicate host edge or preserve same-number origins");

  input.sword = {};
  input.link_source_x = 101;
  input.link_source_y = 101;
  const auto contact = adapter.tick(input, &observed_native_inventory, octoroks);
  if (!contact.link_contact || contact.contact_damage != 1 ||
      contact.host_health != z1::MinishForeignCombatAdapter::kInitialHostHealth - 1)
    return fail("Octorok contact did not record host-only damage");
  const auto protected_contact = adapter.tick(input, &observed_native_inventory, octoroks);
  if (protected_contact.link_contact || protected_contact.host_damage_events != 1)
    return fail("host contact cooldown did not prevent repeated damage");
  if (!octoroks.sword_hit(0, 1))
    return fail("could not create a source-runtime pending drop");
  const auto drop = adapter.tick(input, &observed_native_inventory, octoroks);
  if (drop.drop_collect_count != 1 || drop.drop_collects[0] != 0 ||
      drop.collected_drop_events != 1)
    return fail("pending Zelda drop was not collected through proximity");

  const auto invalid_facing = z1::foreign_sword_facing(1);
  if (invalid_facing || z1::is_active_minish_sword({1, 0, 0, 1, 0}) ||
      z1::is_active_minish_sword({7, 7, 0, 1, 0}))
    return fail("unproven source item/facing was accepted");
  return 0;
}
