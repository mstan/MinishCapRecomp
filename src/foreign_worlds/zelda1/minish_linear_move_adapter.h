#pragma once

#include "foreign_obj_focus.h"

#include <array>
#include <cstdint>

namespace minish::foreign_world::zelda1 {

// Source: zeldaret/tmc @ 5ab63f0, asm/src/code_08001A7C.s,
// LinearMoveDirectionOLD (0x080027EA); src/physics.c FixedMul; and
// src/sineTable.c.  This is a host-only Q8.8 mirror.  It owns no guest
// pointer and never writes guest state; Zelda1OverworldSession owns terrain
// collision after the delta has been derived.
inline constexpr std::uint32_t kMinishPlayerEntityAddress = 0x03001160u;

struct LinearMoveDelta {
    std::int16_t x = 0;
    std::int16_t y = 0;
};

struct LinearMoveAccumulator {
    std::int32_t x_q8 = 0;
    std::int32_t y_q8 = 0;
    // Matches TMC FixedMul's signed-16-bit inputs and truncation toward zero.
    static std::int16_t fixed_mul(std::int16_t a, std::int16_t b);
    // Derives whole-pixel movement from the source direction table while
    // retaining the fractional Q8.8 remainder. Direction 0..31 is the exact
    // LinearMoveDirectionOLD table domain. A bit-7 direction is a source
    // no-op; unsupported directions decline rather than guessing.
    [[nodiscard]] bool step(std::uint8_t direction, std::uint32_t speed,
                            LinearMoveDelta* delta);
    void reset() { x_q8 = 0; y_q8 = 0; }
};

// Exact narrow replacement gate. It deliberately identifies the one static
// gPlayerEntity address, not an inferred Entity kind or a host pointer.
[[nodiscard]] bool can_replace_linear_move(std::uint32_t entity_address,
                                           bool foreign_session_active,
                                           bool survival_safe);

// Completing a handled entry changes only the guest PC to the Thumb return
// address. The caller intentionally leaves R0..R14 and CPSR bit-identical.
[[nodiscard]] std::uint32_t linear_move_handled_return_pc(std::uint32_t lr);

// Immutable v1 PPU descriptor production. The two buffers are intentionally
// separate: fully write the inactive descriptor, then publish its address.
// This makes a prior PPU-visible descriptor stable through the next update.
class ForeignObjFocusDoubleBuffer {
public:
    [[nodiscard]] const GbaForeignObjFocusTransform* next(
        std::int16_t source_feet_x, std::int16_t source_feet_y,
        std::int16_t destination_feet_x, std::int16_t destination_feet_y);
    void reset() { next_index_ = 0; buffers_ = {}; }

private:
    std::array<GbaForeignObjFocusTransform, 2> buffers_{};
    unsigned next_index_ = 0;
};

// KEYINPUT is active-low. Keeping the one-shot edge detector pure means an
// A hold cannot repeatedly acknowledge a cave textbox or acquire an item.
class ActiveLowButtonEdge {
public:
    [[nodiscard]] bool observe(std::uint16_t keyinput, std::uint16_t mask) {
        const bool held = (keyinput & mask) == 0;
        const bool edge = held && !was_held_;
        was_held_ = held;
        return edge;
    }
    void reset() { was_held_ = false; }
private:
    bool was_held_ = false;
};

}  // namespace minish::foreign_world::zelda1
