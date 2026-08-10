#include "foreign_worlds/zelda1/minish_foreign_portal.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>

namespace z1 = minish::foreign_world::zelda1;

namespace {

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

z1::MinishPortalInput input(std::uint32_t frame, bool ready = true,
                            std::int16_t x = z1::kMinishPortalAnchorLocalX,
                            std::int16_t y = z1::kMinishPortalAnchorLocalY,
                            std::int16_t origin_x = 0,
                            std::int16_t origin_y = 0) {
    return {ready, true, frame, x, y, origin_x, origin_y};
}

}  // namespace

int main() {
    const auto world = z1::minish_portal_world_position(0x100, 0x80);
    if (world.x != 0x350 || world.y != 0x238)
        return fail("local source portal anchor did not convert to world coordinates");
    const auto screen = z1::project_minish_portal(0, 0, 0x200, 0x100);
    if (screen.x != 64 || screen.y != 0x90)
        return fail("source-world portal did not project through RoomControls scroll");
    if (!z1::minish_portal_contains(0x250, 0x1b8, 0, 0) ||
        // F0 playtest geometry: Link's feet can visibly touch the rift from
        // 23px below its bottom anchor and must trigger contact entry.
        !z1::minish_portal_contains(0x250, 0x1cf, 0, 0) ||
        !z1::minish_portal_contains(0x268, 0x18c, 0, 0) ||
        z1::minish_portal_contains(0x269, 0x1b8, 0, 0) ||
        z1::minish_portal_contains(0x250, 0x1d1, 0, 0) ||
        z1::minish_portal_contains(0x250, 0x18b, 0, 0) ||
        !z1::minish_portal_contains(0x350, 0x238, 0x100, 0x80))
        return fail("visible-rift approach bounds are not exact");
    // Source proof: transitions.c defines Link's House as the exact rectangle
    // x=[0x282,0x29e], y=[0x182,0x18e]. The complete interaction rectangle is
    // west/south of it with 25 clear horizontal pixels; no transition captures A.
    constexpr int kHouseWarpLeft = 0x282, kHouseWarpRight = 0x29e;
    constexpr int kHouseWarpTop = 0x182, kHouseWarpBottom = 0x18e;
    constexpr int kPortalLeft = z1::kMinishPortalAnchorLocalX -
        z1::kMinishPortalInteractionWest;
    constexpr int kPortalRight = z1::kMinishPortalAnchorLocalX +
        z1::kMinishPortalInteractionEast;
    constexpr int kPortalTop = z1::kMinishPortalAnchorLocalY -
        z1::kMinishPortalInteractionNorth;
    constexpr int kPortalBottom = z1::kMinishPortalAnchorLocalY +
        z1::kMinishPortalInteractionSouth;
    if (!(kPortalRight < kHouseWarpLeft || kPortalLeft > kHouseWarpRight ||
          kPortalBottom < kHouseWarpTop || kPortalTop > kHouseWarpBottom))
        return fail("open-yard portal interaction overlaps Link's House transition");

    z1::MinishForeignPortalController controller;
    // Starting inside is inert: an outside sample is needed after a
    // load/menu/transition before contact can enter.
    if (controller.observe(input(1, true, 0x250, 0x1cf)) != z1::MinishPortalEvent::None ||
        controller.observe(input(2, true, 0x250, 0x1cf)) != z1::MinishPortalEvent::None)
        return fail("portal appearing beneath Link auto-entered without a crossing");
    if (controller.observe(input(3, true, 0x269, 0x1cf)) != z1::MinishPortalEvent::None ||
        controller.observe(input(4, true, 0x250, 0x1cf)) != z1::MinishPortalEvent::Entered)
        return fail("safe outside-to-inside portal contact did not enter");
    if (controller.observe(input(4, true, 0x250, 0x1cf)) != z1::MinishPortalEvent::None)
        return fail("duplicate ROM/IWRAM contact callbacks entered twice");
    controller.reset();
    if (controller.observe(input(10, true, 0x269, 0x1cf)) != z1::MinishPortalEvent::None ||
        controller.observe(input(11, false, 0x250, 0x1cf)) != z1::MinishPortalEvent::None ||
        controller.observe(input(12, true, 0x269, 0x1cf)) != z1::MinishPortalEvent::None ||
        controller.observe(input(13, true, 0x250, 0x1cf)) != z1::MinishPortalEvent::Entered)
        return fail("unsafe portal frame did not require a later safe contact crossing");
    controller.reset();
    if (controller.observe(input(20, true, 0x269, 0x1cf)) != z1::MinishPortalEvent::None)
        return fail("outside contact fixture did not arm");
    auto late_phase = input(21, false, 0x250, 0x1cf);
    late_phase.hard_safe = true;
    if (controller.observe(late_phase) != z1::MinishPortalEvent::None ||
        controller.observe(input(22, true, 0x250, 0x1cf)) != z1::MinishPortalEvent::Entered)
        return fail("late normal-control phase erased a valid contact crossing arm");
    controller.reset();
    if (controller.observe(input(30, true, 0x269, 0x1cf)) != z1::MinishPortalEvent::None)
        return fail("hard-gate fixture did not arm");
    auto lifecycle_boundary = input(31, false, 0x250, 0x1cf);
    lifecycle_boundary.hard_safe = false;
    if (controller.observe(lifecycle_boundary) != z1::MinishPortalEvent::None ||
        controller.observe(input(32, true, 0x250, 0x1cf)) != z1::MinishPortalEvent::None)
        return fail("hard lifecycle gate failed to revoke a portal contact arm");

    z1::MinishForeignPortalRenderer renderer;
    const auto* first = renderer.next(0x200, 0x100, 0, 0, 0);
    if (!first || first->screen.x != 64 || first->screen.y != 0x90 ||
        std::none_of(first->alpha_q4.begin(), first->alpha_q4.end(),
                     [](std::uint8_t alpha) { return alpha != 0; }) ||
        std::any_of(first->alpha_q4.begin(), first->alpha_q4.end(),
                    [](std::uint8_t alpha) { return alpha > 16; }))
        return fail("portal renderer did not produce a bounded transparent overlay frame");
    const auto first_copy = *first;
    const auto* second = renderer.next(0x201, 0x102, 0, 0, 1);
    if (!second || second == first || first->pixels != first_copy.pixels ||
        first->alpha_q4 != first_copy.alpha_q4 || second->screen.x != 63 ||
        second->screen.y != 0x8e || second->pixels == first_copy.pixels)
        return fail("portal animation/double buffer did not retain the prior immutable frame");

    std::cout << "Minish foreign portal controller and renderer passed\n";
    return 0;
}
