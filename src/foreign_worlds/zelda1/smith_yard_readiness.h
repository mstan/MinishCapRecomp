#pragma once

#include <cstdint>
#include <string_view>

namespace minish::foreign_world::zelda1 {

// Pinned tmc describes the area/room and transition fields below. No normal
// player-control field has been accepted as source-verified for this slice.
enum class NormalControlGate : std::uint8_t {
    Unknown,
    VerifiedNormal,
};

struct SmithYardProbe {
    bool verified_zelda1_asset = false;
    std::uint8_t area = 0;
    std::uint8_t room = 0;
    bool transitioning_out = false;
    NormalControlGate normal_control = NormalControlGate::Unknown;
};

enum class SmithYardReadiness : std::uint8_t {
    Disabled,
    AssetUnavailable,
    WrongOutdoorRoom,
    TransitioningOut,
    NormalControlUnknown,
    Ready,
};

struct SmithYardReadinessState {
    SmithYardReadiness readiness = SmithYardReadiness::Disabled;
    SmithYardProbe last_probe{};
    std::uint64_t observations = 0;

    [[nodiscard]] bool ready() const {
        return readiness == SmithYardReadiness::Ready;
    }
};

[[nodiscard]] SmithYardReadiness evaluate_smith_yard_probe(
    const SmithYardProbe& probe);
[[nodiscard]] std::string_view smith_yard_diagnostic(
    SmithYardReadiness readiness);

class SmithYardReadinessTracker {
public:
    void reset();
    void observe(const SmithYardProbe& probe);
    [[nodiscard]] const SmithYardReadinessState& state() const { return state_; }

private:
    SmithYardReadinessState state_{};
};

}  // namespace minish::foreign_world::zelda1
