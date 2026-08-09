// Temporary, ROM-free QA presentation for the Zelda 1 foreign-world seam.
//
// This is intentionally not a Zelda 1 renderer.  It proves that a validated
// foreign-world package can enter, remain in, and leave a screenshot-visible
// room without writing any Minish Cap gameplay memory. The selected plugin
// publishes immutable BGR555 pixels to the native PPU compositor; Minish
// retains ownership of Link, input dispatch, and collision.

#pragma once

#include <array>
#include <cstdint>

namespace minish::foreign_world::zelda1 {

inline constexpr std::uint32_t kQaRoomWidth = 240;
inline constexpr std::uint32_t kQaRoomHeight = 160;
inline constexpr std::uint32_t kQaRoomPixels = kQaRoomWidth * kQaRoomHeight;
inline constexpr std::uint16_t kGbaKeysReleased = 0x03ff;
inline constexpr std::uint16_t kGbaKeyA = 1u << 0;
inline constexpr std::uint16_t kGbaKeyRight = 1u << 4;
inline constexpr std::uint16_t kGbaKeyLeft = 1u << 5;
inline constexpr std::uint16_t kGbaKeyUp = 1u << 6;
inline constexpr std::uint16_t kGbaKeyDown = 1u << 7;
inline constexpr std::uint16_t kQaChord = kGbaKeyRight | kGbaKeyLeft |
                                      kGbaKeyUp | kGbaKeyDown;

enum class QaRoomEvent : std::uint8_t {
    None,
    Entered,
    ExitedByTrigger,
    ExitedBecauseNormalControlEnded,
};

// The active-session edge-safe state machine. The old all-four-D-pad entry
// chord has been retired: a host-owned South Hyrule Field portal calls enter()
// after independently validating its source-world position and released-then-A
// interaction. The chord remains an explicit active-world exit only.
class SmithYardQaRoom {
public:
    [[nodiscard]] QaRoomEvent update(bool survival_safe,
                                     std::uint16_t keyinput);
    void enter();
    // Snapshot restoration owns only lifecycle visibility, not a partial
    // activation chord. An active restore must observe a release before a
    // held chord can intentionally exit again.
    void restore_active(bool active);
    void reset();
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] unsigned held_updates() const { return held_updates_; }

private:
    bool active_ = false;
    bool chord_latched_ = false;
    unsigned held_updates_ = 0; // retained only for legacy diagnostics (=0)
};

// Deterministically paints a 240x160 BGR555 room with no Zelda ROM bytes.
// The selected live plugin publishes this immutable buffer to the PPU's native
// foreign-background seam; guest OBJ stays PPU-owned for screenshot validation.
// Keeping the renderer pure makes visual content independently testable.
void render_smith_yard_qa_room(
    std::array<std::uint16_t, kQaRoomPixels>& pixels);

}  // namespace minish::foreign_world::zelda1
