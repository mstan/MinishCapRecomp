// Host-owned South Hyrule Field portal geometry and pixels.
//
// This component intentionally has no guest-memory or runtime-ABI dependency.
// The live plugin supplies source-mapped values at UpdateEntities and adapts
// the returned immutable frame to the trusted compositor API.
#pragma once

#include <array>
#include <cstdint>

namespace minish::foreign_world::zelda1 {

inline constexpr std::uint16_t kMinishPortalWidth = 32;
inline constexpr std::uint16_t kMinishPortalHeight = 40;
// TMC's room entity/transition data uses these South Hyrule Field local
// coordinates. The live plugin derives their absolute world point by adding
// gRoomControls.origin_x/y before comparing with Entity x/y.
inline constexpr std::int16_t kMinishPortalAnchorLocalX = 0x290;
inline constexpr std::int16_t kMinishPortalAnchorLocalY = 0x19c;
// This is deliberately an interaction half-extent, not a guest collision
// shape. Native Minish collision remains entirely source-owned.
inline constexpr std::int16_t kMinishPortalInteractionHalfExtent = 12;
inline constexpr std::uint16_t kMinishPortalTransparent = 0x7c1f;

struct MinishPortalWorldPosition {
    std::int16_t x{};
    std::int16_t y{};
};

struct MinishPortalScreenPosition {
    std::int16_t x{};
    std::int16_t y{};
};

// The portal is anchored at its bottom centre so its feet point is the same
// source-world coordinate used for interaction.  Signed coordinates permit
// normal clipping as the camera moves it onto/off the native 240x160 view.
[[nodiscard]] MinishPortalWorldPosition minish_portal_world_position(
    std::int16_t room_origin_x, std::int16_t room_origin_y);
[[nodiscard]] MinishPortalScreenPosition project_minish_portal(
    std::int16_t room_origin_x, std::int16_t room_origin_y,
    std::int16_t scroll_x, std::int16_t scroll_y);
[[nodiscard]] bool minish_portal_contains(std::int16_t player_world_x,
                                          std::int16_t player_world_y,
                                          std::int16_t room_origin_x,
                                          std::int16_t room_origin_y);

enum class MinishPortalEvent : std::uint8_t { None, Entered };

struct MinishPortalInput {
    bool entry_ready{};
    std::uint32_t source_frame{};
    std::int16_t player_world_x{};
    std::int16_t player_world_y{};
    std::int16_t room_origin_x{};
    std::int16_t room_origin_y{};
    std::uint16_t keyinput = 0x03ff;
};

// Owns only the native-world A interaction. The caller must feed every
// inactive ROM/IWRAM observation: an A edge captured in a transient first
// phase remains available to a later fully-ready phase of the same source
// frame, while duplicate ready phases cannot enter twice. A release after any
// reset/restore is mandatory before an A press can enter.
class MinishForeignPortalController {
public:
    [[nodiscard]] MinishPortalEvent observe(const MinishPortalInput& input);
    void reset();

private:
    bool frame_seen_ = false;
    std::uint32_t last_frame_{};
    bool a_released_ = false;
    bool a_was_held_ = false;
    bool a_edge_this_frame_ = false;
    bool entry_consumed_this_frame_ = false;
};

// Data-only frame accepted by the engine screen-overlay adapter. `alpha_q4`
// uses 0 for transparent and 1..16 for a native-BG overlay pixel.
struct MinishForeignPortalFrame {
    std::array<std::uint16_t, kMinishPortalWidth * kMinishPortalHeight> pixels{};
    std::array<std::uint8_t, kMinishPortalWidth * kMinishPortalHeight> alpha_q4{};
    MinishPortalScreenPosition screen{};
};

class MinishForeignPortalRenderer {
public:
    [[nodiscard]] const MinishForeignPortalFrame* next(std::int16_t scroll_x,
                                                        std::int16_t scroll_y,
                                                        std::int16_t room_origin_x,
                                                        std::int16_t room_origin_y,
                                                        std::uint32_t source_frame);
    void reset();

private:
    std::array<MinishForeignPortalFrame, 2> buffers_{};
    unsigned next_index_ = 0;
};

}  // namespace minish::foreign_world::zelda1
