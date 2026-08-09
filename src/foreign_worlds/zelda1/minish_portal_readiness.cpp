#include "foreign_worlds/zelda1/minish_portal_readiness.h"

namespace minish::foreign_world::zelda1 {

MinishPortalReadiness evaluate_minish_portal_readiness(
    const MinishPortalReadinessInput& input) {
    if (!input.verified_zelda1_asset)
        return MinishPortalReadiness::AssetUnavailable;
    if (input.area != 3 || input.room != 1)
        return MinishPortalReadiness::WrongOutdoorRoom;
    if (!input.game_main_update)
        return MinishPortalReadiness::NotInGameUpdate;
    if (input.room_transitioning_out)
        return MinishPortalReadiness::TransitioningOut;
    if (!input.player_entity_alive)
        return MinishPortalReadiness::PlayerEntityUnavailable;
    if (!input.player_entity_drawn)
        return MinishPortalReadiness::PlayerEntityHidden;
    if (!input.normal_player_control)
        return MinishPortalReadiness::PlayerControlLocked;
    if (input.script_or_cutscene_locked)
        return MinishPortalReadiness::ScriptOrCutsceneLocked;
    return MinishPortalReadiness::Ready;
}

std::string_view minish_portal_readiness_diagnostic(
    MinishPortalReadiness readiness) {
    switch (readiness) {
    case MinishPortalReadiness::AssetUnavailable:
        return "validated Zelda 1 asset is unavailable";
    case MinishPortalReadiness::WrongOutdoorRoom:
        return "not in South Hyrule Field (area 3, room 1)";
    case MinishPortalReadiness::NotInGameUpdate:
        return "guest is not in its stable game-update state";
    case MinishPortalReadiness::TransitioningOut:
        return "room transition is in progress";
    case MinishPortalReadiness::PlayerEntityUnavailable:
        return "Link entity is not initialized and alive";
    case MinishPortalReadiness::PlayerEntityHidden:
        return "Link entity is not drawn";
    case MinishPortalReadiness::PlayerControlLocked:
        return "normal player control is unavailable";
    case MinishPortalReadiness::ScriptOrCutsceneLocked:
        return "a script, dialogue, or cutscene owns Link";
    case MinishPortalReadiness::Ready:
        return "South Hyrule Field portal may be interactive";
    }
    return "unknown Minish portal readiness state";
}

void MinishPortalReadinessTracker::reset() {
    state_ = {};
}

void MinishPortalReadinessTracker::observe(const MinishPortalReadinessInput& input) {
    state_.last_input = input;
    state_.readiness = evaluate_minish_portal_readiness(input);
    ++state_.observations;
}

}  // namespace minish::foreign_world::zelda1
