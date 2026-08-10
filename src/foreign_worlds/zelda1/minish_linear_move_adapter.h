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
// Source: zeldaret/tmc include/player.h PlayerActions / PlayerStateFlags and
// src/player.c PlayerRollUpdate.  A live foreign roll is only accepted when
// both independent source facts agree.  Entity::speed is Q8.8 at +0x24 and
// Entity::direction is the 0..31 LinearMoveDirectionOLD domain at +0x15.
inline constexpr std::uint8_t kMinishPlayerRollAction = 24;
inline constexpr std::uint32_t kMinishPlayerRollingFlag = 0x00040000u;
inline constexpr std::uint16_t kMinishRollMaximumSpeedQ8_8 = 0x0300u;

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

struct MinishRollMotionSample {
    std::uint8_t action = 0;
    std::uint8_t direction = 0;
    std::uint16_t speed_q8_8 = 0;
    std::uint32_t player_flags = 0;

    [[nodiscard]] bool rolling() const {
        return action == kMinishPlayerRollAction &&
               (player_flags & kMinishPlayerRollingFlag) != 0;
    }
};

// A bounded, host-only mirror of the PlayerRollUpdate movement output.  It
// deliberately accepts no KEYINPUT: while a source roll remains live, its
// Entity::direction/speed own the momentum even after the player releases the
// D-pad.  Invalid source samples fail closed without emitting an unbounded
// host delta; ending a roll clears the Q8.8 remainder before walk resumes.
class MinishRollMotionAccumulator {
public:
    enum class Result : std::uint8_t { kNotRolling, kMoved, kNoMotion, kInvalid };

    [[nodiscard]] Result step(const MinishRollMotionSample& sample,
                              LinearMoveDelta* delta);
    void reset() { accumulator_.reset(); }

private:
    LinearMoveAccumulator accumulator_{};
};

// Exact narrow replacement gate. It deliberately identifies the one static
// gPlayerEntity address, not an inferred Entity kind or a host pointer.
[[nodiscard]] bool can_replace_linear_move(std::uint32_t entity_address,
                                           bool foreign_session_active,
                                           bool survival_safe);

// Completing a handled entry changes only the guest PC to the Thumb return
// address. The caller intentionally leaves R0..R14 and CPSR bit-identical.
[[nodiscard]] std::uint32_t linear_move_handled_return_pc(std::uint32_t lr);

// Immutable v6 PPU descriptor production.  The two buffers use an explicit
// prepare/commit/discard protocol: an unaccepted candidate is reusable, while
// the PPU-visible descriptor remains pinned until a complete background/focus
// pair is accepted.
// The source-mapped player feet are associated with Entity::spriteVramOffset's
// bounded OAM tile allocation, so nearby room props remain untouched.
class ForeignObjFocusDoubleBuffer {
public:
    [[nodiscard]] const GbaForeignObjFocusTransform* prepare(
        std::int16_t source_feet_x, std::int16_t source_feet_y,
        std::int16_t destination_feet_x, std::int16_t destination_feet_y,
        std::uint16_t source_obj_tile_base, std::uint16_t source_aux_tile_base,
        std::uint16_t source_aux_tile_count, std::uint64_t hud_oam_mask_lo,
        std::uint64_t hud_oam_mask_hi);
    void commit();
    void discard();
    // Compatibility wrapper for standalone descriptor tests. The live plugin
    // must pair prepare/commit/discard with background publication.
    [[nodiscard]] const GbaForeignObjFocusTransform* next(
        std::int16_t source_feet_x, std::int16_t source_feet_y,
        std::int16_t destination_feet_x, std::int16_t destination_feet_y,
        std::uint16_t source_obj_tile_base, std::uint16_t source_aux_tile_base,
        std::uint16_t source_aux_tile_count, std::uint64_t hud_oam_mask_lo,
        std::uint64_t hud_oam_mask_hi);
    void reset() {
        published_index_ = 0;
        candidate_index_ = 0;
        published_ = false;
        candidate_prepared_ = false;
        buffers_ = {};
    }

private:
    std::array<GbaForeignObjFocusTransform, 2> buffers_{};
    unsigned published_index_ = 0;
    unsigned candidate_index_ = 0;
    bool published_ = false;
    bool candidate_prepared_ = false;
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
