#include "foreign_worlds/zelda1/minish_portal_readiness.h"

#include <iostream>
#include <string>

namespace z1 = minish::foreign_world::zelda1;

namespace {

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

z1::MinishPortalReadinessInput ready_input() {
    return {
        .verified_zelda1_asset = true,
        .area = 3,
        .room = 1,
        .game_main_update = true,
        .room_transitioning_out = false,
        .player_entity_alive = true,
        .player_entity_drawn = true,
        .normal_player_control = true,
        .script_or_cutscene_locked = false,
    };
}

}  // namespace

int main() {
    using R = z1::MinishPortalReadiness;

    auto input = ready_input();
    if (z1::evaluate_minish_portal_readiness(input) != R::Ready)
        return fail("complete South Hyrule Field gate was not ready");

    input.area = 0x22;
    input.room = 0x11;
    if (z1::evaluate_minish_portal_readiness(input) != R::WrongOutdoorRoom)
        return fail("starting house was accepted as a portal host");
    input = ready_input();

    struct RejectionCase {
        const char* name;
        void (*set)(z1::MinishPortalReadinessInput&);
        R expected;
    };
    const RejectionCase cases[] = {
        {"asset", [](z1::MinishPortalReadinessInput& v) { v.verified_zelda1_asset = false; }, R::AssetUnavailable},
        {"update", [](z1::MinishPortalReadinessInput& v) { v.game_main_update = false; }, R::NotInGameUpdate},
        {"transition", [](z1::MinishPortalReadinessInput& v) { v.room_transitioning_out = true; }, R::TransitioningOut},
        {"entity", [](z1::MinishPortalReadinessInput& v) { v.player_entity_alive = false; }, R::PlayerEntityUnavailable},
        {"draw", [](z1::MinishPortalReadinessInput& v) { v.player_entity_drawn = false; }, R::PlayerEntityHidden},
        {"control", [](z1::MinishPortalReadinessInput& v) { v.normal_player_control = false; }, R::PlayerControlLocked},
        {"script", [](z1::MinishPortalReadinessInput& v) { v.script_or_cutscene_locked = true; }, R::ScriptOrCutsceneLocked},
    };
    for (const auto& rejection : cases) {
        auto rejected = ready_input();
        rejection.set(rejected);
        if (z1::evaluate_minish_portal_readiness(rejected) != rejection.expected)
            return fail(std::string("missing ") + rejection.name + " gate");
    }

    z1::MinishPortalReadinessTracker tracker;
    tracker.observe(ready_input());
    auto locked = ready_input();
    locked.script_or_cutscene_locked = true;
    tracker.observe(locked);
    if (tracker.state().observations != 2 || tracker.state().ready() ||
        tracker.state().readiness != R::ScriptOrCutsceneLocked ||
        z1::minish_portal_readiness_diagnostic(R::Ready).empty()) {
        return fail("tracker did not retain the latest non-ready gate");
    }

    tracker.reset();
    if (tracker.state().observations != 0 || tracker.state().ready())
        return fail("tracker reset was not deterministic");

    std::cout << "Minish portal readiness contract passed\n";
    return 0;
}
