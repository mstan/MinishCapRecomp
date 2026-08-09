#include "foreign_worlds/zelda1/smith_yard_readiness_plugin.h"
#include "foreign_worlds/zelda1/smith_yard_qa_room.h"
#include "foreign_worlds/foreign_world_native_state.h"
#include "foreign_worlds/zelda1/minish_foreign_combat_adapter.h"
#include "foreign_worlds/zelda1/minish_linear_move_adapter.h"
#include "foreign_worlds/zelda1/minish_portal_readiness.h"
#include "foreign_worlds/zelda1/zelda1_level1_live_adapter.h"
#include "foreign_worlds/zelda1/zelda1_overworld_session.h"

#include <string>

#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace minish::foreign_world::zelda1 {
namespace {

// Source: pinned zeldaret/tmc linker.ld and include/room.h.
constexpr std::uint32_t kRoomControls = 0x03000BF0;
constexpr std::uint32_t kRoomControlsArea = kRoomControls + 0x04;
constexpr std::uint32_t kRoomControlsRoom = kRoomControls + 0x05;
constexpr std::uint32_t kRoomTransition = 0x030010A0;
constexpr std::uint32_t kRoomFrameCount = kRoomTransition;
constexpr std::uint32_t kTransitioningOut = kRoomTransition + 0x08;
constexpr std::uint32_t kUpdateEntities = 0x0805E5C0;
// Source-imported `ram_UpdateEntities` is the IWRAM copy Minish actually
// executes each frame. It remains a read-only lifecycle hook; PPU rendering
// no longer needs a guest-frame reblit or any guest VRAM/MMIO writes.
constexpr std::uint32_t kRamUpdateEntities = 0x03005F40;
// Source-mapped gPlayerState and gPlayerEntity fields. These are read through
// the guest bus only; the full South Hyrule Field readiness contract below
// owns normal-action/control validation without a high-frequency player hook.
constexpr std::uint32_t kPlayerControlMode = 0x0300400B; // gPlayerState + 0x8B
constexpr std::uint32_t kPlayerAction = 0x0300116C; // gPlayerEntity + Entity.action
constexpr std::uint32_t kPlayerAnimationState = 0x0300117C; // + Entity.animationState
constexpr std::uint32_t kGameMainSubstate = 0x03001004;
constexpr std::uint32_t kPlayerKind = 0x03001168, kPlayerFlags = 0x03001170;
constexpr std::uint32_t kPlayerDraw = 0x03001178, kPlayerKilled = 0x03003FBC;
constexpr std::uint32_t kPlayerMoveLocks = 0x03003FB0, kPausePlayer = 0x03003F8A;
constexpr std::uint32_t kPlayerMacro = 0x0300401C, kMessageState = 0x02000050;
constexpr std::uint32_t kPriorityTimer = 0x03003DC8;
// Source: Entity x/y at +0x2C/+0x30, RoomControls scroll at +0x0A/+0x0C.
// The high Q16.16 half is an immutable source feet anchor for the PPU-only
// transform; it is never written by this plugin.
constexpr std::uint32_t kPlayerFeetX = 0x0300118E, kPlayerFeetY = 0x03001192;
// Source: Entity::spriteVramOffset at +0x60 (include/entity.h), allocated
// in 16-tile OBJ units by src/vram.c. This read-only source fact identifies
// Link's composite OAM allocation without moving guest OAM or room props.
constexpr std::uint32_t kPlayerSpriteVramOffset = 0x030011C0;
// Read-only OAM capture with `Entity::spriteSettings.shadow` enabled records
// its 16x8 player shadow as OBJ tile 0x001, immediately after the player body
// entries.  Carry that bounded auxiliary allocation with the player rather
// than leaving a frozen teal oval over a foreign room.
constexpr std::uint16_t kPlayerShadowObjTile = 0x001;
constexpr std::uint32_t kRoomScrollX = 0x03000BFA, kRoomScrollY = 0x03000BFC;
constexpr std::uint32_t kLinearMoveDirectionOld = 0x080027EA;
// Source: zeldaret/tmc linker.ld, include/player.h, and playerUtils.c.
// gActiveItems[0] is the CreateItem3 lane used by all ItemSword definitions.
constexpr std::uint32_t kActiveItem0 = 0x03000B80;
constexpr std::uint32_t kActiveItemBehaviorId = kActiveItem0 + 0x01;
constexpr std::uint32_t kActiveItemPriority = kActiveItem0 + 0x09;
constexpr std::uint32_t kActiveItemPlayerAnimationState = kActiveItem0 + 0x0A;
constexpr std::uint32_t kPlayerAttackStatus = 0x03003F84; // PlayerState + 0x04
constexpr std::uint32_t kPlayerSwordState = 0x03003F9B;   // PlayerState + 0x1B

SmithYardReadinessTracker g_tracker;
MinishPortalReadinessTracker g_portal_tracker;
SmithYardQaRoom g_qa_room;
bool g_asset_available = false;
bool g_qa_video_active = false;
bool g_foreign_survival_safe = false;
Zelda1OverworldSession g_overworld_session;
Zelda1Level1LiveAdapter g_level1_adapter;
ForeignObjFocusDoubleBuffer g_obj_focus;
ActiveLowButtonEdge g_cave_a_edge;
ActiveLowButtonEdge g_combat_a_edge;
MinishForeignCombatAdapter g_foreign_combat;
bool g_combat_frame_seen = false;
std::uint32_t g_last_combat_frame = 0;
bool g_locomotion_frame_seen = false;
std::uint32_t g_last_locomotion_frame = 0;
bool g_presentation_restore_pending = false;
std::uint64_t g_seen_native_restore_generation = 0;

std::int16_t read_guest_s16(std::uint32_t address) {
    return static_cast<std::int16_t>(bus_read_u16(address));
}

bool publish_current_foreign_video() {
    if (!g_overworld_session.loaded()) return false;
    const auto level1 = g_level1_adapter.presentation();
    const bool level1_active = level1.active && level1.framebuffer;
    if (gba_mod_publish_foreign_background(
            kZelda1ForeignWorldPluginId,
            level1_active ? level1.framebuffer->data()
                          : g_overworld_session.framebuffer().data()) == 0)
        return false;
    const auto position = g_overworld_session.position();
    const auto cave_position = g_overworld_session.cave_presentation_position();
    const auto* focus = g_obj_focus.next(
        static_cast<std::int16_t>(read_guest_s16(kPlayerFeetX) -
                                  read_guest_s16(kRoomScrollX)),
        static_cast<std::int16_t>(read_guest_s16(kPlayerFeetY) -
                                  read_guest_s16(kRoomScrollY)),
        level1_active ? level1.player.x :
            (cave_position ? cave_position->x : static_cast<std::int16_t>(position.x)),
        level1_active ? level1.player.y :
            (cave_position ? cave_position->y : static_cast<std::int16_t>(position.y)),
        bus_read_u16(kPlayerSpriteVramOffset), kPlayerShadowObjTile,
        (bus_read_u8(kPlayerDraw) & 0x30u) != 0 ? 1u : 0u);
    if (gba_mod_publish_foreign_obj_focus(kZelda1ForeignWorldPluginId, focus) == 0) {
        gba_mod_clear_foreign_obj_focus();
        gba_mod_clear_foreign_background();
        return false;
    }
    return true;
}

void leave_qa_video();

void enter_qa_video() {
    // The PPU reads immutable session pixels and a data-only Link focus
    // descriptor during its normal composition. No guest VRAM, OAM, entity,
    // input, or coordinate bytes are touched.
    g_obj_focus.reset();
    g_cave_a_edge.reset();
    g_combat_a_edge.reset();
    g_locomotion_frame_seen = false;
    g_qa_video_active = publish_current_foreign_video();
    foreign_world_native_state().set_zelda1_presentation_active(g_qa_video_active);
    if (!g_qa_video_active) leave_qa_video();
}

void leave_qa_video() {
    gba_mod_clear_foreign_obj_focus();
    gba_mod_clear_foreign_background();
    g_qa_video_active = false;
    g_foreign_survival_safe = false;
    g_cave_a_edge.reset();
    g_combat_a_edge.reset();
    g_foreign_combat.reset();
    g_level1_adapter.reset();
    g_combat_frame_seen = false;
    g_locomotion_frame_seen = false;
    g_presentation_restore_pending = false;
    foreign_world_native_state().set_zelda1_presentation_active(false);
}

bool sync_live_overworld_session() {
    if (!g_overworld_session.loaded()) return false;
    const auto state = g_overworld_session.serialize();
    std::string error;
    return foreign_world_native_state().set_zelda1_overworld_session_blob(
        state, &error);
}

// Snapshot restore occurs in the trusted native-state provider before the
// next guest hook. Rebuild the already ROM-validated live session from its
// attached Z1OS record before any publish or host mutation. The generation is
// host-only coordination state, so ordinary checkpoint writes never recurse.
bool apply_pending_native_restore() {
    auto& native = foreign_world_native_state();
    const auto generation = native.restore_generation();
    if (generation == g_seen_native_restore_generation) return true;
    g_seen_native_restore_generation = generation;
    if (!g_overworld_session.loaded()) return false;
    const auto& blob = native.zelda1_overworld_session_blob();
    // A provider restore that predates Z1OS v5 has no authoritative live
    // session to restore. Do not retain a newer in-memory foreign position
    // across that snapshot boundary: exit presentation instead.
    if (blob.empty()) return false;
    std::string error;
    if (!g_overworld_session.restore(blob, &error)) return false;
    // The native record currently owns the OW session only.  Never carry a
    // newer in-memory Level 1 child across that snapshot boundary.
    g_level1_adapter.reset();
    g_cave_a_edge.reset();
    g_combat_a_edge.reset();
    g_foreign_combat.reset();
    g_combat_frame_seen = false;
    g_locomotion_frame_seen = false;
    g_qa_room.restore_active(native.zelda1_presentation_active());
    g_qa_video_active = false;
    g_presentation_restore_pending = native.zelda1_presentation_active();
    if (!g_presentation_restore_pending) {
        g_obj_focus.reset();
        gba_mod_clear_foreign_obj_focus();
        gba_mod_clear_foreign_background();
    }
    return true;
}

void tick_foreign_combat_once(std::uint16_t keyinput) {
    // gRoomTransition.frameCount is incremented by GameMain once per game
    // frame (pinned src/game.c). The plugin observes both ROM/IWRAM entry
    // addresses during startup relocation, so this rejects an accidental
    // duplicate callback without relying on a host render cadence.
    const std::uint32_t frame = bus_read_u32(kRoomFrameCount);
    if (g_combat_frame_seen && g_last_combat_frame == frame) return;
    g_combat_frame_seen = true;
    g_last_combat_frame = frame;
    const MinishSwordSample sword{
        bus_read_u8(kActiveItemBehaviorId),
        bus_read_u8(kActiveItemPriority),
        bus_read_u8(kActiveItemPlayerAnimationState),
        bus_read_u8(kPlayerSwordState),
        bus_read_u8(kPlayerAttackStatus),
    };
    // Sample every active foreign frame, including cave/Level-1 frames, so a
    // held A cannot become a synthetic rising edge merely by entering OW66.
    // This remains a read-only KEYINPUT observation.
    const bool selected_sword_edge = g_combat_a_edge.observe(keyinput, kGbaKeyA);
    if (g_level1_adapter.active()) {
        // Aquamentus receives only the existing source-observed Minish sword
        // edge. This path reads guest state but creates no guest entity or
        // guest write; the adapter owns its host-side framebuffer/state.
        std::string error;
        const auto result = g_level1_adapter.observe_sword(
            sword, &foreign_world_native_state().inventory(), LoadoutId::A,
            selected_sword_edge, bus_read_u8(kPlayerAnimationState), &error);
        if (result == Level1LiveResult::kInvalid ||
            result == Level1LiveResult::kInventoryRejected)
            leave_qa_video();
        return;
    }
    // The combat seam consumes the session's canonical Zelda object
    // coordinates, never a reconstructed crop offset. Fail closed before
    // narrowing the signed source position to the host actor byte range.
    const auto source_position = g_overworld_session.source_position();
    if (source_position.obj_x < 0 || source_position.obj_x > 0xff ||
        source_position.obj_y < 0 || source_position.obj_y > 0xff)
        return;
    const ForeignCombatInput input{
        true, g_foreign_survival_safe, source_position.room_id,
        static_cast<std::uint8_t>(source_position.obj_x),
        static_cast<std::uint8_t>(source_position.obj_y), sword, LoadoutId::A,
        selected_sword_edge, bus_read_u8(kPlayerAnimationState)};
    const auto events = g_foreign_combat.tick(
        input, &foreign_world_native_state().inventory(),
        g_overworld_session.octoroks());
    if (!events.tick_octoroks) return;
    // Session mutation is entirely host-side. No Minish memory, health,
    // animation, item behavior, or input byte is written by this bridge.
    (void)g_overworld_session.tick_octoroks();
    for (std::size_t i = 0; i < events.sword_hit_count; ++i)
        (void)g_overworld_session.hit_octorok(events.sword_hits[i], 1);
    for (std::size_t i = 0; i < events.drop_collect_count; ++i)
        (void)g_overworld_session.collect_octorok_drop(events.drop_collects[i]);
}

bool observe_cave_interaction(std::uint16_t keyinput) {
    if (g_level1_adapter.active()) {
        g_cave_a_edge.reset();
        return true;
    }
    if (!g_overworld_session.in_cave()) {
        g_cave_a_edge.reset();
        return true;
    }
    // Explicit, active-low A edges only. The first edge dismisses the
    // source dialogue; later edges attempt the source control's exact sword
    // hotspot, so an A press elsewhere cannot manufacture an item.
    if (!g_cave_a_edge.observe(keyinput, kGbaKeyA)) return true;
    if (!g_overworld_session.cave_dialogue_acknowledged()) {
        (void)g_overworld_session.acknowledge_cave_dialogue();
    } else {
        std::string error;
        (void)g_overworld_session.try_take_start_sword(
            &foreign_world_native_state().inventory(), &error);
    }
    return true;
}

// CheckWarps is a per-update source routine, not a side effect reserved for
// a successful movement probe.  In particular, a player who releases the
// D-pad on a source-aligned $24/$88/$70..$73 tile must still be able to take
// the normal cave/level transition on the next update.  Keep this seam
// host-only: both callees consume the decoded Zelda state and only exchange
// immutable presentation state with the renderer.
bool observe_overworld_entrance() {
    if (g_overworld_session.in_cave() || g_level1_adapter.active())
        return false;
    if (g_overworld_session.try_enter_cave() ==
        OverworldSessionCaveResult::kEntered)
        return true;
    return g_level1_adapter.try_enter_from_overworld(g_overworld_session) ==
        Level1LiveResult::kEntered;
}

// Performs exactly one host-side Zelda movement pixel.  This is deliberately
// driven from the source frame observer, not a Minish collision callback:
// native house walls can prevent LinearMoveDirectionOLD from running even
// while the foreign PPU world remains active.  The function never touches
// guest memory and its bool means that a diagonal's X probe consumed the
// source move (collision, transition, or foreign-world entry).
bool move_foreign_one_pixel(std::int16_t dx, std::int16_t dy) {
    if (dx == 0 && dy == 0) return true;
    if (g_level1_adapter.active()) {
        auto& inventory = foreign_world_native_state().inventory();
        std::uint8_t returned_room{};
        const auto result = g_level1_adapter.move_by(
            dx, dy, &inventory, LoadoutId::A, &returned_room);
        if (result == Level1LiveResult::kInvalid ||
            result == Level1LiveResult::kInventoryRejected) {
            leave_qa_video();
            return false;
        }
        if (result != Level1LiveResult::kMoved) return false;
        std::string error;
        const auto collected = g_level1_adapter.collect_room_item(&inventory, &error);
        if (collected == Level1LiveResult::kInvalid ||
            collected == Level1LiveResult::kInventoryRejected) {
            leave_qa_video();
            return false;
        }
        (void)returned_room;
        return true;
    }
    if (g_overworld_session.in_cave()) {
        const auto result = g_overworld_session.move_cave_by(dx, dy);
        return result == StartCaveMoveResult::kMoved;
    }
    const auto movement = g_overworld_session.move_by(dx, dy);
    if (movement != OverworldSessionMoveResult::kMoved) return false;
    // Both entrances are source-coordinate hotspots. This probe is made
    // after every virtual pixel, including either half of a diagonal.
    if (observe_overworld_entrance())
        return false;
    return true;
}

void tick_foreign_locomotion_once(std::uint16_t keyinput) {
    const std::uint32_t frame = bus_read_u32(kRoomFrameCount);
    if (g_locomotion_frame_seen && g_last_locomotion_frame == frame) return;
    g_locomotion_frame_seen = true;
    g_last_locomotion_frame = frame;
    // KEYINPUT is active-low. Opposite held directions cancel per axis;
    // diagonals take their horizontal source pixel before vertical one.
    const bool right = (keyinput & kGbaKeyRight) == 0;
    const bool left = (keyinput & kGbaKeyLeft) == 0;
    const bool up = (keyinput & kGbaKeyUp) == 0;
    const bool down = (keyinput & kGbaKeyDown) == 0;
    const std::int16_t dx = right == left ? 0 : (right ? 1 : -1);
    const std::int16_t dy = down == up ? 0 : (down ? 1 : -1);
    const auto before = g_overworld_session.serialize();
    tick_foreign_combat_once(keyinput);
    if (!g_qa_room.active() || !g_qa_video_active || !g_foreign_survival_safe)
        return;
    if (dx != 0 && move_foreign_one_pixel(dx, 0) &&
        !g_overworld_session.in_cave() && !g_level1_adapter.active())
        (void)move_foreign_one_pixel(0, dy);
    else if (dx == 0)
        (void)move_foreign_one_pixel(0, dy);
    // Zelda's CheckWarps runs at the end of every ordinary play update.  The
    // per-pixel path above handles a moving crossing; this second observation
    // supplies the source's stationary case and also remains available after
    // a blocked source probe.  It is deliberately after movement so a D-pad
    // press can leave a warp tile before it is considered, as in the source.
    (void)observe_overworld_entrance();
    if (g_qa_room.active() && g_qa_video_active && g_foreign_survival_safe &&
        !observe_cave_interaction(keyinput))
        return;
    // Movement, cave dialogue/sword actions, and the presentation descriptor
    // have one checkpoint/publication decision for this source frame.
    if (g_overworld_session.serialize() != before && !sync_live_overworld_session()) {
        leave_qa_video();
        return;
    }
    if (g_qa_room.active() && g_qa_video_active && !publish_current_foreign_video())
        leave_qa_video();
}

int observe_update_entities(std::uint32_t, int, ArmCpuState*) {
    // Read only documented global fields through the runtime's guest bus API.
    // Do not allocate entities, call guest code, or modify ArmCpuState.
    if (g_asset_available) {
        if (!apply_pending_native_restore()) {
            leave_qa_video();
            return 0;
        }
        const std::uint8_t area = bus_read_u8(kRoomControlsArea);
        const std::uint8_t room = bus_read_u8(kRoomControlsRoom);
        const bool transitioning_out = bus_read_u8(kTransitioningOut) != 0;
        // Build the complete source-mapped, read-only gate for the intended
        // South Hyrule Field portal (area 3, room 1). It rejects cutscenes,
        // messages, transitions, hidden/dead Link, macro input, and movement
        // locks before the temporary chord can affect presentation.
        const bool normal_player_ready = bus_read_u8(kPlayerControlMode) == 0 &&
            (bus_read_u32(kPlayerMoveLocks) & 0x22189B75u) == 0 &&
            bus_read_u8(kPlayerAction) == 1;
        const MinishPortalReadinessInput portal_input{
            g_asset_available, area, room, bus_read_u8(kGameMainSubstate) == 2,
            transitioning_out,
            bus_read_u8(kPlayerKind) == 1 && (bus_read_u8(kPlayerFlags) & 0x10) == 0 &&
                bus_read_u8(kPlayerKilled) == 0,
            (bus_read_u8(kPlayerDraw) & 3) != 0,
            normal_player_ready,
            bus_read_u32(kPlayerMacro) != 0 || (bus_read_u8(kPausePlayer) & 0x80) != 0 ||
                (bus_read_u8(kMessageState) & 0x7f) != 0 || bus_read_u16(kPriorityTimer) != 0};
        g_portal_tracker.observe(portal_input);
        g_tracker.observe({
            true,
            area,
            room,
            transitioning_out,
            normal_player_ready ? NormalControlGate::VerifiedNormal
                                     : NormalControlGate::Unknown,
        });
        // Entry requires the whole source-mapped normal-control gate. Once
        // active, transient sword, item, and roll actions intentionally do
        // not dismiss the foreign session: only a room/lifecycle danger does.
        const bool survival_safe = area == 3 && room == 1 &&
            portal_input.game_main_update && !portal_input.room_transitioning_out &&
            portal_input.player_entity_alive && portal_input.player_entity_drawn &&
            !portal_input.script_or_cutscene_locked;
        g_foreign_survival_safe = survival_safe;
        const std::uint16_t keyinput = bus_read_u16(0x04000130);
        // A provider restore has rebuilt Z1OS before this point. Rebind its
        // lifecycle-owned presentation only after the normal read-only
        // survival gate is available; inactive snapshots explicitly clear.
        if (g_presentation_restore_pending) {
            g_presentation_restore_pending = false;
            if (survival_safe) {
                g_qa_video_active = publish_current_foreign_video();
                if (!g_qa_video_active) leave_qa_video();
            } else {
                leave_qa_video();
            }
        }
        const auto qa_event = g_qa_room.update(g_portal_tracker.state().ready(),
                                               survival_safe, keyinput);
        switch (qa_event) {
        case QaRoomEvent::Entered:
            enter_qa_video();
            break;
        case QaRoomEvent::None:
            break;
        case QaRoomEvent::ExitedByTrigger:
        case QaRoomEvent::ExitedBecauseNormalControlEnded:
            leave_qa_video();
            break;
        }
        // Entered already publishes an immutable frame. All following active
        // frames go through this one deduped host-locomotion/publish path,
        // including when the Minish player is blocked by native geometry and
        // never calls LinearMoveDirectionOLD.
        if (qa_event != QaRoomEvent::Entered && g_qa_room.active() &&
            g_qa_video_active && g_foreign_survival_safe)
            tick_foreign_locomotion_once(keyinput);
    }
    return 0;  // Observe only: always continue through UpdateEntities.
}

int replace_player_linear_move(std::uint32_t, int, ArmCpuState* cpu) {
    if (g_asset_available && !apply_pending_native_restore()) {
        leave_qa_video();
        return 0;
    }
    if (!cpu || !can_replace_linear_move(cpu->R[0], g_qa_room.active() &&
                                             g_qa_video_active &&
                                             g_overworld_session.loaded(),
                                         g_foreign_survival_safe))
        return 0;
    // Host locomotion is KEYINPUT/frame driven above. This narrow callback
    // only prevents a native Player move from changing Minish coordinates
    // while a foreign session is active; it neither samples the direction nor
    // mutates foreign state. A handled call changes exactly the guest PC.
    cpu->R[15] = linear_move_handled_return_pc(cpu->R[14]);
    return 1;
}

}  // namespace

void reset_smith_yard_readiness_plugin() {
    g_asset_available = false;
    g_tracker.reset();
    g_portal_tracker.reset();
    g_qa_room.reset();
    // A reset recreates the bus/PPU before the mod callback is invoked, so
    // restoring old VRAM here would be invalid.  The next session begins
    // inactive; normal exit is handled by leave_qa_video().
    g_qa_video_active = false;
    g_foreign_survival_safe = false;
    g_overworld_session = {};
    g_level1_adapter.reset();
    g_obj_focus.reset();
    g_cave_a_edge.reset();
    g_combat_a_edge.reset();
    g_foreign_combat.reset();
    g_combat_frame_seen = false;
    g_locomotion_frame_seen = false;
    g_presentation_restore_pending = false;
    g_seen_native_restore_generation = foreign_world_native_state().restore_generation();
    foreign_world_native_state().set_zelda1_presentation_active(false);
    gba_mod_clear_foreign_obj_focus();
    gba_mod_clear_foreign_background();
    (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 0);
}

void activate_smith_yard_readiness_plugin() {
    // Provider registration is deliberately activation-time, never a module
    // constructor side effect. The engine freezes this catalog at its first
    // snapshot, so enabling Zelda after a snapshot requires a restart.
    std::string error;
    if (!register_foreign_world_native_state_provider(&error)) {
        (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 0);
        return;
    }
    // The mod runtime only publishes this path after target and exact asset
    // validation. A null path must leave the hook disabled.
    const char* asset_path = gba_mod_required_asset_path(
        kZelda1ForeignWorldPackageId, kZelda1ForeignWorldAssetId);
    g_asset_available = false;
    g_overworld_session = {};
    g_level1_adapter.reset();
    if (!asset_path) {
        (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 0);
        return;
    }
    if (!g_overworld_session.load_hash_validated_ines(
            asset_path, Zelda1OverworldSession::kInitialRoom, &error)) {
        // Runtime's package SHA-1 validation is the authority for ownership;
        // the loader additionally validates the canonical PRG0 iNES layout
        // and decoder inputs. Any mismatch fails closed before hooks run.
        (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 0);
        return;
    }
    auto& native = foreign_world_native_state();
    const auto& saved_session = native.zelda1_overworld_session_blob();
    if (!saved_session.empty()) {
        if (!g_overworld_session.restore(saved_session, &error)) {
            g_overworld_session = {};
            (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 0);
            return;
        }
    } else if (!sync_live_overworld_session()) {
        g_overworld_session = {};
        (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 0);
        return;
    }
    g_seen_native_restore_generation = native.restore_generation();
    g_asset_available = true;
    (void)gba_mod_set_function_hook_enabled(kZelda1ForeignWorldPluginId, 1);
}

const SmithYardReadinessState& smith_yard_readiness_state() {
    return g_tracker.state();
}

}  // namespace minish::foreign_world::zelda1

GBA_MOD_CONSTRUCTOR(minish_register_zelda1_foreign_world_plugin) {
    using namespace minish::foreign_world::zelda1;
    (void)gba_mod_register_reset_callback(reset_smith_yard_readiness_plugin);
    (void)gba_mod_register_function_entry_plugin(
        kZelda1ForeignWorldPluginId, 0x0805E5C0u, 1, observe_update_entities);
    (void)gba_mod_register_function_entry_plugin(
        kZelda1ForeignWorldPluginId, 0x03005F40u, 0, observe_update_entities);
    (void)gba_mod_register_function_entry_plugin(
        kZelda1ForeignWorldPluginId, kLinearMoveDirectionOld, 1,
        replace_player_linear_move);
    (void)gba_mod_register_activation_plugin(
        kZelda1ForeignWorldPluginId, activate_smith_yard_readiness_plugin);
}
