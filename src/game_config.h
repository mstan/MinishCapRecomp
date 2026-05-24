// game_config.h — Minish Cap-specific wiring exposed to the runtime.
//
// Anything that's a *fact* about Minish Cap (entry point, save type,
// known symbols) lives in game.toml. Anything that's *code* — a
// game-specific hook the runtime needs to call — lives here.
//
// Per PRINCIPLES: do NOT use this file to paper over recompiler /
// runtime bugs. Hooks here exist to provide data the platform core
// can't possibly know about (e.g., the path to a region config), not
// to special-case GBA hardware behavior.

#pragma once

#include <cstdint>
#include <string_view>

namespace minish_cap {

// Returns the default region this build was configured with.
std::string_view default_region();

}  // namespace minish_cap
