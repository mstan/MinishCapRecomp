// Host-owned sword feedback for a selected cross-world sword.
//
// A Zelda-origin sword deliberately does not forge a TMC ItemSword or write
// Minish equipment/story state.  This tiny compositor-side layer makes the
// already-authoritative host A-edge visible while preserving that boundary.

#pragma once

#include "foreign_worlds/zelda1/minish_foreign_combat_adapter.h"
#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace minish::foreign_world::zelda1 {

class HostSwordSwingPresentation {
 public:
  // Six foreign source updates makes the read-only strike visible at normal
  // presentation cadence without turning a button hold into a continuous hit.
  static constexpr unsigned kVisibleFrames = 6;

  void reset() { frames_remaining_ = 0; }
  void begin(ForeignSwordFacing facing) {
    facing_ = facing;
    frames_remaining_ = kVisibleFrames;
  }
  void advance() {
    if (frames_remaining_ != 0) --frames_remaining_;
  }
  [[nodiscard]] bool active() const { return frames_remaining_ != 0; }
  [[nodiscard]] unsigned frames_remaining() const { return frames_remaining_; }
  [[nodiscard]] ForeignSwordFacing facing() const { return facing_; }

  // The returned memory remains valid through the next composition call.  It
  // is not a guest framebuffer and composing it has no guest-memory side
  // effect.  An inactive swing returns the immutable source frame directly.
  [[nodiscard]] const std::uint16_t* compose(const std::uint16_t* source,
                                             std::int16_t link_feet_x,
                                             std::int16_t link_feet_y) {
    if (!source || !active()) return source;
    auto& output = buffers_[next_buffer_];
    std::copy_n(source, output.size(), output.begin());
    next_buffer_ ^= 1u;
    draw_sword(&output, link_feet_x, link_feet_y, facing_);
    return output.data();
  }

 private:
  static constexpr std::uint16_t kBlade = 0x7fff;
  static constexpr std::uint16_t kBladeShade = 0x56b5;
  static constexpr std::uint16_t kHilt = 0x001f;

  static void put(OwFramebuffer* frame, int x, int y, std::uint16_t color) {
    if (!frame || x < 0 || y < 0 || x >= static_cast<int>(kOwRenderWidth) ||
        y >= static_cast<int>(kOwRenderHeight))
      return;
    (*frame)[static_cast<std::size_t>(y) * kOwRenderWidth +
             static_cast<std::size_t>(x)] = color;
  }

  static void draw_sword(OwFramebuffer* frame, std::int16_t feet_x,
                         std::int16_t feet_y, ForeignSwordFacing facing) {
    // The focused Minish body supplies its own sprite.  Draw a compact,
    // original BGR555 blade outward from its feet/center in the Zelda frame;
    // it is intentionally not a copied NES/TMC sprite asset.
    int dx = 0, dy = 0, px = feet_x, py = feet_y - 8;
    switch (facing) {
    case ForeignSwordFacing::kNorth: dy = -1; py = feet_y - 16; break;
    case ForeignSwordFacing::kEast: dx = 1; px = feet_x + 7; py = feet_y - 8; break;
    case ForeignSwordFacing::kSouth: dy = 1; py = feet_y - 1; break;
    case ForeignSwordFacing::kWest: dx = -1; px = feet_x - 7; py = feet_y - 8; break;
    }
    // Blue hilt, two-pixel silver grip, then a five-pixel blade with a small
    // highlight.  The perpendicular offset makes the slash legible on the
    // low-resolution terrain without changing collision or gameplay state.
    const int perp_x = dy, perp_y = -dx;
    put(frame, px - dx, py - dy, kHilt);
    for (int step = 0; step != 5; ++step) {
      const int x = px + dx * step;
      const int y = py + dy * step;
      put(frame, x, y, kBlade);
      put(frame, x + perp_x, y + perp_y, kBladeShade);
    }
    put(frame, px + dx * 5, py + dy * 5, kBladeShade);
  }

  std::array<OwFramebuffer, 2> buffers_{};
  unsigned next_buffer_ = 0;
  unsigned frames_remaining_ = 0;
  ForeignSwordFacing facing_ = ForeignSwordFacing::kNorth;
};

}  // namespace minish::foreign_world::zelda1
