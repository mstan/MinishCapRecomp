#pragma once

#include "foreign_worlds/zelda1/smith_yard_readiness.h"

namespace minish::foreign_world::zelda1 {

inline constexpr char kZelda1ForeignWorldPackageId[] =
    "minish-cap.foreign-world.zelda1";
inline constexpr char kZelda1ForeignWorldAssetId[] =
    "zelda1-first-quest-prg0";
inline constexpr char kZelda1ForeignWorldPluginId[] =
    "minish-cap.zelda1-foreign-world";

// The runtime calls these through constructor registration. They are public so
// the same activation and diagnostics contract can be tested without a ROM.
void reset_smith_yard_readiness_plugin();
void activate_smith_yard_readiness_plugin();
[[nodiscard]] const SmithYardReadinessState& smith_yard_readiness_state();

}  // namespace minish::foreign_world::zelda1
