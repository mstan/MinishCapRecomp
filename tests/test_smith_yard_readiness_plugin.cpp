#include "foreign_worlds/zelda1/smith_yard_readiness_plugin.h"

#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace z1 = minish::foreign_world::zelda1;

namespace {

const char* g_asset_path = nullptr;
std::uint8_t g_area = 0;
std::uint8_t g_room = 0;
std::uint8_t g_transitioning_out = 0;

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

}  // namespace

extern "C" int gba_mod_register_activation_plugin(const char*,
                                                    GBAModActivationCallback) {
    return 1;
}
extern "C" int gba_mod_register_reset_callback(GBAModActivationCallback) {
    return 1;
}
extern "C" const char* gba_mod_required_asset_path(const char* package_id,
                                                     const char* asset_id) {
    if (std::string(package_id) != z1::kZelda1ForeignWorldPackageId ||
        std::string(asset_id) != z1::kZelda1ForeignWorldAssetId)
        return nullptr;
    return g_asset_path;
}
extern "C" std::uint8_t bus_read_u8(std::uint32_t address) {
    switch (address) {
    case 0x03000BF4: return g_area;
    case 0x03000BF5: return g_room;
    case 0x030010A8: return g_transitioning_out;
    default: return 0;
    }
}

int main() {
    z1::reset_smith_yard_readiness_plugin();
    z1::activate_smith_yard_readiness_plugin();
    if (gba_mod_function_hook_enabled(z1::kZelda1ForeignWorldPluginId))
        return fail("missing required asset enabled the observation hook");

    g_asset_path = "C:/user/zelda1.nes";
    z1::activate_smith_yard_readiness_plugin();
    if (!gba_mod_function_hook_enabled(z1::kZelda1ForeignWorldPluginId))
        return fail("validated required asset did not enable the observation hook");

    g_area = 3;
    g_room = 1;
    g_transitioning_out = 0;
    ArmCpuState cpu{};
    cpu.R[0] = 0x12345678u;
    cpu.R[15] = 0x0805E5C0u;
    if (gba_mod_function_entry(0x0805E5C0u, 1, &cpu) != 0 ||
        cpu.R[0] != 0x12345678u || cpu.R[15] != 0x0805E5C0u) {
        return fail("readiness hook did not decline without changing guest CPU state");
    }
    const auto& state = z1::smith_yard_readiness_state();
    if (state.observations != 1 || state.last_probe.area != 3 ||
        state.last_probe.room != 1 || state.last_probe.transitioning_out ||
        state.readiness != z1::SmithYardReadiness::NormalControlUnknown) {
        return fail("readiness hook did not use the source-verified guest fields");
    }

    z1::reset_smith_yard_readiness_plugin();
    if (gba_mod_function_hook_enabled(z1::kZelda1ForeignWorldPluginId) ||
        z1::smith_yard_readiness_state().readiness != z1::SmithYardReadiness::Disabled)
        return fail("reset did not disable hook and diagnostics");
    std::cout << "Smith-yard plugin asset gating and observation-only hook passed\n";
    return 0;
}
