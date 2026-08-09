#include "foreign_worlds/zelda1/smith_yard_readiness.h"

#include <iostream>
#include <string>

namespace z1 = minish::foreign_world::zelda1;

namespace {

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

}  // namespace

int main() {
    z1::SmithYardProbe probe{};
    if (z1::evaluate_smith_yard_probe(probe) != z1::SmithYardReadiness::AssetUnavailable)
        return fail("missing validated asset was not the first gate");

    probe.verified_zelda1_asset = true;
    if (z1::evaluate_smith_yard_probe(probe) != z1::SmithYardReadiness::WrongOutdoorRoom)
        return fail("wrong room was accepted");
    probe.area = 3;
    probe.room = 1;
    if (z1::evaluate_smith_yard_probe(probe) !=
        z1::SmithYardReadiness::NormalControlUnknown)
        return fail("unknown normal-control gate was not explicit");
    probe.transitioning_out = true;
    if (z1::evaluate_smith_yard_probe(probe) != z1::SmithYardReadiness::TransitioningOut)
        return fail("transitioning-out gate was not enforced");
    probe.transitioning_out = false;
    probe.normal_control = z1::NormalControlGate::VerifiedNormal;
    if (z1::evaluate_smith_yard_probe(probe) != z1::SmithYardReadiness::Ready ||
        z1::smith_yard_diagnostic(z1::SmithYardReadiness::Ready).empty())
        return fail("fully verified probe was not ready");

    z1::SmithYardReadinessTracker tracker;
    tracker.observe(probe);
    tracker.observe({});
    if (tracker.state().observations != 2 || tracker.state().ready() ||
        tracker.state().readiness != z1::SmithYardReadiness::AssetUnavailable) {
        return fail("readiness tracker did not retain the latest diagnostic state");
    }
    tracker.reset();
    if (tracker.state().observations != 0 ||
        tracker.state().readiness != z1::SmithYardReadiness::Disabled)
        return fail("readiness tracker reset was not deterministic");

    std::cout << "Smith-yard readiness gates and diagnostics passed\n";
    return 0;
}
