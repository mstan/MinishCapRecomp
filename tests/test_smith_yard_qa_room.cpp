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
    for (unsigned i = 0; i + 1 < z1::SmithYardQaRoom::kActivationUpdates; ++i)
        if (room.update(true, true, 0) != z1::QaRoomEvent::None || room.active())
            return fail("QA room entered before the documented chord hold");
    if (room.update(true, true, 0) != z1::QaRoomEvent::Entered || !room.active())
        return fail("QA room did not enter after all-D-pad hold");
    // The entering chord is latched; it cannot also immediately exit.
    if (room.update(true, true, 0) != z1::QaRoomEvent::None || !room.active())
        return fail("QA trigger was not edge safe");
    (void)room.update(true, true, z1::kGbaKeysReleased);
    if (room.update(true, true, 0) != z1::QaRoomEvent::ExitedByTrigger || room.active())
        return fail("QA room did not leave after a released-and-held chord");

    room.reset();
    for (unsigned i = 0; i < z1::SmithYardQaRoom::kActivationUpdates; ++i)
        (void)room.update(true, true, 0);
    // Action/control can become transiently unavailable while the session is
    // active; only the independent danger/survival gate may dismiss it.
    if (room.update(false, true, z1::kGbaKeysReleased) != z1::QaRoomEvent::None || !room.active())
        return fail("active QA room incorrectly used entry readiness as survival");
    if (room.update(false, false, z1::kGbaKeysReleased) !=
            z1::QaRoomEvent::ExitedBecauseNormalControlEnded || room.active())
        return fail("QA room did not leave when normal player control ended");

    std::array<std::uint16_t, z1::kQaRoomPixels> pixels{};
    z1::render_smith_yard_qa_room(pixels);
    const auto nonblack = std::count_if(pixels.begin(), pixels.end(),
        [](std::uint16_t pixel) { return pixel != 0; });
    if (nonblack < z1::kQaRoomPixels / 2 || pixels[0] == pixels[120 * 240 + 0])
        return fail("ROM-free QA room did not render stable distinct terrain");
    std::cout << "Smith-yard ROM-free QA room lifecycle and pixels passed\n";
    return 0;
}
