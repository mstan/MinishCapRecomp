#include "foreign_worlds/zelda1/smith_yard_qa_room.h"

#include <algorithm>

namespace minish::foreign_world::zelda1 {
namespace {

constexpr std::uint16_t bgr(unsigned r, unsigned g, unsigned b) {
    return static_cast<std::uint16_t>((r & 31u) | ((g & 31u) << 5u) |
                                      ((b & 31u) << 10u));
}

constexpr std::uint16_t kGrass = bgr(8, 20, 5);
constexpr std::uint16_t kGrassLight = bgr(12, 26, 8);
constexpr std::uint16_t kWater = bgr(18, 11, 2);
constexpr std::uint16_t kWaterLight = bgr(28, 19, 5);
constexpr std::uint16_t kPath = bgr(22, 18, 8);
constexpr std::uint16_t kCliff = bgr(11, 8, 4);
constexpr std::uint16_t kStone = bgr(20, 20, 17);
constexpr std::uint16_t kBlack = bgr(0, 0, 0);
constexpr std::uint16_t kWhite = bgr(31, 31, 31);

void fill_rect(std::array<std::uint16_t, kQaRoomPixels>& pixels, int x, int y,
               int width, int height, std::uint16_t color) {
    const int left = std::clamp(x, 0, static_cast<int>(kQaRoomWidth));
    const int right = std::clamp(x + width, 0, static_cast<int>(kQaRoomWidth));
    const int top = std::clamp(y, 0, static_cast<int>(kQaRoomHeight));
    const int bottom = std::clamp(y + height, 0, static_cast<int>(kQaRoomHeight));
    for (int py = top; py < bottom; ++py)
        for (int px = left; px < right; ++px)
            pixels[static_cast<std::size_t>(py) * kQaRoomWidth + px] = color;
}

// Compact five-by-seven bitmap glyphs for the room's explicit QA label.
const std::uint8_t* glyph(char c) {
    static constexpr std::uint8_t kA[] = {14, 17, 17, 31, 17, 17, 17};
    static constexpr std::uint8_t kD[] = {30, 17, 17, 17, 17, 17, 30};
    static constexpr std::uint8_t kE[] = {31, 16, 16, 30, 16, 16, 31};
    static constexpr std::uint8_t kI[] = {31, 4, 4, 4, 4, 4, 31};
    static constexpr std::uint8_t kL[] = {16, 16, 16, 16, 16, 16, 31};
    static constexpr std::uint8_t kQ[] = {14, 17, 17, 17, 21, 18, 13};
    static constexpr std::uint8_t kR[] = {30, 17, 17, 30, 20, 18, 17};
    static constexpr std::uint8_t kT[] = {31, 4, 4, 4, 4, 4, 4};
    static constexpr std::uint8_t kZ[] = {31, 1, 2, 4, 8, 16, 31};
    static constexpr std::uint8_t k1[] = {4, 12, 4, 4, 4, 4, 14};
    switch (c) {
    case 'A': return kA; case 'D': return kD; case 'E': return kE;
    case 'I': return kI; case 'L': return kL; case 'Q': return kQ;
    case 'R': return kR; case 'T': return kT; case 'Z': return kZ;
    case '1': return k1; default: return nullptr;
    }
}

void draw_label(std::array<std::uint16_t, kQaRoomPixels>& pixels) {
    constexpr char kText[] = "ZELDA 1 QA";
    int cursor = 13;
    for (char c : kText) {
        if (c == ' ') { cursor += 6; continue; }
        const std::uint8_t* rows = glyph(c);
        if (!rows) continue;
        for (int y = 0; y != 7; ++y)
            for (int x = 0; x != 5; ++x)
                if ((rows[y] & (1u << (4 - x))) != 0u)
                    fill_rect(pixels, cursor + x * 2, 8 + y * 2, 2, 2, kWhite);
        cursor += 12;
    }
}

bool trigger_held(std::uint16_t keyinput) {
    return (keyinput & kQaChord) == 0;
}

}  // namespace

QaRoomEvent SmithYardQaRoom::update(bool survival_safe, std::uint16_t keyinput) {
    if (active_ && !survival_safe) {
        active_ = false;
        held_updates_ = 0;
        chord_latched_ = false;
        return QaRoomEvent::ExitedBecauseNormalControlEnded;
    }

    const bool held = trigger_held(keyinput);
    if (!held) {
        held_updates_ = 0;
        chord_latched_ = false;
        return QaRoomEvent::None;
    }
    if (chord_latched_) return QaRoomEvent::None;

    if (active_) {
        active_ = false;
        chord_latched_ = true;
        held_updates_ = 0;
        return QaRoomEvent::ExitedByTrigger;
    }
    // Deliberately no inactive entry path. The concrete native portal owns
    // that interaction and all-Dpad is never a Minish->Zelda input route.
    return QaRoomEvent::None;
}

void SmithYardQaRoom::enter() {
    active_ = true;
    // Contact entry can occur while a direction is held. Require a complete
    // release before the pre-existing all-D-pad chord may act as an exit.
    chord_latched_ = true;
    held_updates_ = 0;
}

void SmithYardQaRoom::reset() {
    active_ = false;
    chord_latched_ = false;
    held_updates_ = 0;
}

void SmithYardQaRoom::restore_active(bool active) {
    active_ = active;
    chord_latched_ = active;
    held_updates_ = 0;
}

void render_smith_yard_qa_room(
    std::array<std::uint16_t, kQaRoomPixels>& pixels) {
    pixels.fill(kGrass);
    // A deliberately simple overworld: water moat, a wood-coloured bridge,
    // cliff/cave landmark, and grass dithering.  It is recognizable in a raw
    // PPU screenshot while containing no protected Zelda 1 art or ROM bytes.
    for (unsigned y = 0; y < kQaRoomHeight; ++y)
        for (unsigned x = 0; x < kQaRoomWidth; ++x)
            if (((x * 3u + y * 5u) & 31u) == 0u)
                pixels[static_cast<std::size_t>(y) * kQaRoomWidth + x] = kGrassLight;
    fill_rect(pixels, 0, 104, 240, 56, kWater);
    for (int y = 110; y < 160; y += 12)
        for (int x = (y / 3) % 19; x < 240; x += 37)
            fill_rect(pixels, x, y, 13, 2, kWaterLight);
    fill_rect(pixels, 97, 90, 46, 70, kPath);
    for (int y = 96; y < 160; y += 9)
        fill_rect(pixels, 97, y, 46, 2, kCliff);
    fill_rect(pixels, 0, 38, 78, 46, kCliff);
    fill_rect(pixels, 5, 42, 68, 34, kStone);
    fill_rect(pixels, 25, 54, 28, 22, kBlack);
    fill_rect(pixels, 28, 57, 22, 19, kCliff);
    fill_rect(pixels, 78, 77, 85, 25, kPath);
    fill_rect(pixels, 81, 80, 79, 19, bgr(26, 13, 3));
    draw_label(pixels);
}

}  // namespace minish::foreign_world::zelda1
