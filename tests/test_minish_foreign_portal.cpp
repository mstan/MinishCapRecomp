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

z1::MinishPortalInput input(std::uint32_t frame, std::uint16_t keyinput,
                            bool ready = true,
                            std::int16_t x = z1::kMinishPortalAnchorLocalX,
                            std::int16_t y = z1::kMinishPortalAnchorLocalY,
                            std::int16_t origin_x = 0,
                            std::int16_t origin_y = 0) {
    return {ready, frame, x, y, origin_x, origin_y, keyinput};
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
        // 23px below its bottom anchor and must still be able to press A.
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
    // A held A at activation is intentionally inert until a release happens.
    if (controller.observe(input(1, 0x03fe)) != z1::MinishPortalEvent::None ||
        controller.observe(input(2, 0x03fe)) != z1::MinishPortalEvent::None)
        return fail("held A entered before the required release");
    if (controller.observe(input(3, 0x03ff)) != z1::MinishPortalEvent::None ||
        controller.observe(input(4, 0x03fe)) != z1::MinishPortalEvent::Entered)
        return fail("released-then-A did not enter the exact portal interaction zone");
    if (controller.observe(input(4, 0x03fe)) != z1::MinishPortalEvent::None)
        return fail("duplicate ready phase entered portal twice");

    controller.reset();
    if (controller.observe(input(5, 0x03ff, true, 0x250, 0x1cf)) !=
            z1::MinishPortalEvent::None ||
        controller.observe(input(6, 0x03fe, true, 0x250, 0x1cf)) !=
            z1::MinishPortalEvent::Entered)
        return fail("F0 visible-touch approach offset could not enter the portal");

    controller.reset();
    (void)controller.observe(input(10, 0x03ff));
    if (controller.observe(input(11, 0x03fe, true, 0x269, 0x1b8)) !=
        z1::MinishPortalEvent::None)
        return fail("A outside the native portal entered Zelda");
    // A press sampled by an unready ROM phase is retained for a later ready
    // IWRAM phase of the same source frame, but not across source frames.
    if (controller.observe(input(12, 0x03ff)) != z1::MinishPortalEvent::None ||
        controller.observe(input(13, 0x03fe, false)) != z1::MinishPortalEvent::None ||
        controller.observe(input(13, 0x03fe, true)) != z1::MinishPortalEvent::Entered)
        return fail("ready second hook lost the same-frame released-then-A portal edge");

    controller.reset();
    (void)controller.observe(input(20, 0x03ff));
    for (std::uint32_t frame = 21; frame != 61; ++frame)
        if (controller.observe(input(frame, 0x030f)) != z1::MinishPortalEvent::None)
            return fail("all-Dpad chord remained an inactive portal entry path");

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
