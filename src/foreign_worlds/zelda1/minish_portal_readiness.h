#pragma once

#include <cstdint>
#include <string_view>

namespace minish::foreign_world::zelda1 {

// This deliberately contains only values derived from guest reads. Keeping
// guest memory access out of the decision makes the safety gate deterministic
// and lets the plugin own its read timing.
struct MinishPortalReadinessInput {
    bool verified_zelda1_asset = false;
    std::uint8_t area = 0;
    std::uint8_t room = 0;
    bool game_main_update = false;
    bool room_transitioning_out = false;
    bool player_entity_alive = false;
    bool player_entity_drawn = false;
    bool normal_player_control = false;
    bool script_or_cutscene_locked = true;
};

enum class MinishPortalReadiness : std::uint8_t {
    AssetUnavailable,
    WrongOutdoorRoom,
    NotInGameUpdate,
    TransitioningOut,
    PlayerEntityUnavailable,
    PlayerEntityHidden,
    PlayerControlLocked,
    ScriptOrCutsceneLocked,
    Ready,
};

struct MinishPortalReadinessState {
    MinishPortalReadiness readiness = MinishPortalReadiness::AssetUnavailable;
    MinishPortalReadinessInput last_input{};
    std::uint64_t observations = 0;

    [[nodiscard]] bool ready() const {
        return readiness == MinishPortalReadiness::Ready;
    }
};

[[nodiscard]] MinishPortalReadiness evaluate_minish_portal_readiness(
    const MinishPortalReadinessInput& input);
[[nodiscard]] std::string_view minish_portal_readiness_diagnostic(
    MinishPortalReadiness readiness);

class MinishPortalReadinessTracker {
public:
    void reset();
    void observe(const MinishPortalReadinessInput& input);
    [[nodiscard]] const MinishPortalReadinessState& state() const { return state_; }

private:
    MinishPortalReadinessState state_{};
};

}  // namespace minish::foreign_world::zelda1
