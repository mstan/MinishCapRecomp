#include "foreign_worlds/zelda1/smith_yard_qa_room.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace z1 = minish::foreign_world::zelda1;

namespace {
int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}
}

int main() {
    z1::SmithYardQaRoom room;
    // The old four-direction chord is now exit-only. No inactive chord hold
    // may enter this controller; the source-coordinate portal owns entry.
    for (unsigned i = 0; i != 40; ++i)
        if (room.update(true, 0) != z1::QaRoomEvent::None || room.active())
            return fail("inactive all-Dpad chord remained a foreign-world entry path");
    room.enter();
    if (!room.active()) return fail("host portal did not activate the foreign lease");
    if (room.update(true, z1::kGbaKeysReleased) != z1::QaRoomEvent::None || !room.active())
        return fail("portal entry did not require Dpad release before exit");
    if (room.update(true, 0) != z1::QaRoomEvent::ExitedByTrigger || room.active())
        return fail("active all-Dpad chord did not leave the foreign lease");
    // The leaving chord remains latched until released, so it cannot bounce
    // immediately into any future entry mechanism.
    if (room.update(true, 0) != z1::QaRoomEvent::None || room.active())
        return fail("held exit chord was not latch-safe");

    room.reset();
    // Action/control can become transiently unavailable while the session is
    // active; only the independent danger/survival gate may dismiss it.
    room.enter();
    if (room.update(true, z1::kGbaKeysReleased) != z1::QaRoomEvent::None || !room.active())
        return fail("active room incorrectly used native readiness as survival");
    if (room.update(false, z1::kGbaKeysReleased) !=
            z1::QaRoomEvent::ExitedBecauseNormalControlEnded || room.active())
        return fail("foreign lease did not leave when normal player control ended");

    std::array<std::uint16_t, z1::kQaRoomPixels> pixels{};
    z1::render_smith_yard_qa_room(pixels);
    const auto nonblack = std::count_if(pixels.begin(), pixels.end(),
        [](std::uint16_t pixel) { return pixel != 0; });
    if (nonblack < z1::kQaRoomPixels / 2 || pixels[0] == pixels[120 * 240 + 0])
        return fail("ROM-free QA room did not render stable distinct terrain");
    std::cout << "Smith-yard ROM-free QA room lifecycle and pixels passed\n";
    return 0;
}
