#include "foreign_worlds/zelda1/minish_foreign_portal.h"

namespace minish::foreign_world::zelda1 {
namespace {

constexpr std::uint16_t bgr(unsigned r, unsigned g, unsigned b) {
    return static_cast<std::uint16_t>((r & 31u) | ((g & 31u) << 5u) |
                                      ((b & 31u) << 10u));
}

constexpr std::uint16_t kPortalBlue = bgr(7, 19, 31);
constexpr std::uint16_t kPortalCyan = bgr(16, 29, 31);
constexpr std::uint16_t kPortalViolet = bgr(26, 9, 28);
constexpr std::uint16_t kPortalWhite = bgr(31, 31, 31);

}  // namespace

MinishPortalWorldPosition minish_portal_world_position(std::int16_t room_origin_x,
                                                        std::int16_t room_origin_y) {
    return {static_cast<std::int16_t>(room_origin_x + kMinishPortalAnchorLocalX),
            static_cast<std::int16_t>(room_origin_y + kMinishPortalAnchorLocalY)};
}

MinishPortalScreenPosition project_minish_portal(std::int16_t room_origin_x,
                                                 std::int16_t room_origin_y,
                                                 std::int16_t scroll_x,
                                                 std::int16_t scroll_y) {
    // `x/y` are the top-left screen coordinates for the immutable overlay.
    const auto world = minish_portal_world_position(room_origin_x, room_origin_y);
    return {
        static_cast<std::int16_t>(world.x - scroll_x - kMinishPortalWidth / 2),
        static_cast<std::int16_t>(world.y - scroll_y - kMinishPortalHeight),
    };
}

bool minish_portal_contains(std::int16_t player_world_x,
                            std::int16_t player_world_y,
                            std::int16_t room_origin_x,
                            std::int16_t room_origin_y) {
    const auto world = minish_portal_world_position(room_origin_x, room_origin_y);
    const int dx = static_cast<int>(player_world_x) - world.x;
    const int dy = static_cast<int>(player_world_y) - world.y;
    return dx >= -kMinishPortalInteractionWest &&
           dx <= kMinishPortalInteractionEast &&
           dy >= -kMinishPortalInteractionNorth &&
           dy <= kMinishPortalInteractionSouth;
}

MinishPortalEvent MinishForeignPortalController::observe(
    const MinishPortalInput& input) {
    if (!frame_seen_ || last_frame_ != input.source_frame) {
        frame_seen_ = true;
        last_frame_ = input.source_frame;
        entry_consumed_this_frame_ = false;
    }

    if (!input.hard_safe) {
        contact_armed_ = false;
        return MinishPortalEvent::None;
    }
    if (!input.entry_ready) return MinishPortalEvent::None;
    const bool inside = minish_portal_contains(
        input.player_world_x, input.player_world_y,
        input.room_origin_x, input.room_origin_y);
    if (!inside) {
        contact_armed_ = true;
        return MinishPortalEvent::None;
    }
    if (entry_consumed_this_frame_ || !contact_armed_)
        return MinishPortalEvent::None;

    entry_consumed_this_frame_ = true;
    contact_armed_ = false;
    return MinishPortalEvent::Entered;
}

void MinishForeignPortalController::reset() {
    frame_seen_ = false;
    last_frame_ = 0;
    entry_consumed_this_frame_ = false;
    contact_armed_ = false;
}

const MinishForeignPortalFrame* MinishForeignPortalRenderer::next(
    std::int16_t scroll_x, std::int16_t scroll_y,
    std::int16_t room_origin_x, std::int16_t room_origin_y,
    std::uint32_t source_frame) {
    MinishForeignPortalFrame& out = buffers_[next_index_];
    out.pixels.fill(kMinishPortalTransparent);
    out.alpha_q4.fill(0);
    out.screen = project_minish_portal(room_origin_x, room_origin_y,
                                       scroll_x, scroll_y);

    // An original, small magical rift: a two-pixel-stable elliptical rim and
    // a deterministic counter-rotating core. It intentionally contains no
    // Minish or Zelda source pixels.
    const int phase = static_cast<int>(source_frame % 12u);
    for (int y = 0; y < static_cast<int>(kMinishPortalHeight); ++y) {
        for (int x = 0; x < static_cast<int>(kMinishPortalWidth); ++x) {
            const int dx = x - 15;
            const int dy = y - 21;
            const int ellipse = dx * dx * 4 + dy * dy;
            const std::size_t index = static_cast<std::size_t>(y) *
                                      kMinishPortalWidth + x;
            if (ellipse >= 245 && ellipse <= 345) {
                out.pixels[index] = ((x + y + phase) & 3) == 0 ?
                    kPortalCyan : kPortalBlue;
                out.alpha_q4[index] = 16;
            } else if (ellipse < 245 && ellipse > 36 &&
                       ((x * 3 - y * 2 + phase * 5) & 15) < 4) {
                out.pixels[index] = ((x - y + phase) & 7) == 0 ?
                    kPortalWhite : kPortalViolet;
                out.alpha_q4[index] = 10;
            } else if (ellipse <= 36 && ((phase + x + y) & 3) == 0) {
                out.pixels[index] = kPortalCyan;
                out.alpha_q4[index] = 13;
            }
        }
    }
    next_index_ ^= 1u;
    return &out;
}

void MinishForeignPortalRenderer::reset() {
    buffers_ = {};
    next_index_ = 0;
}

}  // namespace minish::foreign_world::zelda1
