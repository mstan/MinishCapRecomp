#include "foreign_worlds/foreign_world.h"

#include <iostream>
#include <string>
#include <vector>

namespace fw = minish::foreign_world;

namespace {

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

class NativeWorld final : public fw::WorldLifecycle {
public:
    bool enter(const fw::WorldPosition& entry, std::string* error) override {
        entry_ = entry;
        ++enters;
        if (!enter_succeeds) {
            if (error) *error = "native rollback unavailable";
            return false;
        }
        return true;
    }
    bool leave(const fw::ReturnAnchor&, std::string*) override {
        ++leaves;
        return true;
    }
    void tick() override { ++ticks; }

    fw::WorldPosition entry_{};
    int enters = 0;
    int leaves = 0;
    int ticks = 0;
    bool enter_succeeds = true;
};

class FailingEnterWorld final : public fw::WorldLifecycle {
public:
    bool enter(const fw::WorldPosition&, std::string* error) override {
        ++enters;
        if (error) *error = "synthetic destination rejected entry";
        return false;
    }
    bool leave(const fw::ReturnAnchor&, std::string*) override {
        ++leaves;
        return true;
    }
    void tick() override { ++ticks; }

    int enters = 0;
    int leaves = 0;
    int ticks = 0;
};

int test_registry_and_proof_world() {
    NativeWorld native;
    fw::TwoRoomProofWorld proof;
    fw::WorldRegistry registry;
    std::string error;
    if (!registry.register_world(fw::WorldId::Native, native, &error) ||
        !registry.register_world(fw::WorldId::Test, proof, &error))
        return fail("could not register ROM-free worlds: " + error);

    const fw::WorldPosition native_position{99, 11, 12};
    const fw::ReturnAnchor anchor{fw::WorldId::Native, native_position};
    if (!registry.transition_to(fw::WorldId::Test,
                                fw::TwoRoomProofWorld::kRoomAEntry,
                                anchor, &error))
        return fail("could not enter proof world: " + error);
    fw::WorldPosition landing;
    if (!proof.take_door(fw::ProofDoor::East, &landing, &error) ||
        landing != fw::TwoRoomProofWorld::kRoomBEntry ||
        proof.take_door(fw::ProofDoor::East, &landing, &error))
        return fail("proof world transitions were not deterministic");
    registry.set_current_position(landing);
    registry.tick();
    if (proof.ticks() != 1 ||
        registry.current_position() != fw::TwoRoomProofWorld::kRoomBEntry ||
        !registry.return_to_anchor(&error) ||
        registry.current_world() != fw::WorldId::Native ||
        registry.current_position() != native_position || native.enters != 1)
        return fail("return anchor lifecycle did not restore the native world");
    return 0;
}

int test_lifecycle_rollback() {
    const fw::WorldPosition old_position{50, 6, 7};
    const fw::WorldPosition rejected_entry{90, 1, 2};
    const fw::ReturnAnchor anchor{fw::WorldId::Native, old_position};

    NativeWorld native;
    FailingEnterWorld failing;
    fw::WorldRegistry registry;
    std::string error;
    if (!registry.register_world(fw::WorldId::Native, native, &error) ||
        !registry.register_world(fw::WorldId::Zelda1, failing, &error))
        return fail("could not register rollback test worlds: " + error);
    registry.set_current_position(old_position);
    if (registry.transition_to(fw::WorldId::Zelda1, rejected_entry, anchor, &error) ||
        registry.current_world() != fw::WorldId::Native ||
        registry.current_position() != old_position || native.leaves != 1 ||
        native.enters != 1 || native.entry_ != old_position ||
        error.find("previous world restored") == std::string::npos)
        return fail("failed destination enter did not restore the prior lifecycle");

    native.enter_succeeds = false;
    if (registry.transition_to(fw::WorldId::Zelda1, rejected_entry, anchor, &error) ||
        registry.current_world() != fw::WorldId::Native ||
        error.find("destination enter failed") == std::string::npos ||
        error.find("rollback to previous world failed") == std::string::npos)
        return fail("failed rollback did not report both lifecycle failures");

    NativeWorld return_native;
    fw::TwoRoomProofWorld proof;
    FailingEnterWorld return_destination;
    fw::WorldRegistry return_registry;
    const fw::ReturnAnchor failed_return{fw::WorldId::Zelda1, rejected_entry};
    if (!return_registry.register_world(fw::WorldId::Native, return_native, &error) ||
        !return_registry.register_world(fw::WorldId::Test, proof, &error) ||
        !return_registry.register_world(fw::WorldId::Zelda1, return_destination, &error) ||
        !return_registry.transition_to(fw::WorldId::Test,
                                       fw::TwoRoomProofWorld::kRoomAEntry,
                                       failed_return, &error))
        return fail("could not prepare failed-return test: " + error);
    fw::WorldPosition landing;
    if (!proof.take_door(fw::ProofDoor::East, &landing, &error))
        return fail("could not move rollback proof world: " + error);
    return_registry.set_current_position(landing);
    if (return_registry.return_to_anchor(&error) ||
        return_registry.current_world() != fw::WorldId::Test ||
        return_registry.current_position() != landing || proof.position() != landing ||
        !return_registry.return_anchor() ||
        error.find("previous world restored") == std::string::npos)
        return fail("failed return enter did not restore the active foreign world");
    return 0;
}

int test_inventory_and_serialization() {
    fw::InventoryCore inventory;
    std::string error;
    const fw::CrossWorldItemId native_sword{fw::WorldId::Native, 7};
    const fw::CrossWorldItemId zelda_sword{fw::WorldId::Zelda1, 7};
    const fw::ItemTraits native_traits{
        1, {fw::WorldId::Native, 100}, fw::ItemUseKind::Equip,
        fw::ResourcePoolProvenance::None};
    const fw::ItemTraits zelda_traits{
        4, {fw::WorldId::Zelda1, 101}, fw::ItemUseKind::Equip,
        fw::ResourcePoolProvenance::Zelda1};
    if (!inventory.set_item(native_sword, fw::OwnershipFlags::Owned,
                            fw::Capability::Sword, native_traits, &error) ||
        !inventory.set_item(zelda_sword,
                            fw::OwnershipFlags::Owned | fw::OwnershipFlags::Quest,
                            fw::Capability::Sword | fw::Capability::Bomb,
                            zelda_traits, &error) ||
        !inventory.set_loadout_slot(fw::LoadoutId::A, 0, native_sword, &error) ||
        !inventory.set_loadout_slot(fw::LoadoutId::B, 0, zelda_sword, &error))
        return fail("could not build dual-origin inventory: " + error);

    auto providers = inventory.capability_providers(fw::LoadoutId::B,
                                                     fw::Capability::Sword);
    if (providers.size() != 1 || providers.front() != zelda_sword ||
        inventory.has_capability(fw::LoadoutId::A, fw::Capability::Bomb) ||
        inventory.traits(native_sword) != native_traits ||
        inventory.traits(zelda_sword) != zelda_traits ||
        inventory.capabilities(zelda_sword) !=
            (fw::Capability::Sword | fw::Capability::Bomb))
        return fail("capability query merged same-number items across origins");

    const auto forged_loadout = static_cast<fw::LoadoutId>(99);
    if (inventory.set_loadout_slot(forged_loadout, 0, native_sword, &error) ||
        inventory.loadout_slot(forged_loadout, 0) ||
        !inventory.capability_providers(forged_loadout, fw::Capability::Sword).empty() ||
        inventory.has_capability(forged_loadout, fw::Capability::Sword))
        return fail("forged loadout id was not safely rejected");

    inventory.native_resources() = {6, 6, 20, 3, 0, 0};
    inventory.zelda1_resources() = {9, 12, 255, 8, 4, 2};
    const auto bytes = inventory.serialize();
    fw::InventoryCore loaded;
    if (!fw::InventoryCore::deserialize(bytes, &loaded, &error) ||
        loaded.loadout_slot(fw::LoadoutId::A, 0) != native_sword ||
        loaded.loadout_slot(fw::LoadoutId::B, 0) != zelda_sword ||
        loaded.traits(native_sword) != native_traits ||
        loaded.traits(zelda_sword) != zelda_traits ||
        loaded.native_resources() != inventory.native_resources() ||
        loaded.zelda1_resources() != inventory.zelda1_resources())
        return fail("inventory round trip lost origin, loadout, or resources: " + error);

    const auto preserved = loaded.serialize();
    std::vector<std::uint8_t> corrupt = bytes;
    corrupt[0] = 0;
    if (fw::InventoryCore::deserialize(corrupt, &loaded, &error) ||
        loaded.serialize() != preserved)
        return fail("bad inventory magic was accepted");
    corrupt = bytes;
    corrupt[4] = 4;
    if (fw::InventoryCore::deserialize(corrupt, &loaded, &error) ||
        loaded.serialize() != preserved)
        return fail("unknown inventory version was accepted");
    corrupt.pop_back();
    if (fw::InventoryCore::deserialize(corrupt, &loaded, &error) ||
        loaded.serialize() != preserved)
        return fail("truncated inventory payload was accepted");
    // Header is 8 bytes and an item record is 19 bytes. The first record's
    // capability starts at 14, tier at 18, behavior key at 20, use kind at
    // 25, resource-pool provenance at 26, and A's persisted ID at 46.
    corrupt = bytes;
    corrupt[14] = 0x80;
    if (fw::InventoryCore::deserialize(corrupt, &loaded, &error) ||
        loaded.serialize() != preserved)
        return fail("unknown capability bit was accepted");
    corrupt = bytes;
    corrupt[18] = 0xff;
    corrupt[19] = 0xff;
    if (fw::InventoryCore::deserialize(corrupt, &loaded, &error) ||
        loaded.serialize() != preserved)
        return fail("out-of-range item tier was accepted");
    corrupt = bytes;
    corrupt[20] = 0xff;
    if (fw::InventoryCore::deserialize(corrupt, &loaded, &error) ||
        loaded.serialize() != preserved)
        return fail("unknown item behavior origin was accepted");
    corrupt = bytes;
    corrupt[25] = 0xff;
    if (fw::InventoryCore::deserialize(corrupt, &loaded, &error) ||
        loaded.serialize() != preserved)
        return fail("unknown item use kind was accepted");
    corrupt = bytes;
    corrupt[26] = 0xff;
    if (fw::InventoryCore::deserialize(corrupt, &loaded, &error) ||
        loaded.serialize() != preserved)
        return fail("unknown resource-pool provenance was accepted");
    corrupt = bytes;
    corrupt[46] = 99;
    if (fw::InventoryCore::deserialize(corrupt, &loaded, &error) ||
        loaded.serialize() != preserved)
        return fail("forged persisted loadout id was accepted");
    return 0;
}

int test_cross_world_inventory_use_resolution() {
    fw::InventoryCore inventory;
    std::string error;
    const fw::CrossWorldItemId native_bomb{fw::WorldId::Native, 42};
    const fw::CrossWorldItemId zelda_bomb{fw::WorldId::Zelda1, 42};
    const fw::ItemTraits native_traits{
        2, {fw::WorldId::Native, 0x701}, fw::ItemUseKind::Consume,
        fw::ResourcePoolProvenance::Native};
    const fw::ItemTraits zelda_traits{
        5, {fw::WorldId::Zelda1, 0x701}, fw::ItemUseKind::Consume,
        fw::ResourcePoolProvenance::Zelda1};

    // Equal numeric item IDs are separate acquisition keys. Reacquiring the
    // Native ID may add a Quest flag, but it cannot replace its behavior with
    // Zelda's trait record.
    if (!inventory.acquire_item(native_bomb, fw::OwnershipFlags::Owned,
                                fw::Capability::Bomb, native_traits, &error) ||
        !inventory.acquire_item(zelda_bomb,
                                fw::OwnershipFlags::Owned | fw::OwnershipFlags::Quest,
                                fw::Capability::Bomb, zelda_traits, &error) ||
        !inventory.acquire_item(native_bomb,
                                fw::OwnershipFlags::Owned | fw::OwnershipFlags::Quest,
                                fw::Capability::Bomb, native_traits, &error) ||
        inventory.acquire_item(native_bomb, fw::OwnershipFlags::Owned,
                               fw::Capability::Bomb, zelda_traits, &error)) {
        return fail("origin-qualified acquisition did not preserve distinct records");
    }
    if (!fw::has_flag(inventory.ownership(native_bomb), fw::OwnershipFlags::Quest) ||
        !fw::has_flag(inventory.ownership(zelda_bomb), fw::OwnershipFlags::Quest) ||
        inventory.traits(native_bomb) != native_traits ||
        inventory.traits(zelda_bomb) != zelda_traits) {
        return fail("same-number acquisition collapsed origin-qualified flags or traits");
    }

    // Both origins can appear in one loadout; use dispatch deliberately has no
    // active-world parameter, so a native adapter and Zelda adapter receive
    // the same exact identities rather than a world-filtered item number.
    if (!inventory.set_loadout_slot(fw::LoadoutId::A, 0, native_bomb, &error) ||
        !inventory.set_loadout_slot(fw::LoadoutId::A, 1, zelda_bomb, &error)) {
        return fail("cross-world loadout selection rejected an owned exact key");
    }
    const auto native_use = inventory.resolve_loadout_use(fw::LoadoutId::A, 0);
    const auto zelda_use = inventory.resolve_loadout_use(fw::LoadoutId::A, 1);
    if (!native_use || !zelda_use || native_use->acquired_id != native_bomb ||
        zelda_use->acquired_id != zelda_bomb ||
        native_use->behavior_id != native_traits.behavior_id ||
        zelda_use->behavior_id != zelda_traits.behavior_id ||
        native_use->resource_pool != fw::ResourcePoolProvenance::Native ||
        zelda_use->resource_pool != fw::ResourcePoolProvenance::Zelda1 ||
        inventory.resolve_loadout_use(fw::LoadoutId::A, fw::kLoadoutSlots)) {
        return fail("cross-world use did not retain exact behavior/resource provenance");
    }

    inventory.native_resources().bombs = 3;
    inventory.zelda1_resources().bombs = 9;
    auto* native_pool = inventory.resource_pool_for(native_use->resource_pool);
    auto* zelda_pool = inventory.resource_pool_for(zelda_use->resource_pool);
    if (!native_pool || !zelda_pool || native_pool == zelda_pool ||
        inventory.resource_pool_for(fw::ResourcePoolProvenance::None) != nullptr) {
        return fail("resource provenance returned an implicit or shared pool");
    }
    --native_pool->bombs;  // Native behavior is selected in the shared loadout.
    --zelda_pool->bombs;   // Zelda behavior is selected in that same loadout.
    if (inventory.native_resources().bombs != 2 ||
        inventory.zelda1_resources().bombs != 8) {
        return fail("cross-world item use silently shared resource pools");
    }
    return 0;
}

}  // namespace

int main() {
    if (const int result = test_registry_and_proof_world()) return result;
    if (const int result = test_lifecycle_rollback()) return result;
    if (const int result = test_inventory_and_serialization()) return result;
    if (const int result = test_cross_world_inventory_use_resolution()) return result;
    std::cout << "Foreign-world registry, dual-origin inventory, and safe save format passed\n";
    return 0;
}
