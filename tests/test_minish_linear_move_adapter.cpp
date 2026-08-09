#include "foreign_worlds/zelda1/minish_linear_move_adapter.h"

#include "runtime_arm.h"

#include <cstring>
#include <iostream>

namespace z1 = minish::foreign_world::zelda1;

int main() {
    minish::foreign_world::zelda1::LinearMoveAccumulator a;
    if (z1::LinearMoveAccumulator::fixed_mul(-257, 1) != -1 ||
        z1::LinearMoveAccumulator::fixed_mul(257, 1) != 1) {
        std::cerr << "FAIL: FixedMul must truncate toward zero\n";
        return 1;
    }
    z1::LinearMoveDelta delta{};
    if (!a.step(8, 384, &delta) || a.x_q8 != 384 || a.y_q8 != 0 ||
        delta.x != 1 || delta.y != 0) {
        std::cerr << "FAIL: pinned direction-table Q8.8 movement accumulation\n";
        return 1;
    }
    a.x_q8 = 0x7fffffff;
    const auto x = a.x_q8, y = a.y_q8;
    if (a.step(8, 0x100, &delta) || a.x_q8 != x || a.y_q8 != y) {
        std::cerr << "FAIL: overflow must decline atomically\n";
        return 1;
    }
    if (a.step(0x80, 0x100, &delta) || a.step(32, 0x100, &delta))
        return (std::cerr << "FAIL: unsupported source direction did not decline\n", 1);

    // Source: PlayerRollUpdate accepts only PLAYER_ROLL plus PL_ROLLING and
    // feeds its live Entity::direction/speed to UpdatePlayerMovement.  Its
    // largest ordinary phase is 0x300 Q8.8.  The host mirror retains exact
    // fixed-point movement but rejects a corrupt larger sample before it can
    // produce an unbounded foreign-world move.
    z1::MinishRollMotionAccumulator roll;
    z1::MinishRollMotionSample roll_sample{
        z1::kMinishPlayerRollAction, 8, 0x200,
        z1::kMinishPlayerRollingFlag};
    if (roll.step(roll_sample, &delta) !=
            z1::MinishRollMotionAccumulator::Result::kMoved ||
        delta.x != 2 || delta.y != 0)
        return (std::cerr << "FAIL: source 2px roll did not preserve Q8.8 displacement\n", 1);
    roll_sample.speed_q8_8 = 0x300;
    if (roll.step(roll_sample, &delta) !=
            z1::MinishRollMotionAccumulator::Result::kMoved ||
        delta.x != 3 || delta.y != 0)
        return (std::cerr << "FAIL: source 3px roll was not bounded exactly\n", 1);
    // A diagonal direction is owned by the source action, independent of an
    // opposite/held host D-pad. Both axes stay within the three-pixel bound.
    roll_sample.direction = 4;
    if (roll.step(roll_sample, &delta) !=
            z1::MinishRollMotionAccumulator::Result::kMoved ||
        delta.x < -3 || delta.x > 3 || delta.y < -3 || delta.y > 3 ||
        (delta.x == 0 && delta.y == 0))
        return (std::cerr << "FAIL: diagonal roll did not derive bounded source momentum\n", 1);
    roll_sample.speed_q8_8 = 0;
    if (roll.step(roll_sample, &delta) !=
            z1::MinishRollMotionAccumulator::Result::kNoMotion ||
        delta.x != 0 || delta.y != 0)
        return (std::cerr << "FAIL: source roll zero-speed phase moved host terrain\n", 1);
    roll_sample.action = 1; // PLAYER_NORMAL ends the roll and clears remainder.
    if (roll.step(roll_sample, &delta) !=
            z1::MinishRollMotionAccumulator::Result::kNotRolling)
        return (std::cerr << "FAIL: roll termination did not clear host momentum\n", 1);
    roll_sample.action = z1::kMinishPlayerRollAction;
    roll_sample.direction = 8;
    roll_sample.speed_q8_8 = 0x200;
    if (roll.step(roll_sample, &delta) !=
            z1::MinishRollMotionAccumulator::Result::kMoved ||
        delta.x != 2 || delta.y != 0)
        return (std::cerr << "FAIL: ended roll leaked fractional momentum into next roll\n", 1);
    roll_sample.speed_q8_8 = 0x301;
    if (roll.step(roll_sample, &delta) !=
            z1::MinishRollMotionAccumulator::Result::kInvalid ||
        delta.x != 0 || delta.y != 0)
        return (std::cerr << "FAIL: corrupt roll speed was not failure-atomic\n", 1);

    if (!z1::can_replace_linear_move(z1::kMinishPlayerEntityAddress, true, true) ||
        z1::can_replace_linear_move(z1::kMinishPlayerEntityAddress + 0x88, true, true) ||
        z1::can_replace_linear_move(z1::kMinishPlayerEntityAddress, false, true) ||
        z1::can_replace_linear_move(z1::kMinishPlayerEntityAddress, true, false))
        return (std::cerr << "FAIL: Player-only hook gate is not exact\n", 1);

    ArmCpuState cpu{};
    for (unsigned i = 0; i != 16; ++i) cpu.R[i] = 0x11110000u + i;
    cpu.R[14] = 0x08001235u;
    cpu.cpsr = CPSR_T_BIT | 0x13u;
    const ArmCpuState before = cpu;
    cpu.R[15] = z1::linear_move_handled_return_pc(cpu.R[14]);
    if (cpu.R[15] != 0x08001234u || cpu.cpsr != before.cpsr)
        return (std::cerr << "FAIL: handled return PC was not Thumb-normalized\n", 1);
    for (unsigned i = 0; i != 15; ++i)
        if (cpu.R[i] != before.R[i])
            return (std::cerr << "FAIL: handled hook modified a non-PC register\n", 1);

    z1::ForeignObjFocusDoubleBuffer focus;
    const auto* first = focus.next(90, 70, 120, 80, 0x160, 1, 1,
                                   0x5u, 0x8000000000000000ull);
    const auto first_copy = *first;
    const auto* second = focus.next(91, 71, 121, 81, 0x170, 0, 0, 0, 0);
    if (!first || !second || first == second ||
        std::memcmp(first, &first_copy, sizeof(first_copy)) != 0 ||
        first->abi_version != GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION ||
        first->source_radius_x != 32 || first->source_radius_y != 40 ||
        first->flags != (GBA_FOREIGN_OBJ_FOCUS_PRESERVE_UNFOCUSED |
                          GBA_FOREIGN_OBJ_FOCUS_SOURCE_TILE_RANGE |
                          GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_LARGE_NEARBY |
                          GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_NEARBY_NONMATCHING |
                          GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_NONMATCHING_EXCEPT_HUD_OAM) ||
        first->source_obj_tile_base != 0x160 ||
        first->source_obj_tile_count != 16 ||
        first->source_aux_obj_tile_base != 1 ||
        first->source_aux_obj_tile_count != 1 ||
         first->source_obj_scale_q8_8 != 192 || first->reserved != 0 ||
         first->hud_oam_mask_lo != 0x5u ||
         first->hud_oam_mask_hi != 0x8000000000000000ull ||
         first->hud_bg_layer_mask != 0x01 ||
         first->hud_bg_map_rect_count != 1 || first->hud_bg_reserved != 0 ||
         first->hud_bg_map_tile_x != 0 || first->hud_bg_map_tile_y != 1 ||
         first->hud_bg_map_tile_width != 12 ||
         first->hud_bg_map_tile_height != 3 ||
         first->hud_bg_output_x != 0 || first->hud_bg_output_y != 8 ||
         first->hud_bg_output_width != 96 ||
         first->hud_bg_output_height != 24)
        return (std::cerr << "FAIL: focus descriptor double-buffer is not stable\n", 1);
    z1::ActiveLowButtonEdge edge;
    if (edge.observe(0x03ff, 1) || !edge.observe(0x03fe, 1) ||
        edge.observe(0x03fe, 1) || edge.observe(0x03ff, 1) ||
        !edge.observe(0x03fe, 1))
        return (std::cerr << "FAIL: active-low A edge was not one-shot\n", 1);
    return 0;
}
