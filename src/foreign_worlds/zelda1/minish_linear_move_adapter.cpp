#include "foreign_worlds/zelda1/minish_linear_move_adapter.h"

#include <limits>

namespace minish::foreign_world::zelda1 {
namespace {

// Every eighth Q8.8 sample used by LinearMoveDirectionOLD: direction * 8
// and direction * 8 + 64. Values are copied from the pinned gSineTable, not
// calculated with host floating point at runtime.
constexpr std::int16_t kDirectionSine[32] = {
     0,  49,  97, 142, 181, 212, 236, 251,
   256, 251, 236, 212, 181, 142,  97,  49,
     0, -49, -97,-142,-181,-212,-236,-251,
  -256,-251,-236,-212,-181,-142, -97, -49,
};
constexpr std::int16_t kDirectionCosine[32] = {
   256, 251, 236, 212, 181, 142,  97,  49,
     0, -49, -97,-142,-181,-212,-236,-251,
  -256,-251,-236,-212,-181,-142, -97, -49,
     0,  49,  97, 142, 181, 212, 236, 251,
};

constexpr std::int32_t truncate_q8_to_pixel(std::int32_t value) {
    return value < 0 ? -((-value) / 256) : value / 256;
}

bool add_ok(std::int32_t a, std::int32_t b) {
    return !((b > 0 && a > std::numeric_limits<std::int32_t>::max() - b) ||
             (b < 0 && a < std::numeric_limits<std::int32_t>::min() - b));
}

}  // namespace

std::int16_t LinearMoveAccumulator::fixed_mul(std::int16_t a, std::int16_t b) {
    const std::int32_t product = static_cast<std::int32_t>(a) * b;
    const std::int32_t result = (product + (product < 0 ? 255 : 0)) >> 8;
    return static_cast<std::int16_t>(result);
}

bool LinearMoveAccumulator::step(std::uint8_t direction, std::uint32_t speed,
                                 LinearMoveDelta* delta) {
    if (!delta || (direction & 0x80u) != 0 || direction >= 32) return false;
    const std::int16_t source_speed = static_cast<std::int16_t>(speed);
    const std::int32_t dx_q8 = fixed_mul(kDirectionSine[direction], source_speed);
    const std::int32_t dy_q8 = -fixed_mul(kDirectionCosine[direction], source_speed);
    if (!add_ok(x_q8, dx_q8) || !add_ok(y_q8, dy_q8)) return false;
    const std::int32_t before_x = x_q8;
    const std::int32_t before_y = y_q8;
    x_q8 += dx_q8;
    y_q8 += dy_q8;
    const std::int32_t moved_x = truncate_q8_to_pixel(x_q8) -
                                 truncate_q8_to_pixel(before_x);
    const std::int32_t moved_y = truncate_q8_to_pixel(y_q8) -
                                 truncate_q8_to_pixel(before_y);
    if (moved_x < std::numeric_limits<std::int16_t>::min() ||
        moved_x > std::numeric_limits<std::int16_t>::max() ||
        moved_y < std::numeric_limits<std::int16_t>::min() ||
        moved_y > std::numeric_limits<std::int16_t>::max())
        return false;
    *delta = {static_cast<std::int16_t>(moved_x), static_cast<std::int16_t>(moved_y)};
    return true;
}

bool can_replace_linear_move(std::uint32_t entity_address,
                             bool foreign_session_active,
                             bool survival_safe) {
    return entity_address == kMinishPlayerEntityAddress && foreign_session_active &&
           survival_safe;
}

std::uint32_t linear_move_handled_return_pc(std::uint32_t lr) {
    return lr & ~1u;
}

const GbaForeignObjFocusTransform* ForeignObjFocusDoubleBuffer::next(
    std::int16_t source_feet_x, std::int16_t source_feet_y,
    std::int16_t destination_feet_x, std::int16_t destination_feet_y,
    std::uint16_t source_obj_tile_base, std::uint16_t source_aux_tile_base,
    std::uint16_t source_aux_tile_count, std::uint64_t hud_oam_mask_lo,
    std::uint64_t hud_oam_mask_hi) {
    GbaForeignObjFocusTransform& out = buffers_[next_index_];
    out = {
        GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION,
        source_feet_x,
        source_feet_y,
        destination_feet_x,
        destination_feet_y,
        // `Entity::x/y` are Link's source feet (pinned player/entity headers).
        // The larger geometric association preserves Link's composite OAM;
        // SOURCE_TILE_RANGE filters it to Entity::spriteVramOffset's exact
        // 16-tile allocation so a nearby smith-house door cannot follow him.
        32,
        40,
        GBA_FOREIGN_OBJ_FOCUS_PRESERVE_UNFOCUSED |
            GBA_FOREIGN_OBJ_FOCUS_SOURCE_TILE_RANGE |
            GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_LARGE_NEARBY |
            GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_NEARBY_NONMATCHING |
            GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_NONMATCHING_EXCEPT_HUD_OAM,
        source_obj_tile_base,
        16,
        source_aux_tile_base,
        source_aux_tile_count,
        // Minish Link's source sprite is designed around 16px-scale GBA
        // characters, while Zelda 1's terrain is authored on an 8px grid.
        // A 3/4 presentation-only scale makes the focused body and shadow
        // read proportionally without changing the NES coordinate/collision
        // model or zooming the room.
        192,
        0,
        hud_oam_mask_lo,
        hud_oam_mask_hi,
        // Minish's pinned `DrawHearts` writes gBG0Buffer cells [0x20..]:
        // BG0 tile-map columns 0..11, rows 1..3.  The PPU evaluates this
        // source-cell range after live BG0 scroll/ring mapping and retains
        // only its nontransparent BG0 texels, never a composed room pixel.
        // That preserves the meter without exposing the cave playfield.
        0x01,
        1,
        0,
        0,
        1,
        12,
        3,
        // The same source map cells are HUD only at native x=0..95/y=8..31.
        // Scroll can wrap these cells elsewhere after dialogue; that output
        // bound is an independent required gate, never a composed-pixel mask.
        0,
        8,
        96,
        24,
    };
    next_index_ ^= 1u;
    return &out;
}

}  // namespace minish::foreign_world::zelda1
