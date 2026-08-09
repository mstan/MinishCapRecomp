#include "foreign_worlds/zelda1/smith_yard_readiness_plugin.h"

#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace minish::foreign_world::zelda1 {
namespace {

// Source: pinned zeldaret/tmc linker.ld and include/room.h.
constexpr std::uint32_t kRoomControls = 0x03000BF0;
constexpr std::uint32_t kRoomControlsArea = kRoomControls + 0x04;
constexpr std::uint32_t kRoomControlsRoom = kRoomControls + 0x05;
constexpr std::uint32_t kRoomTransition = 0x030010A0;
constexpr std::uint32_t kTransitioningOut = kRoomTransition + 0x08;
constexpr std::uint32_t kUpdateEntities = 0x0805E5C0;

SmithYardReadinessTracker g_tracker;
bool g_asset_available = false;

int observe_update_entities(std::uint32_t, int, ArmCpuState*) {
    // Read only documented global fields through the runtime's guest bus API.
    // Do not allocate entities, call guest code, or modify ArmCpuState.
    if (g_asset_available) {
        g_tracker.observe({
            true,
            bus_read_u8(kRoomControlsArea),
            bus_read_u8(kRoomControlsRoom),
            bus_read_u8(kTransitioningOut) != 0,
            NormalControlGate::Unknown,
        });
    }
    return 0;  // Observe only: always continue through UpdateEntities.
}

}  // namespace

void reset_smith_yard_readiness_plugin() {
    g_asset_available = false;
    g_tracker.reset();
    (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 0);
}

void activate_smith_yard_readiness_plugin() {
    // The mod runtime only publishes this path after target and exact asset
    // validation. A null path must leave the hook disabled.
    g_asset_available = gba_mod_required_asset_path(kZelda1ForeignWorldPackageId,
                                                     kZelda1ForeignWorldAssetId) != nullptr;
    if (!g_asset_available) {
        (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 0);
        return;
    }
    (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 1);
}

const SmithYardReadinessState& smith_yard_readiness_state() {
    return g_tracker.state();
}

}  // namespace minish::foreign_world::zelda1

GBA_MOD_CONSTRUCTOR(minish_register_zelda1_foreign_world_plugin) {
    using namespace minish::foreign_world::zelda1;
    (void)gba_mod_register_reset_callback(reset_smith_yard_readiness_plugin);
    (void)gba_mod_register_function_entry_plugin(
        kZelda1ForeignWorldPluginId, 0x0805E5C0u, 1, observe_update_entities);
    (void)gba_mod_register_activation_plugin(
        kZelda1ForeignWorldPluginId, activate_smith_yard_readiness_plugin);
}
