#pragma once

#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"
#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"

#include <array>
#include <cstdint>
#include <span>

namespace minish::foreign_world::zelda1 {

// Bounded OW $66 combat state. Roster, positions, and packed HP are source
// facts; the update cadence and one-pixel axis movement are explicit host
// policy until Wanderer_TargetPlayer is translated instruction-for-instruction.
struct OctorokActorState {
  std::uint8_t source_type{};
  std::uint8_t hp{};
  std::uint8_t x{};
  std::uint8_t y{};
  bool defeated{};
  bool pending_drop{}; // Zelda's drop choice remains RNG/table work.
};

class Ow66OctorokRuntime {
public:
  static constexpr std::size_t kActorCount = 4, kSerializedSize = 32;
  bool initialize(const FirstQuestData &data,
                  std::uint8_t spawn_direction = 0) {
    OverworldRoomView room{};
    OverworldEnemyRosterView roster{};
    if (!data.overworld_room(6, 6, &room) ||
        !data.overworld_enemy_roster(room, spawn_direction, &roster))
      return false;
    for (unsigned i = 0; i < kActorCount; ++i) {
      const auto type = roster.spawns[i].source_type;
      if (type != 0x07 && type != 0x08)
        return false;
      actors_[i] = {type,
                    static_cast<std::uint8_t>(type == 0x07 ? 1 : 3),
                    roster.spawns[i].source_x,
                    roster.spawns[i].source_y,
                    false,
                    false};
    }
    tick_ = 0;
    initialized_ = true;
    return true;
  }
  [[nodiscard]] bool initialized() const { return initialized_; }
  [[nodiscard]] std::uint8_t source_room() const { return 0x66; }
  void tick() {
    if (!initialized_)
      return;
    ++tick_;
    for (auto &a : actors_)
      if (!a.defeated) {
        // Source speed distinguishes slow ($20) and fast ($40); preserve that
        // ratio as host-pixel cadence, with a stable RNG-free direction cycle.
        const unsigned period = a.source_type == 0x07 ? 2 : 1;
        // Bounded host policy: stop at the source play-area bound rather than
        // wrapping through unknown terrain; geometry integration may refine
        // this.
        if ((tick_ % period) == 0 && a.x < 0xd0)
          ++a.x;
      }
  }
  bool sword_hit(std::size_t index, std::uint8_t source_damage) {
    if (!initialized_ || index >= kActorCount || actors_[index].defeated ||
        source_damage == 0)
      return false;
    auto &a = actors_[index];
    a.hp = source_damage >= a.hp
               ? 0
               : static_cast<std::uint8_t>(a.hp - source_damage);
    if (!a.hp) {
      a.defeated = true;
      a.pending_drop = true;
    }
    return true;
  }
  bool contact(std::size_t index, std::uint8_t source_x, std::uint8_t source_y,
               std::uint8_t *damage) const {
    if (!initialized_ || !damage || index >= kActorCount)
      return false;
    const auto &a = actors_[index];
    if (a.defeated || source_x < a.x || source_x > a.x + 15 || source_y < a.y ||
        source_y > a.y + 15)
      return false;
    *damage = 1;
    return true;
  }
  bool collect_drop(std::size_t index) {
    if (!initialized_ || index >= kActorCount || !actors_[index].pending_drop)
      return false;
    actors_[index].pending_drop = false;
    return true;
  }
  [[nodiscard]] const std::array<OctorokActorState, kActorCount> &
  actors() const {
    return actors_;
  }
  [[nodiscard]] std::uint32_t tick_count() const { return tick_; }
  // TEMPORARY QA overlay, deliberately not Zelda art: source Octorok
  // metasprite descriptors/CHR selection remain under investigation. A
  // magenta 8x8 square makes the source-backed actor positions observable.
  void render_qa_markers(OwFramebuffer *frame) const {
    if (!initialized_ || !frame)
      return;
    constexpr std::uint16_t kQaMarkerBgr555 = 0x7c1f;
    for (const auto &a : actors_)
      if (!a.defeated) {
        const int x0 = static_cast<int>(a.x) - 8,
                  y0 = static_cast<int>(a.y) - 72;
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; ++x)
            if (x0 + x >= 0 && x0 + x < 240 && y0 + y >= 0 && y0 + y < 160)
              (*frame)[(y0 + y) * 240 + x0 + x] = kQaMarkerBgr555;
      }
    // TEMPORARY QA drop indicator. Cyan is intentionally distinct from the
    // living-actor magenta and is not claimed as Zelda item art.
    constexpr std::uint16_t kQaDropMarkerBgr555 = 0x03ff;
    for (const auto &a : actors_)
      if (a.pending_drop) {
        const int x0 = static_cast<int>(a.x) - 8,
                  y0 = static_cast<int>(a.y) - 72;
        for (int y = 2; y < 6; ++y)
          for (int x = 2; x < 6; ++x)
            if (x0 + x >= 0 && x0 + x < 240 && y0 + y >= 0 && y0 + y < 160)
              (*frame)[(y0 + y) * 240 + x0 + x] = kQaDropMarkerBgr555;
      }
  }
  [[nodiscard]] std::array<std::uint8_t, kSerializedSize> serialize() const {
    std::array<std::uint8_t, kSerializedSize> b{};
    b[0] = 'Z';
    b[1] = '1';
    b[2] = 'O';
    b[3] = 'R';
    b[4] = 1;
    for (unsigned i = 0; i < 4; ++i)
      b[5 + i] = static_cast<std::uint8_t>(tick_ >> (i * 8));
    for (unsigned i = 0; i < kActorCount; ++i) {
      const auto &a = actors_[i];
      auto p = 9 + i * 5;
      b[p] = a.source_type;
      b[p + 1] = a.hp;
      b[p + 2] = a.x;
      b[p + 3] = a.y;
      b[p + 4] = (a.defeated ? 1 : 0) | (a.pending_drop ? 2 : 0);
    }
    return b;
  }
  bool restore(std::span<const std::uint8_t> b) {
    if (b.size() != kSerializedSize || b[0] != 'Z' || b[1] != '1' ||
        b[2] != 'O' || b[3] != 'R' || b[4] != 1)
      return false;
    for (unsigned i = 29; i < 32; ++i)
      if (b[i])
        return false;
    std::array<OctorokActorState, kActorCount> n{};
    std::uint32_t t{};
    for (unsigned i = 0; i < 4; ++i)
      t |= std::uint32_t(b[5 + i]) << (i * 8);
    for (unsigned i = 0; i < kActorCount; ++i) {
      auto p = 9 + i * 5;
      const auto f = b[p + 4];
      const bool dead = f & 1, drop = f & 2;
      if ((b[p] != 7 && b[p] != 8) || f > 3 || b[p + 2] < 0x50 ||
          b[p + 2] > 0xd0 || b[p + 3] < 0x50 || b[p + 3] > 0xd0 ||
          b[p + 1] > (b[p] == 7 ? 1 : 3) || (!dead && !b[p + 1]) ||
          (dead && b[p + 1]) || (drop && !dead))
        return false;
      n[i] = {b[p], b[p + 1], b[p + 2], b[p + 3], dead, drop};
    }
    actors_ = n;
    tick_ = t;
    initialized_ = true;
    return true;
  }

private:
  std::array<OctorokActorState, kActorCount> actors_{};
  std::uint32_t tick_{};
  bool initialized_{};
};
} // namespace minish::foreign_world::zelda1
