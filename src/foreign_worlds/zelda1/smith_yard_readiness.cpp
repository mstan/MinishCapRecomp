#include "foreign_worlds/zelda1/smith_yard_readiness.h"

namespace minish::foreign_world::zelda1 {

SmithYardReadiness evaluate_smith_yard_probe(const SmithYardProbe& probe) {
    if (!probe.verified_zelda1_asset)
        return SmithYardReadiness::AssetUnavailable;
    if (probe.area != 3 || probe.room != 1)
        return SmithYardReadiness::WrongOutdoorRoom;
    if (probe.transitioning_out)
        return SmithYardReadiness::TransitioningOut;
    if (probe.normal_control != NormalControlGate::VerifiedNormal)
        return SmithYardReadiness::NormalControlUnknown;
    return SmithYardReadiness::Ready;
}

std::string_view smith_yard_diagnostic(SmithYardReadiness readiness) {
    switch (readiness) {
    case SmithYardReadiness::Disabled:
        return "Zelda 1 foreign world is disabled";
    case SmithYardReadiness::AssetUnavailable:
        return "validated Zelda 1 PRG0 asset is unavailable";
    case SmithYardReadiness::WrongOutdoorRoom:
        return "not in Smith-yard outdoor area 3, room 1";
    case SmithYardReadiness::TransitioningOut:
        return "room transition is in progress";
    case SmithYardReadiness::NormalControlUnknown:
        return "normal-control gate is not source-verified";
    case SmithYardReadiness::Ready:
        return "Smith-yard portal renderer may inspect readiness";
    }
    return "unknown Smith-yard readiness state";
}

void SmithYardReadinessTracker::reset() {
    state_ = {};
}

void SmithYardReadinessTracker::observe(const SmithYardProbe& probe) {
    state_.last_probe = probe;
    state_.readiness = evaluate_smith_yard_probe(probe);
    ++state_.observations;
}

}  // namespace minish::foreign_world::zelda1
