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
  // Nine source updates give the host-only strike a readable Minish-like
  // wind-up, travelling arc, and recovery without turning a held button into
  // a continuous attack.  This is deliberately presentation timing only:
  // combat continues to consume the single source A-edge.
  static constexpr unsigned kVisibleFrames = 9;

  enum class Phase : std::uint8_t { kWindup, kStrike, kRecovery };

  void reset() {
    frames_remaining_ = 0;
    candidate_prepared_ = false;
    published_host_buffer_ = false;
  }
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
  [[nodiscard]] Phase phase() const {
    const unsigned elapsed = kVisibleFrames - frames_remaining_;
    return elapsed < 2 ? Phase::kWindup
           : elapsed < 6 ? Phase::kStrike
                         : Phase::kRecovery;
  }

  // Prepare a candidate buffer without changing the buffer currently owned
  // by the PPU.  The plugin must call commit() only after the matching focus
  // descriptor has also published, or discard() after either publication
  // rejects.  Retrying then overwrites the rejected candidate, never the
  // restored complete foreign frame.
  [[nodiscard]] const std::uint16_t* prepare(const std::uint16_t* source,
                                              std::int16_t link_feet_x,
                                              std::int16_t link_feet_y) {
    candidate_prepared_ = true;
    candidate_host_buffer_ = source != nullptr && active();
    if (!candidate_host_buffer_) return source;
    candidate_buffer_ = published_host_buffer_ ? (published_buffer_ ^ 1u) : 0u;
    auto& output = buffers_[candidate_buffer_];
    std::copy_n(source, output.size(), output.begin());
    draw_sword(&output, link_feet_x, link_feet_y, facing_,
               kVisibleFrames - frames_remaining_);
    return output.data();
  }

  // Pair these with the surrounding PPU publication transaction.  They are
  // intentionally no-ops for an inactive swing, whose source framebuffer is
  // already owned by its session rather than this presentation object.
  void commit() {
    if (!candidate_prepared_) return;
    if (candidate_host_buffer_) {
      published_buffer_ = candidate_buffer_;
      published_host_buffer_ = true;
    } else {
      published_host_buffer_ = false;
    }
    candidate_prepared_ = false;
  }
  void discard() { candidate_prepared_ = false; }

  // Compatibility wrapper for isolated presentation tests.  The live plugin
  // must use prepare/commit/discard with its matching OBJ-focus transaction.
  [[nodiscard]] const std::uint16_t* compose(const std::uint16_t* source,
                                              std::int16_t link_feet_x,
                                              std::int16_t link_feet_y) {
    const auto* output = prepare(source, link_feet_x, link_feet_y);
    commit();
    return output;
  }

 private:
  static constexpr std::uint16_t kBlade = 0x7fff;
  static constexpr std::uint16_t kBladeShade = 0x5ad6;
  static constexpr std::uint16_t kBladeEdge = 0x4210;
  static constexpr std::uint16_t kHilt = 0x021f;
  static constexpr std::uint16_t kGrip = 0x0010;
  static constexpr std::uint16_t kArc = 0x03bf;

  static void put(OwFramebuffer* frame, int x, int y, std::uint16_t color) {
    if (!frame || x < 0 || y < 0 || x >= static_cast<int>(kOwRenderWidth) ||
        y >= static_cast<int>(kOwRenderHeight))
      return;
    (*frame)[static_cast<std::size_t>(y) * kOwRenderWidth +
             static_cast<std::size_t>(x)] = color;
  }

  struct Direction {
    int forward_x, forward_y;
    int side_x, side_y;
  };

  struct Pose {
    int base_forward, base_side;
    int blade_forward, blade_side;
    int length;
    bool draw_arc;
  };

  static Direction direction(ForeignSwordFacing facing) {
    switch (facing) {
    case ForeignSwordFacing::kNorth: return {0, -1, -1, 0};
    case ForeignSwordFacing::kEast: return {1, 0, 0, -1};
    case ForeignSwordFacing::kSouth: return {0, 1, 1, 0};
    case ForeignSwordFacing::kWest: return {-1, 0, 0, 1};
    }
    return {};
  }

  static Pose pose_for(unsigned elapsed) {
    // Local axes are forward and Link's left. The diagonal wind-up and
    // follow-through make this read as a Minish-style slice while the slim
    // silver/blue silhouette preserves the NES wooden-sword role.
    static constexpr std::array<Pose, kVisibleFrames> kPoses{{
        {-2, 5, 1, -1, 8, false},   // wind-up, behind Link's shoulder
        {0, 4, 1, -1, 10, false},
        {2, 3, 1, -1, 12, true},    // travelling forward slash
        {4, 1, 1, 0, 14, true},     // full extension
        {5, -1, 1, 1, 12, true},
        {4, -3, 1, 1, 10, false},
        {2, -4, 1, 1, 9, false},    // recovery
        {0, -4, 1, 1, 8, false},
        {-1, -3, 1, 1, 6, false},
    }};
    return kPoses[elapsed < kPoses.size() ? elapsed : kPoses.size() - 1];
  }

  static void draw_sword(OwFramebuffer* frame, std::int16_t feet_x,
                         std::int16_t feet_y, ForeignSwordFacing facing,
                         unsigned elapsed) {
    // The focused Minish body remains the live guest sprite. This original
    // BGR555 overlay draws only its sword: no copied game art, OAM mutation,
    // or guest animation write is required.
    const Direction d = direction(facing);
    const Pose pose = pose_for(elapsed);
    const int pivot_x = feet_x;
    const int pivot_y = feet_y - 8;
    const auto point_x = [&](int forward, int side) {
      return pivot_x + d.forward_x * forward + d.side_x * side;
    };
    const auto point_y = [&](int forward, int side) {
      return pivot_y + d.forward_y * forward + d.side_y * side;
    };
    const int perp_forward = -pose.blade_side;
    const int perp_side = pose.blade_forward;

    // Blue crossguard and dark grip make the hand end legible against every
    // Zelda overworld palette. The 14px extended strike is nearly triple the
    // old five-pixel blade and stays wholly compositor-owned.
    for (int guard = -2; guard <= 2; ++guard)
      put(frame, point_x(pose.base_forward + perp_forward * guard,
                         pose.base_side + perp_side * guard),
          point_y(pose.base_forward + perp_forward * guard,
                         pose.base_side + perp_side * guard), kHilt);
    for (int grip = 1; grip <= 3; ++grip)
      put(frame, point_x(pose.base_forward - pose.blade_forward * grip,
                         pose.base_side - pose.blade_side * grip),
          point_y(pose.base_forward - pose.blade_forward * grip,
                         pose.base_side - pose.blade_side * grip), kGrip);
    for (int step = 0; step < pose.length; ++step) {
      const int forward = pose.base_forward + pose.blade_forward * step;
      const int side = pose.base_side + pose.blade_side * step;
      put(frame, point_x(forward, side), point_y(forward, side), kBlade);
      put(frame, point_x(forward + perp_forward, side + perp_side),
          point_y(forward + perp_forward, side + perp_side), kBladeShade);
      if (step > 2)
        put(frame, point_x(forward - perp_forward, side - perp_side),
            point_y(forward - perp_forward, side - perp_side), kBladeEdge);
    }
    put(frame, point_x(pose.base_forward + pose.blade_forward * pose.length,
                       pose.base_side + pose.blade_side * pose.length),
        point_y(pose.base_forward + pose.blade_forward * pose.length,
                       pose.base_side + pose.blade_side * pose.length), kBladeShade);
    if (pose.draw_arc) {
      for (int mark = 2; mark <= 5; ++mark) {
        const int forward = pose.base_forward - pose.blade_forward * mark +
                            perp_forward * (mark - 1);
        const int side = pose.base_side - pose.blade_side * mark +
                         perp_side * (mark - 1);
        put(frame, point_x(forward, side), point_y(forward, side), kArc);
      }
    }
  }

  std::array<OwFramebuffer, 2> buffers_{};
  unsigned published_buffer_ = 0;
  unsigned candidate_buffer_ = 0;
  bool published_host_buffer_ = false;
  bool candidate_host_buffer_ = false;
  bool candidate_prepared_ = false;
  unsigned frames_remaining_ = 0;
  ForeignSwordFacing facing_ = ForeignSwordFacing::kNorth;
};

}  // namespace minish::foreign_world::zelda1
