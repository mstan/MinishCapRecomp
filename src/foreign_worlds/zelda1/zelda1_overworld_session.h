#pragma once

#include "foreign_worlds/foreign_world.h"
#include "foreign_worlds/zelda1/zelda1_cave_control.h"
#include "foreign_worlds/zelda1/zelda1_cave_renderer.h"
#include "foreign_worlds/zelda1/zelda1_live_frame_loader.h"
#include "foreign_worlds/zelda1/zelda1_ow_geometry.h"
#include "foreign_worlds/zelda1/zelda1_octorok_runtime.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace minish::foreign_world::zelda1 {

// Host-side movement result; this deliberately is not an emulated NES button
// or object-state result.
enum class OverworldSessionMoveResult : std::uint8_t {
  kMoved,
  kCrossedRoom,
  kBlocked,
  kNotLoaded,
  kInvalid,
};
enum class OverworldWalkDirection : std::uint8_t {
  kNone = 0,
  kUp = 1,
  kDown = 2,
  kLeft = 3,
  kRight = 4,
};

// The session currently owns only the overworld terrain framebuffer.  Cave
// mode is nevertheless explicit so an adapter cannot continue moving the OW
// avatar while the source would have switched modes.  Rendering the cave
// interior is intentionally a separate, not-yet-proven slice.
enum class OverworldSessionArea : std::uint8_t { kOverworld, kCave };
enum class OverworldSessionCaveResult : std::uint8_t {
  kEntered,
  kReturned,
  kNoEntrance,
  kNotOverworld,
  kNotInCave,
  kInvalid,
};
enum class OverworldSessionSwordResult : std::uint8_t {
  kAcquired,
  kAlreadyAcquired,
  kNotInStartCave,
  kNotAtSourcePickupHotspot,
  kInventoryRejected,
  kInvalid,
};

// Z_01:Anim_WriteSpritePair writes ObjX/ObjY as the left/top OAM coordinate
// of Link's 16x16 sprite and writes its second half at X+$08. The foreign OBJ
// focus ABI, however, takes Link feet: source presentation must therefore use
// bottom-center (+$08,+$10), not the sprite origin. Z_07 collision's
// ObjY+$0b hotspot remains an inset point five pixels above those feet and is
// intentionally not used for visual focus.
inline constexpr std::int16_t kZelda1LinkFeetOffsetX = 0x08;
inline constexpr std::int16_t kZelda1LinkFeetOffsetY = 0x10;

// Presentation feet coordinates are deliberately separate from the retained
// OW portal position while the session is in a cave.
struct CavePresentationPosition {
  std::int16_t x = 0;
  std::int16_t y = 0;
};

struct OverworldSessionPosition {
  std::uint8_t room_id;
  // Renderer/camera crop coordinates.  They intentionally may be negative
  // while Link occupies the source screen-edge collar (ObjX=$00 or
  // ObjY=$3d), which lies outside the 240x160 host crop.
  std::int16_t x;
  std::int16_t y;
};

// Authoritative Zelda object coordinates.  Z_07 collision and
// PlayerScreenEdgeBounds operate here, not in the cropped presentation space.
struct OverworldSourcePosition {
  std::uint8_t room_id;
  std::int16_t obj_x;
  std::int16_t obj_y;
};

// A pure, deterministic session over caller-approved Zelda PRG0 data. The
// source-derived pieces are the final-tile geometry and the x+16*y OW room
// lattice. The virtual coordinates, a 1x1 feet probe, movement order,
// and crop-boundary transition positions are adapter policy for Minish-style
// pixel movement, deliberately separate from NES object/hotspot behavior.
// A transition first tries the opposite crop edge. It preserves the
// perpendicular coordinate where possible; otherwise it deterministically
// selects the closest passable position measured inward from that edge, then
// by perpendicular distance. That fallback is explicit host policy.
class Zelda1OverworldSession {
public:
  Zelda1OverworldSession()
      : framebuffers_{std::make_unique<OwFramebuffer>(),
                      std::make_unique<OwFramebuffer>()} {}
  // Frame buffers are publication-owned identities: copying a session would
  // let two controllers overwrite the same inactive PPU backing store. Live
  // adapters take this session by reference, so prohibit copies explicitly.
  Zelda1OverworldSession(const Zelda1OverworldSession&) = delete;
  Zelda1OverworldSession& operator=(const Zelda1OverworldSession&) = delete;
  Zelda1OverworldSession(Zelda1OverworldSession&&) noexcept = default;
  Zelda1OverworldSession& operator=(Zelda1OverworldSession&&) noexcept = default;
  inline static constexpr std::uint8_t kInitialRoom = 0x77;
  inline static constexpr std::uint8_t kInitialLinkX = 120;
  // The host's retained OW77 entry point is snapped to Z_07's documented
  // ObjGridOffset==0 alignment (raw ObjY=$9d, crop Y=85), rather than the
  // prior unaligned presentation-only Y=80.
  inline static constexpr std::uint8_t kInitialLinkY = 85;
  // The conservative choice for this incomplete adapter is its feet probe:
  // it never permits a sampled source-final-tile solid. Zelda's source
  // collision is hotspot/tile based rather than an 8x8 sprite rectangle.
  inline static constexpr std::uint8_t kLinkFootprintWidth = 1;
  inline static constexpr std::uint8_t kLinkFootprintHeight = 1;
  // v9 additionally persists the source standing-fire ObjAnimFrame and
  // ObjAnimCounter at bytes 23/24. v5/v6/v7/v8 records migrate on restore.
  // Bytes 25..31 are canonical padding;
  // combat remains at byte 32 to keep FWNS's fixed provider footprint.
  inline static constexpr std::size_t kSerializedSize = 64;

  // `path` must already have passed whole-ROM identity validation by the
  // caller. Loader/header validation, initial render, geometry and footprint
  // checks all succeed before this replaces an existing session.
  bool load_hash_validated_ines(const std::filesystem::path &path,
                                std::uint8_t initial_room = kInitialRoom,
                                std::string *error = nullptr) {
    Zelda1LiveFrameLoader loader_candidate;
    if (!loader_candidate.load_and_render_hash_validated_ines(
            path, initial_room, error))
      return false;
    OwRoomGeometry geometry_candidate{};
    if (!loader_candidate.first_quest_data() ||
        !build_overworld_room_geometry(*loader_candidate.first_quest_data(),
                                       initial_room, &geometry_candidate)) {
      set_error(error, "could not build source-backed overworld geometry");
      return false;
    }
    const OverworldSourcePosition source_candidate{
        initial_room, static_cast<std::int16_t>(kInitialLinkX + 8),
        static_cast<std::int16_t>(kInitialLinkY + 72)};
    if (!source_position_is_valid(geometry_candidate, source_candidate.obj_x,
                                  source_candidate.obj_y)) {
      set_error(error,
                "default virtual Link footprint is blocked in requested room");
      return false;
    }
    Ow66OctorokRuntime octorok_candidate;
    OwFramebuffer frame_candidate = loader_candidate.framebuffer();
    if (initial_room == 0x66) {
      if (!octorok_candidate.initialize(*loader_candidate.first_quest_data())) {
        set_error(error, "could not initialize source OW66 Octorok roster");
        return false;
      }
      octorok_candidate.render_qa_markers(&frame_candidate);
    }
    loader_ = std::move(loader_candidate);
    commit_frame(frame_candidate);
    geometry_ = geometry_candidate;
    source_position_ = source_candidate;
    area_ = OverworldSessionArea::kOverworld;
    cave_index_ = 0;
    start_sword_acquired_ = false;
    grid_offset_ = 0;
    walk_direction_ = OverworldWalkDirection::kNone;
    octoroks_ = octorok_candidate;
    loaded_ = true;
    return true;
  }

  [[nodiscard]] bool loaded() const { return loaded_; }
  [[nodiscard]] OverworldSessionPosition position() const {
    return {source_position_.room_id,
            static_cast<std::int16_t>(source_position_.obj_x - 8),
            static_cast<std::int16_t>(source_position_.obj_y - 72)};
  }
  // This is the only OW coordinate suitable for GbaForeignObjFocusTransform's
  // destination_link_feet fields. It intentionally leaves source_position_
  // untouched: ObjY's unadjusted value is still required by CheckWarps and
  // ObjGridOffset cadence.
  [[nodiscard]] OverworldSessionPosition presentation_feet_position() const {
    return {source_position_.room_id,
            static_cast<std::int16_t>(source_position_.obj_x +
                                      kZelda1LinkFeetOffsetX - 8),
            static_cast<std::int16_t>(source_position_.obj_y +
                                      kZelda1LinkFeetOffsetY - 72)};
  }
  [[nodiscard]] OverworldSourcePosition source_position() const {
    return source_position_;
  }
  [[nodiscard]] bool at_source_grid_point() const { return grid_offset_ == 0; }
  [[nodiscard]] OverworldSessionArea area() const { return area_; }
  [[nodiscard]] bool in_cave() const {
    return area_ == OverworldSessionArea::kCave;
  }
  // The exact source cave index (AttrsB & $FC, divided by four), or zero
  // outside a cave.  It is an ID, not a guessed cave-content semantic.
  [[nodiscard]] std::uint8_t cave_index() const { return cave_index_; }
  [[nodiscard]] bool start_sword_acquired() const {
    return start_sword_acquired_;
  }
  [[nodiscard]] bool cave_entry_settled() const {
    return area_ == OverworldSessionArea::kCave && cave_control_.loaded() &&
           !cave_control_.entering();
  }
  [[nodiscard]] bool cave_dialogue_acknowledged() const {
    return area_ == OverworldSessionArea::kCave &&
           cave_control_.dialogue_acknowledged();
  }
  [[nodiscard]] std::uint8_t cave_dialogue_visible_character_count() const {
    return area_ == OverworldSessionArea::kCave
        ? cave_control_.dialogue_visible_character_count() : 0;
  }
  [[nodiscard]] std::optional<CaveSourcePosition> cave_source_position() const {
    if (area_ != OverworldSessionArea::kCave || !cave_control_.loaded())
      return std::nullopt;
    return cave_control_.position();
  }
  [[nodiscard]] std::optional<CavePresentationPosition>
  cave_presentation_position() const {
    const auto source = cave_source_position();
    if (!source) return std::nullopt;
    return CavePresentationPosition{
        static_cast<std::int16_t>(static_cast<int>(source->x) +
                                  kZelda1LinkFeetOffsetX - 8),
        static_cast<std::int16_t>(static_cast<int>(source->y) +
                                  kZelda1LinkFeetOffsetY - 72)};
  }
  [[nodiscard]] const OwFramebuffer &framebuffer() const {
    return *framebuffers_[active_framebuffer_index_];
  }
  [[nodiscard]] const OwRoomGeometry &geometry() const { return geometry_; }
  [[nodiscard]] const Zelda1LiveFrameLoader &loader() const { return loader_; }
  [[nodiscard]] const Ow66OctorokRuntime &octoroks() const { return octoroks_; }
  bool tick_octoroks() {
    if (source_position_.room_id != 0x66 || !octoroks_.initialized()) return false;
    auto candidate = octoroks_;
    candidate.tick();
    return commit_octorok_candidate(candidate);
  }
  bool hit_octorok(std::size_t index, std::uint8_t damage) {
    if (source_position_.room_id != 0x66 || !octoroks_.initialized()) return false;
    auto candidate = octoroks_;
    return candidate.sword_hit(index, damage) &&
           commit_octorok_candidate(candidate);
  }
  bool collect_octorok_drop(std::size_t index) {
    if (source_position_.room_id != 0x66 || !octoroks_.initialized()) return false;
    auto candidate = octoroks_;
    return candidate.collect_drop(index) && commit_octorok_candidate(candidate);
  }

  // Applies a virtual pixel displacement. Components are decomposed in a
  // stable X-then-Y order into one-pixel probes, avoiding diagonal tunneling.
  // A blocked probe stops that axis but does not undo earlier legal pixels.
  OverworldSessionMoveResult move_by(std::int16_t dx, std::int16_t dy) {
    if (!loaded_)
      return OverworldSessionMoveResult::kNotLoaded;
    if (area_ != OverworldSessionArea::kOverworld)
      return OverworldSessionMoveResult::kBlocked;
    if (dx == std::numeric_limits<std::int16_t>::min() ||
        dy == std::numeric_limits<std::int16_t>::min())
      return OverworldSessionMoveResult::kInvalid;
    OverworldSessionMoveResult result = OverworldSessionMoveResult::kMoved;
    // Z_07:GetOppositeDir reduces simultaneous input to one cardinal
    // ObjInputDir. Preserve the historical host X priority rather than
    // attempting two perpendicular source moves in one update.
    if (dx != 0) {
      (void)move_axis(dx, true, &result);
      return result;
    }
    (void)move_axis(dy, false, &result);
    return result;
  }

  [[nodiscard]] std::array<std::uint8_t, kSerializedSize> serialize() const {
    std::array<std::uint8_t, kSerializedSize> out{};
    out[0] = 'Z';
    out[1] = '1';
    out[2] = 'O';
    out[3] = 'S';
    out[4] = 9;
    out[5] = loaded_ ? 1 : 0;
    out[6] = source_position_.room_id;
    write_i16(&out[7], source_position_.obj_x);
    write_i16(&out[9], source_position_.obj_y);
    out[11] = static_cast<std::uint8_t>(area_);
    out[12] = cave_index_;
    out[13] = start_sword_acquired_ ? 1 : 0;
    out[14] = octoroks_.initialized() ? 1 : 0;
    if (area_ == OverworldSessionArea::kCave && cave_control_.loaded()) {
      const auto cave_position = cave_control_.position();
      out[15] = cave_control_.entering() ? 1 : 0;
      out[16] = cave_control_.dialogue_acknowledged() ? 1 : 0;
      out[18] = cave_position.x;
      out[19] = cave_position.y;
      out[21] = cave_control_.dialogue_visible_character_count();
      out[22] = cave_control_.dialogue_frame_delay();
      out[23] = cave_control_.standing_fire_animation_frame();
      out[24] = cave_control_.standing_fire_animation_counter();
    }
    out[17] = static_cast<std::uint8_t>(grid_offset_);
    out[20] = static_cast<std::uint8_t>(walk_direction_);
    if (octoroks_.initialized()) {
      const auto combat = octoroks_.serialize();
      std::copy(combat.begin(), combat.end(), out.begin() + 32);
    }
    return out;
  }

  // Restore requires a session loaded from the same caller-validated PRG.
  // It renders and validates a candidate before exposing any new state.
  bool restore(std::span<const std::uint8_t> blob,
               std::string *error = nullptr) {
    if (!loaded_) {
      set_error(error,
                "cannot restore overworld session before loading PRG0 data");
      return false;
    }
    std::array<std::uint8_t, kSerializedSize> migrated{};
    if (!migrate_serialized(blob, &migrated) || !validate_v9(migrated)) {
      set_error(error, "invalid Zelda1OverworldSession v9 state");
      return false;
    }
    const auto next_area = static_cast<OverworldSessionArea>(migrated[11]);
    const auto &data = *loader_.first_quest_data();
    OverworldRoomView room{};
    if (!data.overworld_room(migrated[6] & 0x0f, migrated[6] >> 4, &room)) {
      set_error(error, "restored room is not source-backed");
      return false;
    }
    const auto entrance = FirstQuestData::decode_overworld_entrance(room);
    if ((next_area == OverworldSessionArea::kOverworld && migrated[12] != 0) ||
        (next_area == OverworldSessionArea::kCave &&
         (entrance.kind != OverworldEntranceKind::kCave ||
          entrance.destination_index != migrated[12] || migrated[12] != 16))) {
      set_error(error,
                "restored cave state does not match source room entrance");
      return false;
    }
    OwRoomGeometry geometry_candidate{};
    OwFramebuffer frame_candidate{};
    const auto restored_x = read_i16(&migrated[7]);
    const auto restored_y = read_i16(&migrated[9]);
    if (!prepare_room(migrated[6], &geometry_candidate, &frame_candidate, error) ||
        (next_area == OverworldSessionArea::kOverworld &&
         !source_position_is_valid(geometry_candidate, restored_x, restored_y))) {
      if (error && error->empty())
        set_error(error, "restored Link footprint is blocked");
      return false;
    }
    Zelda1StartCaveControl cave_candidate;
    if (next_area == OverworldSessionArea::kCave &&
        !restore_cave_control(data, migrated[15] != 0, migrated[16] != 0,
                              migrated[13] != 0, {migrated[18], migrated[19]},
                              migrated[21], migrated[22], migrated[23], migrated[24],
                              &cave_candidate)) {
      set_error(error, "restored start-cave control state is unreachable");
      return false;
    }
    if (next_area == OverworldSessionArea::kCave &&
        !render_cave_frame(data, migrated[13] != 0, migrated[16] != 0,
                           migrated[21], migrated[23],
                           &frame_candidate, error))
      return false;
    Ow66OctorokRuntime combat_candidate;
    const bool combat_present = migrated[14] == 1;
    // OW66 combat state is retained while Link visits other screens, so a
    // defeated source actor cannot silently respawn on a later return.  It is
    // rendered only in room $66; this does not claim source persistence.
    if ((next_area == OverworldSessionArea::kCave && combat_present) ||
        (combat_present && !combat_candidate.restore(std::span<const std::uint8_t>(migrated).subspan(32, Ow66OctorokRuntime::kSerializedSize)) ) ||
        (!combat_present && !std::all_of(migrated.begin()+32, migrated.end(), [](std::uint8_t byte) { return byte == 0; }))) {
      set_error(error, "invalid persisted OW66 Octorok state"); return false;
    }
    geometry_ = geometry_candidate;
    commit_frame(frame_candidate);
    source_position_ = {migrated[6], restored_x, restored_y};
    area_ = next_area;
    cave_index_ = migrated[12];
    start_sword_acquired_ = migrated[13] != 0;
    grid_offset_ = static_cast<std::int8_t>(migrated[17]);
    walk_direction_ = static_cast<OverworldWalkDirection>(migrated[20]);
    cave_control_ = cave_candidate;
    octoroks_ = combat_candidate;
    if (source_position_.room_id == 0x66 && octoroks_.initialized()) {
      OwFramebuffer marked = framebuffer();
      octoroks_.render_qa_markers(&marked);
      commit_frame(marked);
    }
    return true;
  }

  static bool validate_serialized(std::span<const std::uint8_t> blob) {
    std::array<std::uint8_t, kSerializedSize> migrated{};
    return migrate_serialized(blob, &migrated) && validate_v9(migrated);
  }
  static bool validate_v9(const std::array<std::uint8_t, kSerializedSize> &blob) {
    const bool cave = blob[11] == static_cast<std::uint8_t>(OverworldSessionArea::kCave);
    const bool canonical_padding = blob.size() == kSerializedSize &&
        std::all_of(blob.begin() + 25, blob.begin() + 32,
                    [](std::uint8_t byte) { return byte == 0; });
    const bool cave_fields = !cave
        ? blob[12] == 0 && blob[15] == 0 && blob[16] == 0 &&
          blob[18] == 0 && blob[19] == 0 && blob[21] == 0 && blob[22] == 0 &&
          blob[23] == 0 && blob[24] == 0
        : blob[12] == 16 && blob[15] <= 1 && blob[16] <= 1 && blob[17] == 0 &&
          blob[20] == 0 &&
          blob[21] <= Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount &&
          blob[22] <= Zelda1StartCaveControl::kTextboxFramesPerGlyph &&
          blob[23] <= 1 && blob[24] >= 1 &&
          blob[24] <= Zelda1StartCaveControl::kStandingFireFramesPerPhase &&
          !(blob[15] != 0 && (blob[16] != 0 || blob[13] != 0 ||
                              blob[21] != 0 || blob[22] != 0)) &&
          !(blob[16] != 0 &&
            (blob[21] != Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount ||
             blob[22] != 0));
    const auto offset = static_cast<std::int8_t>(blob[17]);
    const auto direction = static_cast<OverworldWalkDirection>(blob[20]);
    const bool walk_segment = offset >= -7 && offset <= 7 &&
        blob[20] <= static_cast<std::uint8_t>(OverworldWalkDirection::kRight) &&
        ((offset == 0) == (direction == OverworldWalkDirection::kNone)) &&
        (offset == 0 || next_grid_segment_is_consistent(read_i16(&blob[7]), read_i16(&blob[9]), offset, direction));
    return blob.size() == kSerializedSize && blob[0] == 'Z' && blob[1] == '1' &&
               blob[2] == 'O' && blob[3] == 'S' && blob[4] == 9 && blob[5] == 1 &&
           blob[6] < kRoomCount &&
           source_coordinates_in_bounds(read_i16(&blob[7]), read_i16(&blob[9])) &&
           blob[11] <= static_cast<std::uint8_t>(OverworldSessionArea::kCave) &&
           blob[13] <= 1 && blob[14] <= 1 && canonical_padding && cave_fields && walk_segment;
  }

  // Model the source's stationary warp gate, rather than treating every
  // cave-attributed screen as enterable. HandleWarpOW requires the final
  // tile to be $24/$88/$70..$73 and, outside the level-6 special case, X
  // aligned to $10 and Y to ($10+$D). The virtual point converts back to
  // the renderer's NES coordinates (x+8, y+72). It is intentionally a
  // one-point host adapter to the source hotspot, not an invented portal
  // rectangle.
  OverworldSessionCaveResult try_enter_cave() {
    if (!loaded_)
      return OverworldSessionCaveResult::kInvalid;
    if (area_ != OverworldSessionArea::kOverworld)
      return OverworldSessionCaveResult::kNotOverworld;
    if (grid_offset_ != 0)
      return OverworldSessionCaveResult::kNoEntrance;
    const unsigned source_x = static_cast<unsigned>(source_position_.obj_x);
    const unsigned source_y = static_cast<unsigned>(source_position_.obj_y);
    // CheckWarps calls GetCollidableTileStill.  That routine is not a raw
    // ObjY lookup: GetCollidableTile first forms Link's collision point at
    // ObjY + $0b, then reads the final-tile map at that point.  Keeping the
    // source alignment test on ObjY while sampling the $0b-offset tile is
    // what puts the trigger at the visible cave mouth rather than a phantom
    // tile eleven pixels above it.
    if ((source_x & 0x0f) != 0 || (source_y & 0x0f) != 0x0d ||
        !ow_source_still_is_warp_trigger(geometry_, source_position_.obj_x,
                                          source_position_.obj_y))
      return OverworldSessionCaveResult::kNoEntrance;
    OverworldRoomView room{};
    if (!loader_.first_quest_data() ||
        !loader_.first_quest_data()->overworld_room(
            source_position_.room_id & 0x0f, source_position_.room_id >> 4, &room))
      return OverworldSessionCaveResult::kInvalid;
    const auto entrance = FirstQuestData::decode_overworld_entrance(room);
    if (entrance.kind != OverworldEntranceKind::kCave)
      return OverworldSessionCaveResult::kNoEntrance;
    // This bounded cave renderer is the source-proven normal cave slot
    // zero only; other source cave slots remain explicitly unsupported.
    if (entrance.destination_index != 16)
      return OverworldSessionCaveResult::kInvalid;
    Zelda1StartCaveControl cave_candidate;
    if (!cave_candidate.load(*loader_.first_quest_data()) ||
        !cave_candidate.settle_entry())
      return OverworldSessionCaveResult::kInvalid;
    OwFramebuffer cave_frame{};
    if (!render_cave_frame(*loader_.first_quest_data(), start_sword_acquired_,
                           false, 0,
                           cave_candidate.standing_fire_animation_frame(),
                           &cave_frame, nullptr))
      return OverworldSessionCaveResult::kInvalid;
    commit_frame(cave_frame);
    area_ = OverworldSessionArea::kCave;
    cave_index_ = entrance.destination_index;
    cave_control_ = cave_candidate;
    grid_offset_ = 0;
    walk_direction_ = OverworldWalkDirection::kNone;
    return OverworldSessionCaveResult::kEntered;
  }

  bool acknowledge_cave_dialogue() {
    if (area_ != OverworldSessionArea::kCave)
      return false;
    Zelda1StartCaveControl candidate_control = cave_control_;
    if (!candidate_control.acknowledge_dialogue()) return false;
    OwFramebuffer candidate{};
    if (!render_cave_frame(*loader_.first_quest_data(), start_sword_acquired_,
                           true, candidate_control.dialogue_visible_character_count(),
                           candidate_control.standing_fire_animation_frame(),
                           &candidate, nullptr))
      return false;
    cave_control_ = candidate_control;
    commit_frame(candidate);
    return true;
  }

  // The cave person's textbox and both standing fires are ordinary per-frame
  // source object updates. Stage the two control mutations and pixels
  // together, so a failed source render cannot advance any persisted state.
  bool tick_cave_frame() {
    if (area_ != OverworldSessionArea::kCave) return false;
    Zelda1StartCaveControl candidate_control = cave_control_;
    const bool dialogue_advanced = candidate_control.tick_dialogue();
    if (!candidate_control.tick_standing_fire()) return false;
    // Unlike text, fire continues after the final `$C0`; therefore a render
    // success is a live update even when the dialogue was already complete.
    if (!dialogue_advanced && !candidate_control.dialogue_acknowledged())
      return false;
    OwFramebuffer candidate{};
    if (!render_cave_frame(*loader_.first_quest_data(), start_sword_acquired_,
                           candidate_control.dialogue_acknowledged(),
                           candidate_control.dialogue_visible_character_count(),
                           candidate_control.standing_fire_animation_frame(),
                           &candidate, nullptr))
      return false;
    cave_control_ = candidate_control;
    commit_frame(candidate);
    return true;
  }

  // Compatibility name for existing adapters/tests. It now advances the
  // whole source cave object frame, including standing-fire animation.
  bool tick_cave_dialogue() { return tick_cave_frame(); }

  StartCaveMoveResult move_cave_one(CaveDirection direction) {
    if (area_ != OverworldSessionArea::kCave) return StartCaveMoveResult::kNotLoaded;
    const auto result = cave_control_.move_one(direction);
    if (result == StartCaveMoveResult::kExited) {
      if (return_from_cave(nullptr) != OverworldSessionCaveResult::kReturned)
        return StartCaveMoveResult::kBlocked;
    }
    return result;
  }

  StartCaveMoveResult move_cave_by(std::int16_t dx, std::int16_t dy) {
    if ((dx != 0 && dy != 0) || (dx == std::numeric_limits<std::int16_t>::min()) ||
        (dy == std::numeric_limits<std::int16_t>::min()))
      return StartCaveMoveResult::kInvalidDirection;
    const std::int16_t amount = dx != 0 ? dx : dy;
    if (amount == 0) return StartCaveMoveResult::kInvalidDirection;
    const CaveDirection direction = dx > 0 ? CaveDirection::kRight : dx < 0 ? CaveDirection::kLeft :
                                    dy > 0 ? CaveDirection::kDown : CaveDirection::kUp;
    const unsigned steps = static_cast<unsigned>(amount < 0 ? -amount : amount);
    for (unsigned i = 0; i != steps; ++i) {
      const auto result = move_cave_one(direction);
      if (result != StartCaveMoveResult::kMoved) return result;
    }
    return StartCaveMoveResult::kMoved;
  }

  // Z_01:UpdateCaveItems takes the one source ware only at the current
  // control position. The renderer is prepared before InventoryCore commits,
  // then both the cave flag and framebuffer update together.
  OverworldSessionSwordResult
  try_take_start_sword(minish::foreign_world::InventoryCore *inventory,
                          std::string *error = nullptr) {
    if (!loaded_ || !inventory)
      return OverworldSessionSwordResult::kInvalid;
    if (area_ != OverworldSessionArea::kCave || cave_index_ != 16)
      return OverworldSessionSwordResult::kNotInStartCave;
    if (start_sword_acquired_)
      return OverworldSessionSwordResult::kAlreadyAcquired;
    if (cave_control_.start_sword_eligibility() != StartCaveSwordEligibility::kEligible)
      return OverworldSessionSwordResult::kNotAtSourcePickupHotspot;
    OwFramebuffer frame_candidate{};
    if (!render_cave_frame(*loader_.first_quest_data(), true, true,
                           Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount,
                           cave_control_.standing_fire_animation_frame(),
                           &frame_candidate, error))
      return OverworldSessionSwordResult::kInvalid;
    using namespace minish::foreign_world;
    const CrossWorldItemId sword{WorldId::Zelda1, 0x01};
    const ItemTraits traits{1, sword, ItemUseKind::Equip,
                            ResourcePoolProvenance::None};
    minish::foreign_world::InventoryCore inventory_candidate = *inventory;
    if (!inventory_candidate.acquire_item(sword, OwnershipFlags::Owned,
                                          Capability::Sword, traits, error))
      return OverworldSessionSwordResult::kInventoryRejected;
    // The cave source grants this exact Zelda-origin record.  It never
    // replaces an already selected compatible sword (including Native); when
    // none is selected, make the new record immediately usable in the first
    // empty A slot.  Everything is staged in inventory_candidate so a later
    // failure leaves both the source pickup and InventoryCore untouched.
    bool compatible_selected = false;
    for (std::size_t slot = 0; slot < kLoadoutSlots; ++slot) {
      const auto use = inventory_candidate.resolve_loadout_use(LoadoutId::A, slot);
      if (!use || use->use_kind != ItemUseKind::Equip)
        continue;
      const auto capabilities = inventory_candidate.capabilities(use->acquired_id);
      if (capabilities && has_capability(*capabilities, Capability::Sword)) {
        compatible_selected = true;
        break;
      }
    }
    if (!compatible_selected) {
      bool selected = false;
      for (std::size_t slot = 0; slot < kLoadoutSlots; ++slot) {
        if (inventory_candidate.loadout_slot(LoadoutId::A, slot))
          continue;
        if (!inventory_candidate.set_loadout_slot(LoadoutId::A, slot, sword, error))
          return OverworldSessionSwordResult::kInventoryRejected;
        selected = true;
        break;
      }
      if (!selected) {
        set_error(error, "no empty A loadout slot for Zelda1 start sword");
        return OverworldSessionSwordResult::kInventoryRejected;
      }
    }
    Zelda1StartCaveControl control_candidate = cave_control_;
    if (!control_candidate.record_start_sword_taken()) {
      set_error(error, "source cave pickup state changed before commit");
      return OverworldSessionSwordResult::kInvalid;
    }
    *inventory = std::move(inventory_candidate);
    cave_control_ = control_candidate;
    commit_frame(frame_candidate);
    start_sword_acquired_ = true;
    return OverworldSessionSwordResult::kAcquired;
  }

  // Returns at InitMode2's initial walking-out coordinate, not its later
  // target coordinate. This is both source-authentic and keeps the host
  // feet probe on the $24 mouth while the original walks Link downward.
  OverworldSessionCaveResult return_from_cave(std::string *error = nullptr) {
    if (!loaded_)
      return OverworldSessionCaveResult::kInvalid;
    if (area_ != OverworldSessionArea::kCave)
      return OverworldSessionCaveResult::kNotInCave;
    if (!cave_control_.loaded() || !cave_control_.dialogue_acknowledged() ||
        cave_control_.position().y != Zelda1StartCaveControl::kExitY) {
      set_error(error, "start cave must use its exact down-exit state");
      return OverworldSessionCaveResult::kInvalid;
    }
    OwCaveReturnHotspot return_hotspot{};
    if (!loader_.first_quest_data() ||
        !overworld_cave_return_hotspot(*loader_.first_quest_data(),
                                       source_position_.room_id, &return_hotspot)) {
      set_error(error, "could not derive source cave return hotspot");
      return OverworldSessionCaveResult::kInvalid;
    }
    OwRoomGeometry geometry_candidate{};
    OwFramebuffer frame_candidate{};
    if (!prepare_room(source_position_.room_id, &geometry_candidate, &frame_candidate,
                      error) ||
        !source_position_is_valid(geometry_candidate, return_hotspot.source_x,
                                  return_hotspot.source_initial_y)) {
      if (error && error->empty())
        set_error(error, "source cave return hotspot is not walkable");
      return OverworldSessionCaveResult::kInvalid;
    }
    geometry_ = geometry_candidate;
    commit_frame(frame_candidate);
    source_position_.obj_x = return_hotspot.source_x;
    source_position_.obj_y = return_hotspot.source_initial_y;
    area_ = OverworldSessionArea::kOverworld;
    cave_index_ = 0;
    cave_control_ = {};
    grid_offset_ = 0;
    walk_direction_ = OverworldWalkDirection::kNone;
    return OverworldSessionCaveResult::kReturned;
  }

private:
  static void set_error(std::string *error, const char *message) {
    if (error)
      *error = message;
  }
  static constexpr std::int16_t kSourceMinX = 0x00;
  static constexpr std::int16_t kSourceMaxX = 0xf0;
  static constexpr std::int16_t kSourceMinY = 0x3d;
  static constexpr std::int16_t kSourceMaxY = 0xdd;

  static void write_i16(std::uint8_t *out, std::int16_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xff);
    out[1] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(value) >> 8) & 0xff);
  }
  static std::int16_t read_i16(const std::uint8_t *in) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(in[0]) |
                                     (static_cast<std::uint16_t>(in[1]) << 8));
  }
  static bool source_coordinates_in_bounds(std::int16_t x, std::int16_t y) {
    return x >= kSourceMinX && x <= kSourceMaxX &&
           y >= kSourceMinY && y <= kSourceMaxY;
  }
  static std::optional<std::uint8_t>
  source_final_tile_at(const OwRoomGeometry &geometry, std::int16_t x,
                       std::int16_t y) {
    // Z_07:GetCollidableTile indexes the full 32x22 final-tile playfield
    // with raw Obj coordinates (Y origin $40), including the renderer's
    // top/left collar.  Do not project through the cropped geometry cells.
    if (x < 0 || x >= 256 || y < 0x40 || y >= 0xf0)
      return std::nullopt;
    const auto tx = static_cast<std::size_t>(x >> 3);
    const auto ty = static_cast<std::size_t>((y - 0x40) >> 3);
    if (tx >= kOwPlayfieldTileWidth || ty >= kOwPlayfieldTileHeight)
      return std::nullopt;
    return geometry.final_tiles[ty * kOwPlayfieldTileWidth + tx];
  }
  static bool source_position_is_valid(const OwRoomGeometry &geometry,
                                       std::int16_t x, std::int16_t y) {
    (void)geometry;
    // A persisted Obj coordinate is not a feet collision point. Z_07 probes
    // direction-specific hotspots (often in a different tile), and Link can
    // legally occupy a source collar/solid-underpoint before the next probe.
    // Bounds are therefore the complete grammar here; all mutations still
    // pass the directional probe before committing.
    return source_coordinates_in_bounds(x, y);
  }
  static bool source_collision_sample_due(std::int16_t x, std::int16_t y,
                                          bool) {
    // Z_07:EnsureObjectAligned describes every ObjGridOffset==0 collision
    // point: X is a multiple of eight and Y is a multiple of eight plus five.
    // This host samples the exact directional probe only at those points.
    return (x & 7) == 0 && (y & 7) == 5;
  }
  static OverworldWalkDirection walk_direction_for(int direction,
                                                    bool horizontal) {
    if (horizontal)
      return direction < 0 ? OverworldWalkDirection::kLeft
                           : OverworldWalkDirection::kRight;
    return direction < 0 ? OverworldWalkDirection::kUp
                         : OverworldWalkDirection::kDown;
  }
  static bool walk_direction_is_horizontal(OverworldWalkDirection direction) {
    return direction == OverworldWalkDirection::kLeft ||
           direction == OverworldWalkDirection::kRight;
  }
  static int walk_direction_sign(OverworldWalkDirection direction) {
    return direction == OverworldWalkDirection::kLeft ||
               direction == OverworldWalkDirection::kUp
           ? -1
           : 1;
  }
  static bool walk_directions_are_opposite(OverworldWalkDirection first,
                                           OverworldWalkDirection second) {
    return walk_direction_is_horizontal(first) ==
               walk_direction_is_horizontal(second) &&
           walk_direction_sign(first) != walk_direction_sign(second);
  }
  static bool next_grid_segment_is_consistent(std::int16_t x, std::int16_t y,
                                              std::int8_t offset,
                                              OverworldWalkDirection direction) {
    if (direction == OverworldWalkDirection::kNone) return offset == 0;
    // The non-moving coordinate stays at the documented source grid origin
    // throughout an in-progress segment; the moving coordinate's low bits
    // encode its signed displacement. ObjDir may temporarily oppose it while
    // Z_05 walks Link back to the previous point, so it cannot constrain the
    // displacement sign here.
    if (walk_direction_is_horizontal(direction))
      return (y & 7) == 5 && (x & 7) == (offset < 0 ? 8 + offset : offset);
    return (x & 7) == 0 && (y & 7) == ((5 + offset) & 7);
  }
  static bool z07_directional_probe_walkable(const OwRoomGeometry &geometry,
                                             std::int16_t obj_x,
                                             std::int16_t obj_y,
                                             int direction, bool horizontal) {
    // Z_07:GetCollidingTileMoving for Link (object slot zero): base Y is
    // ObjY+$0b; up/left test -$08, down +$08, right +$10.  The source
    // suppresses the horizontal offset at its raw X boundaries and the down
    // offset once the hotspot is at/after $dd. Vertical motion
    // samples the adjacent tile column and retains the numerically higher
    // final tile, exactly matching the source's blocking comparison.
    std::int16_t probe_x = obj_x;
    std::int16_t probe_y = static_cast<std::int16_t>(obj_y + 0x0b);
    if (horizontal) {
      if (!((direction < 0 && obj_x < 0x10) ||
            (direction > 0 && obj_x >= 0xf0)))
        probe_x += direction < 0 ? -0x08 : 0x10;
    } else if (!(direction > 0 && probe_y >= 0xdd)) {
      probe_y += direction < 0 ? -0x08 : 0x08;
    }
    auto tile = source_final_tile_at(geometry, probe_x, probe_y);
    if (!tile) return false;
    if (!horizontal) {
      const auto adjacent = source_final_tile_at(geometry,
          static_cast<std::int16_t>(probe_x + 8), probe_y);
      if (!adjacent) return false;
      // Z_07:GetCollidableTile compares `adjacent` against the first tile
      // and keeps the numerically higher value.  Unwalkable final tiles sort
      // after walkable ones, so taking the lower tile inverted vertical
      // collision: visible blockers could be crossed and harmless adjacent
      // terrain could become an invisible wall.
      if (*adjacent > *tile) tile = adjacent;
    }
    return ow_final_tile_is_walkable(*tile);
  }
  static bool migrate_serialized(std::span<const std::uint8_t> blob,
                                 std::array<std::uint8_t, kSerializedSize> *out) {
    if (!out || blob.size() != kSerializedSize || blob[0] != 'Z' ||
        blob[1] != '1' || blob[2] != 'O' || blob[3] != 'S' || blob[5] != 1)
      return false;
    if (blob[4] == 9) {
      std::copy(blob.begin(), blob.end(), out->begin());
      return true;
    }
    if (blob[4] == 8) {
      std::copy(blob.begin(), blob.end(), out->begin());
      (*out)[4] = 9;
      // v8 did not retain standing-fire object bytes. A cave restore resumes
      // at the documented source-boundary canonical phase; OW keeps zeros.
      if ((*out)[11] == static_cast<std::uint8_t>(OverworldSessionArea::kCave)) {
        (*out)[23] = 0;
        (*out)[24] = Zelda1StartCaveControl::kStandingFireFramesPerPhase;
      }
      return true;
    }
    if (blob[4] == 7) {
      std::copy(blob.begin(), blob.end(), out->begin());
      (*out)[4] = 9;
      // v7 had no textbox cursor. A completed legacy cave must retain the
      // source nametable result; an incomplete one resumes from its first
      // transfer rather than retaining the obsolete A-only gate.
      if ((*out)[11] == static_cast<std::uint8_t>(OverworldSessionArea::kCave) &&
          (*out)[16] != 0)
        (*out)[21] = Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount;
      if ((*out)[11] == static_cast<std::uint8_t>(OverworldSessionArea::kCave))
        (*out)[24] = Zelda1StartCaveControl::kStandingFireFramesPerPhase;
      return true;
    }
    if (blob[4] == 6) {
      std::copy(blob.begin(), blob.end(), out->begin());
      (*out)[4] = 9;
      // v6 had byte 17 canonical zero and no direction state. A restored
      // record is therefore at a source grid boundary with no carried input.
      (*out)[17] = 0;
      (*out)[20] = 0;
      if ((*out)[11] == static_cast<std::uint8_t>(OverworldSessionArea::kCave) &&
          (*out)[16] != 0)
        (*out)[21] = Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount;
      if ((*out)[11] == static_cast<std::uint8_t>(OverworldSessionArea::kCave))
        (*out)[24] = Zelda1StartCaveControl::kStandingFireFramesPerPhase;
      return true;
    }
    // v5 persisted cropped uint8 coordinates.  Its grammar is checked
    // before conversion, then its position is losslessly mapped to source
    // ObjX/ObjY by the renderer's fixed (+8,+72) crop inverse.
    if (blob[4] != 5 || blob[6] >= kRoomCount || blob[7] > 239 || blob[8] > 159 ||
        blob[9] > static_cast<std::uint8_t>(OverworldSessionArea::kCave) ||
        blob[11] > 1 || blob[12] > 1 ||
        !std::all_of(blob.begin() + 18, blob.begin() + 32,
                     [](std::uint8_t byte) { return byte == 0; }))
      return false;
    const bool cave = blob[9] == static_cast<std::uint8_t>(OverworldSessionArea::kCave);
    if ((!cave && (blob[10] != 0 || blob[13] != 0 || blob[14] != 0 ||
                   blob[15] != 0 || blob[16] != 0 || blob[17] != 0)) ||
        (cave && (blob[10] != 16 || blob[13] > 1 || blob[14] > 1 ||
                  blob[15] != 0 || (blob[13] && (blob[14] || blob[11])))))
      return false;
    const bool combat_present = blob[12] == 1;
    if ((cave && combat_present) ||
        (combat_present && [&] { Ow66OctorokRuntime candidate; return !candidate.restore(blob.subspan(32, Ow66OctorokRuntime::kSerializedSize)); }()) ||
        (!combat_present && !std::all_of(blob.begin()+32, blob.end(), [](std::uint8_t b){ return b == 0; })))
      return false;
    out->fill(0);
    (*out)[0]='Z'; (*out)[1]='1'; (*out)[2]='O'; (*out)[3]='S'; (*out)[4]=9;
    (*out)[5]=1; (*out)[6]=blob[6];
    write_i16(&(*out)[7], static_cast<std::int16_t>(blob[7] + 8));
    write_i16(&(*out)[9], static_cast<std::int16_t>(blob[8] + 72));
    (*out)[11]=blob[9]; (*out)[12]=blob[10]; (*out)[13]=blob[11]; (*out)[14]=blob[12];
    (*out)[15]=blob[13]; (*out)[16]=blob[14]; (*out)[18]=blob[16]; (*out)[19]=blob[17];
    if ((*out)[11] == static_cast<std::uint8_t>(OverworldSessionArea::kCave) &&
        (*out)[16] != 0)
      (*out)[21] = Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount;
    if ((*out)[11] == static_cast<std::uint8_t>(OverworldSessionArea::kCave))
      (*out)[24] = Zelda1StartCaveControl::kStandingFireFramesPerPhase;
    std::copy(blob.begin()+32, blob.end(), out->begin()+32);
    return true;
  }
  static bool render_cave_frame(const FirstQuestData &data, bool sword_acquired,
                                bool dialogue_acknowledged,
                                std::uint8_t visible_dialogue_characters,
                                std::uint8_t standing_fire_animation_frame,
                                OwFramebuffer *frame, std::string *error) {
    std::vector<std::uint8_t> patterns;
    if (!copy_overworld_background_from_verified_prg(data, &patterns, error) ||
        !render_first_quest_start_cave(data, patterns,
                                       {sword_acquired, dialogue_acknowledged,
                                        visible_dialogue_characters,
                                        standing_fire_animation_frame},
                                       frame)) {
      if (error && error->empty())
        set_error(error, "could not source-render start cave control frame");
      return false;
    }
    return true;
  }
  static bool move_control_axis(Zelda1StartCaveControl *control,
                                std::uint8_t target, bool horizontal) {
    if (!control) return false;
    while ((horizontal ? control->position().x : control->position().y) != target) {
      const auto current = horizontal ? control->position().x : control->position().y;
      const CaveDirection direction = horizontal
          ? (current < target ? CaveDirection::kRight : CaveDirection::kLeft)
          : (current < target ? CaveDirection::kDown : CaveDirection::kUp);
      if (control->move_one(direction) != StartCaveMoveResult::kMoved) return false;
    }
    return true;
  }
  static bool move_control_to(Zelda1StartCaveControl *control,
                              CaveSourcePosition target, bool horizontal_first) {
    if (!control) return false;
    return horizontal_first
        ? move_control_axis(control, target.x, true) &&
              move_control_axis(control, target.y, false)
        : move_control_axis(control, target.y, false) &&
              move_control_axis(control, target.x, true);
  }
  static bool restore_cave_control(const FirstQuestData &data, bool entering,
                                   bool dialogue_acknowledged, bool sword_acquired,
                                   CaveSourcePosition target,
                                   std::uint8_t visible_dialogue_characters,
                                   std::uint8_t dialogue_frame_delay,
                                   std::uint8_t standing_fire_animation_frame,
                                   std::uint8_t standing_fire_animation_counter,
                                   Zelda1StartCaveControl *out) {
    if (!out) return false;
    Zelda1StartCaveControl base;
    if (!base.load(data)) return false;
    if (!base.restore_standing_fire_animation(standing_fire_animation_frame,
                                              standing_fire_animation_counter))
      return false;
    if (entering) {
      if (dialogue_acknowledged || sword_acquired || visible_dialogue_characters != 0 ||
          dialogue_frame_delay != 0 ||
          target.x != Zelda1StartCaveControl::kEntryStart.x ||
          target.y != Zelda1StartCaveControl::kEntryStart.y)
        return false;
      *out = base;
      return true;
    }
    if (!base.settle_entry()) return false;
    if (!dialogue_acknowledged) {
      if (sword_acquired || target.x != Zelda1StartCaveControl::kEntrySettled.x ||
          target.y != Zelda1StartCaveControl::kEntrySettled.y ||
          !base.restore_dialogue_progress(visible_dialogue_characters,
                                          dialogue_frame_delay))
        return false;
      *out = base;
      return true;
    }
    if (visible_dialogue_characters !=
            Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount ||
        dialogue_frame_delay != 0 || !base.acknowledge_dialogue())
      return false;
    if (sword_acquired) {
      Zelda1StartCaveControl with_sword = base;
      if (!move_control_to(&with_sword, {0x78, 0x98}, false) ||
          !with_sword.record_start_sword_taken())
        return false;
      base = with_sword;
    }
    Zelda1StartCaveControl vertical_first = base;
    if (move_control_to(&vertical_first, target, false)) {
      *out = vertical_first;
      return true;
    }
    Zelda1StartCaveControl horizontal_first = base;
    if (move_control_to(&horizontal_first, target, true)) {
      *out = horizontal_first;
      return true;
    }
    return false;
  }
  void commit_frame(const OwFramebuffer &candidate) {
    const unsigned inactive = active_framebuffer_index_ ^ 1u;
    // The two arrays are allocated with the session and never reallocated;
    // a PPU callback holding the old data() address cannot observe this copy.
    *framebuffers_[inactive] = candidate;
    active_framebuffer_index_ = inactive;
  }
  bool commit_octorok_candidate(const Ow66OctorokRuntime &candidate) {
    OwRoomGeometry geometry_candidate{};
    OwFramebuffer frame_candidate{};
    if (!candidate.initialized() ||
        !prepare_room(source_position_.room_id, &geometry_candidate, &frame_candidate,
                      nullptr))
      return false;
    candidate.render_qa_markers(&frame_candidate);
    geometry_ = geometry_candidate;
    commit_frame(frame_candidate);
    octoroks_ = candidate;
    return true;
  }
  bool prepare_room(std::uint8_t room_id, OwRoomGeometry *geometry,
                    OwFramebuffer *frame, std::string *error) const {
    if (!geometry || !frame || !loader_.first_quest_data())
      return false;
    std::vector<std::uint8_t> patterns;
    const auto &data = *loader_.first_quest_data();
    if (!copy_overworld_background_from_verified_prg(data, &patterns, error) ||
        !render_overworld_room(data, room_id, patterns, frame) ||
        !build_overworld_room_geometry(data, room_id, geometry)) {
      if (error && error->empty())
        set_error(error, "could not prepare Zelda 1 overworld room");
      return false;
    }
    return true;
  }
  bool move_axis(std::int16_t amount, bool horizontal,
                 OverworldSessionMoveResult *result) {
    const int direction = amount < 0 ? -1 : 1;
    const auto steps = static_cast<unsigned>(amount < 0 ? -amount : amount);
    for (unsigned i = 0; i < steps; ++i) {
      if (!step_axis(direction, horizontal, result))
        return false;
    }
    return true;
  }
  bool step_axis(int direction, bool horizontal,
                 OverworldSessionMoveResult *result) {
    // Z_05:Link_ModifyDirOnGridLine keeps the active line between points.
    // Opposite input walks back toward the preceding point. A perpendicular
    // request in the first half reverses to that point first; in the second
    // half it keeps the existing line. It never changes axes mid-segment.
    if (grid_offset_ != 0) {
      const auto requested = walk_direction_for(direction, horizontal);
      if (walk_directions_are_opposite(requested, walk_direction_)) {
        walk_direction_ = requested;
      } else if (walk_direction_is_horizontal(requested) !=
                 walk_direction_is_horizontal(walk_direction_)) {
        const int displacement_sign = grid_offset_ < 0 ? -1 : 1;
        const int active_sign = walk_direction_sign(walk_direction_);
        if ((grid_offset_ < 0 ? -grid_offset_ : grid_offset_) < 4 &&
            displacement_sign == active_sign) {
          walk_direction_ = walk_direction_for(
              -active_sign, walk_direction_is_horizontal(walk_direction_));
          grid_offset_ = static_cast<std::int8_t>(
              active_sign > 0 ? -8 + grid_offset_ : 8 + grid_offset_);
        }
      }
      horizontal = walk_direction_is_horizontal(walk_direction_);
      direction = walk_direction_sign(walk_direction_);
    } else {
      walk_direction_ = walk_direction_for(direction, horizontal);
    }
    const auto edge =
        horizontal
            ? (direction < 0 ? OwScreenEdge::kWest : OwScreenEdge::kEast)
            : (direction < 0 ? OwScreenEdge::kNorth : OwScreenEdge::kSouth);
    const auto current = horizontal ? source_position_.obj_x : source_position_.obj_y;
    const auto proposed = static_cast<std::int16_t>(current + direction);
    const auto edge_coordinate = static_cast<std::int16_t>(ow_player_screen_edge_coordinate(edge));
    // Walker_CheckTileCollision runs before MoveObject. Its probe and
    // CheckScreenEdge therefore use the *current* aligned Obj coordinates;
    // checking `proposed` stopped one pixel early and lost the source grid.
    const auto current_x = source_position_.obj_x;
    const auto current_y = source_position_.obj_y;
    if (current != edge_coordinate) {
      if (proposed < (horizontal ? kSourceMinX : kSourceMinY) ||
          proposed > (horizontal ? kSourceMaxX : kSourceMaxY)) {
        *result = OverworldSessionMoveResult::kBlocked;
        return false;
      }
      if (source_collision_sample_due(current_x, current_y, horizontal) &&
          !z07_directional_probe_walkable(geometry_, current_x, current_y, direction,
                                          horizontal)) {
        grid_offset_ = 0;
        walk_direction_ = OverworldWalkDirection::kNone;
        *result = OverworldSessionMoveResult::kBlocked;
        return false;
      }
      if (horizontal) source_position_.obj_x = proposed;
      else source_position_.obj_y = proposed;
      grid_offset_ = static_cast<std::int8_t>(grid_offset_ + direction);
      if (grid_offset_ == 8 || grid_offset_ == -8) {
        grid_offset_ = 0;
        walk_direction_ = OverworldWalkDirection::kNone;
      }
      return true;
    }
    if (!source_collision_sample_due(current_x, current_y, horizontal)) {
      *result = OverworldSessionMoveResult::kBlocked;
      return false;
    }
    std::uint8_t neighbor = 0;
    if (!overworld_neighbor(source_position_.room_id, edge, &neighbor)) {
      *result = OverworldSessionMoveResult::kBlocked;
      return false;
    }
    OwRoomGeometry geometry_candidate{};
    OwFramebuffer frame_candidate{};
    std::string error;
    if (!prepare_room(neighbor, &geometry_candidate, &frame_candidate,
                      &error)) {
      *result = OverworldSessionMoveResult::kInvalid;
      return false;
    }
    OverworldSourcePosition position_candidate = source_position_;
    position_candidate.room_id = neighbor;
    if (horizontal)
      position_candidate.obj_x = direction < 0 ? kSourceMaxX : kSourceMinX;
    else
      position_candidate.obj_y = direction < 0 ? kSourceMaxY : kSourceMinY;
    // Mode 6/7 preserves the perpendicular Obj coordinate and moves to the
    // opposite raw edge; collision is not re-run during that scroll.
    if (!source_coordinates_in_bounds(position_candidate.obj_x,
                                      position_candidate.obj_y)) {
      *result = OverworldSessionMoveResult::kInvalid;
      return false;
    }
    Ow66OctorokRuntime octorok_candidate = octoroks_;
    if (neighbor == 0x66) {
      if (!octorok_candidate.initialized() &&
          !octorok_candidate.initialize(*loader_.first_quest_data())) {
        *result = OverworldSessionMoveResult::kInvalid;
        return false;
      }
      octorok_candidate.render_qa_markers(&frame_candidate);
    }
    source_position_ = position_candidate;
    geometry_ = geometry_candidate;
    commit_frame(frame_candidate);
    octoroks_ = octorok_candidate;
    grid_offset_ = 0;
    walk_direction_ = OverworldWalkDirection::kNone;
    *result = OverworldSessionMoveResult::kCrossedRoom;
    return true;
  }

  Zelda1LiveFrameLoader loader_;
  // A foreign background is consumed directly by the PPU compositor. Static
  // OW frames could safely live in one array, but cave text/fire updates are
  // dynamic: overwriting the currently published array let the compositor
  // observe its terrain pass before the source sprite/text pass completed.
  // Keep both backing arrays alive and flip only after the inactive one has
  // received a completed candidate. The immediately previous PPU pointer
  // remains immutable through the next source update.
  std::array<std::unique_ptr<OwFramebuffer>, 2> framebuffers_{};
  unsigned active_framebuffer_index_ = 0;
  OwRoomGeometry geometry_{};
  OverworldSourcePosition source_position_{};
  OverworldSessionArea area_ = OverworldSessionArea::kOverworld;
  std::uint8_t cave_index_ = 0;
  bool start_sword_acquired_ = false;
  Zelda1StartCaveControl cave_control_{};
  bool loaded_ = false;
  Ow66OctorokRuntime octoroks_{};
  // Host representation of Z_07's signed ObjGridOffset/ObjDir segment.
  // It is zero at an aligned source grid point and otherwise in [-7,7].
  std::int8_t grid_offset_ = 0;
  OverworldWalkDirection walk_direction_ = OverworldWalkDirection::kNone;
};

} // namespace minish::foreign_world::zelda1
