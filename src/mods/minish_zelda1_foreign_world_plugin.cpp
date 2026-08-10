#include "foreign_worlds/zelda1/smith_yard_readiness_plugin.h"
#include "foreign_worlds/zelda1/smith_yard_qa_room.h"
#include "foreign_worlds/foreign_world_native_state.h"
#include "foreign_worlds/zelda1/minish_foreign_combat_adapter.h"
#include "foreign_worlds/zelda1/minish_foreign_portal.h"
#include "foreign_worlds/zelda1/minish_host_sword_swing.h"
#include "foreign_worlds/zelda1/minish_linear_move_adapter.h"
#include "foreign_worlds/zelda1/minish_portal_readiness.h"
#include "foreign_worlds/zelda1/zelda1_level1_live_adapter.h"
#include "foreign_worlds/zelda1/zelda1_overworld_session.h"

#include <string>
#include <array>

#include "foreign_screen_overlay.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace minish::foreign_world::zelda1 {
namespace {

// Source: pinned zeldaret/tmc linker.ld and include/room.h.
constexpr std::uint32_t kRoomControls = 0x03000BF0;
constexpr std::uint32_t kRoomControlsArea = kRoomControls + 0x04;
constexpr std::uint32_t kRoomControlsRoom = kRoomControls + 0x05;
constexpr std::uint32_t kRoomControlsOriginX = kRoomControls + 0x06;
constexpr std::uint32_t kRoomControlsOriginY = kRoomControls + 0x08;
constexpr std::uint32_t kRoomTransition = 0x030010A0;
constexpr std::uint32_t kRoomFrameCount = kRoomTransition;
constexpr std::uint32_t kTransitioningOut = kRoomTransition + 0x08;
constexpr std::uint32_t kUpdateEntities = 0x0805E5C0;
// Source: zeldaret/tmc src/menu/figurineMenu.c:InitPauseMenu.  player.c calls
// this only after CheckInitPauseMenu's Start/control/message eligibility gate,
// so this is narrower than sampling raw Start input from a gameplay hook.
constexpr std::uint32_t kInitPauseMenu = 0x080A4D88;
constexpr std::uint32_t kDoExitTransition = 0x08080840;
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
constexpr std::uint32_t kPlayerDirection = 0x03001175; // + Entity.direction
constexpr std::uint32_t kPlayerSpeed = 0x03001184; // + Entity.speed (Q8.8)
constexpr std::uint32_t kGameMainSubstate = 0x03001004;
constexpr std::uint32_t kPlayerKind = 0x03001168, kPlayerFlags = 0x03001170;
constexpr std::uint32_t kPlayerDraw = 0x03001178, kPlayerKilled = 0x03003FBC;
constexpr std::uint32_t kPlayerMoveLocks = 0x03003FB0, kPausePlayer = 0x03003F8A;
constexpr std::uint32_t kPlayerMacro = 0x0300401C, kMessageState = 0x02000050;
constexpr std::uint32_t kPriorityTimer = 0x03003DC8;
// Source: pinned linker map / HUD layout mirrored by minish_extended_view.cpp.
// Each active gHUD element records its OAM allocation (tile base/count) and
// authored screen anchor.  This lets foreign presentation retain the complete
// live HUD without treating a priority class as UI.
constexpr std::uint32_t kHud = 0x0200AF00;
constexpr std::uint32_t kHudElements = kHud + 0x34;
constexpr std::uint32_t kOam = 0x07000000;
constexpr std::uint32_t kHudElementSize = 0x20;
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
constexpr std::uint32_t kActiveItemStride = 0x1c;
constexpr std::uint32_t kActiveItemBehaviorId = kActiveItem0 + 0x01;
constexpr std::uint32_t kActiveItemPriority = kActiveItem0 + 0x09;
constexpr std::uint32_t kActiveItemPlayerAnimationState = kActiveItem0 + 0x0A;
constexpr std::uint32_t kActiveItemAnimationPriority = kActiveItem0 + 0x0F;
constexpr std::uint32_t kPlayerAttackStatus = 0x03003F84; // PlayerState + 0x04
// Source: zeldaret/tmc include/player.h PlayerState::animation.  This is
// transient live player presentation state, not gSave equipment or inventory.
constexpr std::uint32_t kPlayerAnimation = 0x03003F88; // PlayerState + 0x08
constexpr std::uint32_t kPlayerSwordState = 0x03003F9B;   // PlayerState + 0x1B
// Source: pinned `src/interrupts.c:PlayerUpdate`: this resolver runs after
// DoPlayerAction and immediately before DrawEntity.  It is the only guest
// seam used to request a Zelda-origin sword's *body* animation; the guest
// resolver still runs normally and no player-item entity is created.
constexpr std::uint32_t kResolvePlayerSprite = 0x08078FB0;
constexpr std::uint32_t kDrawEntity = 0x0800404C;
constexpr std::uint16_t kMinishSwordAnimation = 0x0108; // ANIM_SWORD

SmithYardReadinessTracker g_tracker;
MinishPortalReadinessTracker g_portal_tracker;
SmithYardQaRoom g_qa_room;
MinishForeignPortalController g_native_portal;
MinishForeignPortalRenderer g_native_portal_renderer;
std::array<GbaForeignScreenOverlay, 2> g_native_portal_overlays{};
unsigned g_next_native_portal_overlay = 0;
bool g_native_portal_published_frame_seen = false;
std::uint32_t g_last_native_portal_published_frame = 0;
bool g_asset_available = false;
bool g_qa_video_active = false;
bool g_foreign_menu_suspended = false;
bool g_foreign_survival_safe = false;
Zelda1OverworldSession g_overworld_session;
Zelda1Level1LiveAdapter g_level1_adapter;
ForeignObjFocusDoubleBuffer g_obj_focus;
ActiveLowButtonEdge g_cave_a_edge;
ActiveLowButtonEdge g_combat_a_edge;
MinishForeignCombatAdapter g_foreign_combat;
HostSwordSwingPresentation g_host_sword_swing;
// The request is a strictly pre-resolver/post-resolver pair.  The source
// resolver latches ANIM_SWORD into the player sprite, then its immediate
// PlayerUpdate DrawEntity call restores the original PlayerState field before
// draw.  This keeps the live source state normal outside that one resolver.
bool g_player_sword_animation_restore_pending = false;
std::uint16_t g_player_sword_animation_before = 0;
bool g_combat_frame_seen = false;
std::uint32_t g_last_combat_frame = 0;
bool g_locomotion_frame_seen = false;
std::uint32_t g_last_locomotion_frame = 0;
MinishRollMotionAccumulator g_roll_motion;
// The ROM and relocated-IWRAM UpdateEntities hooks can both observe a single
// Minish game frame. An *active* Zelda lease therefore commits control,
// locomotion, and publication once per frame. Entry is different: each phase
// is independently source-gated, but at most the first fully ready phase may
// consume one QA hold heartbeat for that frame. This lets a later stable phase
// enter after an earlier transient phase without treating two callbacks as two
// source frames.
bool g_lifecycle_frame_seen = false;
std::uint32_t g_last_lifecycle_frame = 0;
bool g_presentation_restore_pending = false;
// Recoverable active-session failures must retain the last immutable Zelda
// PPU publication rather than revealing native Minish underneath. Frozen
// sessions accept only explicit exit/reset/load boundaries.
bool g_foreign_session_frozen = false;
// The runtime has independent background and OBJ-focus publication entry
// points.  Keep the last *complete* pair so a rejection of the second call
// can be rolled back without leaving a half-new foreign frame visible.
const std::uint16_t* g_last_published_background = nullptr;
const GbaForeignObjFocusTransform* g_last_published_focus = nullptr;
std::uint64_t g_seen_native_restore_generation = 0;

std::int16_t read_guest_s16(std::uint32_t address) {
    return static_cast<std::int16_t>(bus_read_u16(address));
}

void clear_native_portal_overlay() {
    gba_mod_clear_foreign_screen_overlay();
}

void restore_player_sword_animation_if_pending() {
    if (!g_player_sword_animation_restore_pending) return;
    bus_write_u16(kPlayerAnimation, g_player_sword_animation_before);
    g_player_sword_animation_restore_pending = false;
}

bool publish_native_portal_overlay(std::uint32_t source_frame,
                                   std::int16_t room_origin_x,
                                   std::int16_t room_origin_y) {
    // A native-world portal must never survive into a foreign frame or menu.
    if (!g_asset_available || g_qa_room.active() || g_foreign_menu_suspended) {
        clear_native_portal_overlay();
        return false;
    }
    const auto* frame = g_native_portal_renderer.next(
        read_guest_s16(kRoomScrollX), read_guest_s16(kRoomScrollY),
        room_origin_x, room_origin_y, source_frame);
    GbaForeignScreenOverlay& overlay =
        g_native_portal_overlays[g_next_native_portal_overlay];
    overlay = {
        GBA_FOREIGN_SCREEN_OVERLAY_ABI_VERSION,
        frame->pixels.data(),
        frame->alpha_q4.data(),
        frame->screen.x,
        frame->screen.y,
        kMinishPortalWidth,
        kMinishPortalHeight,
        kMinishPortalWidth,
        0,
        0,
    };
    if (gba_mod_publish_foreign_screen_overlay(kZelda1ForeignWorldPluginId,
                                               &overlay) == 0) {
        clear_native_portal_overlay();
        return false;
    }
    g_next_native_portal_overlay ^= 1u;
    return true;
}

struct HudOamMask { std::uint64_t lo = 0, hi = 0; };

int hud_distance(int a, int b) { return a >= b ? a - b : b - a; }

HudOamMask read_live_hud_oam_mask() {
    HudOamMask result;
    for (unsigned oam_index = 0; oam_index < 128; ++oam_index) {
        const std::uint32_t entry = kOam + oam_index * 8u;
        const std::uint16_t attr0 = bus_read_u16(entry);
        if ((attr0 & 0x0200u) != 0) continue;
        const std::uint16_t attr1 = bus_read_u16(entry + 2u);
        const std::uint16_t attr2 = bus_read_u16(entry + 4u);
        const unsigned tile = attr2 & 0x3ffu;
        const int raw_x = attr1 & 0x1ffu;
        int raw_y = attr0 & 0xffu;
        if (raw_y >= 160) raw_y -= 256;
        bool hud = false;
        for (unsigned i = 0; i < 24 && !hud; ++i) {
            const std::uint32_t element = kHudElements + i * kHudElementSize;
            if ((bus_read_u8(element) & 3u) != 3u) continue;
            const unsigned type = bus_read_u8(element + 1u);
            if (type > 10u) continue;
            const unsigned base = bus_read_u16(element + 0x1au) & 0x3ffu;
            // Button elements report zero despite DrawDirect OAM; the pinned
            // HUD implementation uses their bounded shared 0x20-tile block.
            const unsigned span = bus_read_u8(element + 0x19u);
            const unsigned bounded_span = span != 0 ? span : 0x20u;
            if (((tile - base) & 0x3ffu) >= bounded_span) continue;
            const int x = read_guest_s16(element + 0x0cu);
            const int y = read_guest_s16(element + 0x0eu);
            hud = hud_distance(raw_x, x) <= 32 && hud_distance(raw_y, y) <= 32;
        }
        if (!hud) continue;
        if (oam_index < 64) result.lo |= UINT64_C(1) << oam_index;
        else result.hi |= UINT64_C(1) << (oam_index - 64u);
    }
    return result;
}

bool publish_current_foreign_video() {
    if (!g_overworld_session.loaded()) return false;
    const auto level1 = g_level1_adapter.presentation();
    const bool level1_active = level1.active && level1.framebuffer;
    const auto* const source_background = level1_active ? level1.framebuffer->data()
                                                        : g_overworld_session.framebuffer().data();
    const auto overworld_feet = g_overworld_session.presentation_feet_position();
    const auto cave_position = g_overworld_session.cave_presentation_position();
    const std::int16_t destination_feet_x =
        level1_active ? static_cast<std::int16_t>(level1.player.x +
                                                   kZelda1LinkFeetOffsetX) :
        (cave_position ? cave_position->x : overworld_feet.x);
    // Level1PlayerPresentation is the source 16x16 sprite anchor used by
    // its host controller. The ABI destination is explicitly Link's feet,
    // so use the source sprite's bottom center (+$08,+$10); Z_07's ObjY+$0b
    // collision sample is five pixels above that visual anchor.
    const std::int16_t destination_feet_y =
        level1_active ? static_cast<std::int16_t>(level1.player.y +
                                                   kZelda1LinkFeetOffsetY) :
        (cave_position ? cave_position->y : overworld_feet.y);
    const auto* const background = g_host_sword_swing.prepare(
        source_background, destination_feet_x, destination_feet_y);
    const HudOamMask hud_mask = read_live_hud_oam_mask();
    const auto* focus = g_obj_focus.prepare(
        static_cast<std::int16_t>(read_guest_s16(kPlayerFeetX) -
                                  read_guest_s16(kRoomScrollX)),
        static_cast<std::int16_t>(read_guest_s16(kPlayerFeetY) -
                                  read_guest_s16(kRoomScrollY)),
        destination_feet_x, destination_feet_y,
        bus_read_u16(kPlayerSpriteVramOffset), kPlayerShadowObjTile,
        (bus_read_u8(kPlayerDraw) & 0x30u) != 0 ? 1u : 0u,
        hud_mask.lo, hud_mask.hi);
    // Publish one logical foreign frame transactionally. The background is
    // first because a rejected first call cannot have changed runtime state;
    // if the independently validated focus call rejects, immediately restore
    // the previous complete pair. The focus runtime normally leaves a failed
    // publication untouched; re-publishing its cached value also defends
    // against a rejecting implementation that consumed the candidate first.
    // We intentionally never clear on a failure: an already-active session
    // freezes on its last complete Zelda publication.
    if (gba_mod_publish_foreign_background(kZelda1ForeignWorldPluginId, background) == 0) {
        g_host_sword_swing.discard();
        g_obj_focus.discard();
        return false;
    }
    if (gba_mod_publish_foreign_obj_focus(kZelda1ForeignWorldPluginId, focus) == 0) {
        if (g_last_published_background)
            (void)gba_mod_publish_foreign_background(kZelda1ForeignWorldPluginId,
                                                      g_last_published_background);
        if (g_last_published_focus)
            (void)gba_mod_publish_foreign_obj_focus(kZelda1ForeignWorldPluginId,
                                                     g_last_published_focus);
        g_host_sword_swing.discard();
        g_obj_focus.discard();
        return false;
    }
    g_host_sword_swing.commit();
    g_obj_focus.commit();
    g_last_published_background = background;
    g_last_published_focus = focus;
    return true;
}

void leave_qa_video(bool reset_trigger = true);

void enter_qa_video() {
    // The PPU reads immutable session pixels and a data-only Link focus
    // descriptor during its normal composition. No guest VRAM, OAM, entity,
    // input, or coordinate bytes are touched.
    // The native field portal is a separate compositor layer. Clear it before
    // committing a Zelda background; the PPU also suppresses it defensively
    // while a foreign background exists.
    clear_native_portal_overlay();
    g_obj_focus.reset();
    g_cave_a_edge.reset();
    g_combat_a_edge.reset();
    g_locomotion_frame_seen = false;
    g_foreign_session_frozen = false;
    g_last_published_background = nullptr;
    g_last_published_focus = nullptr;
    g_qa_video_active = publish_current_foreign_video();
    g_foreign_menu_suspended = false;
    foreign_world_native_state().set_zelda1_presentation_active(g_qa_video_active);
    // No foreign PPU pair was committed on this entry attempt, so this is
    // activation-before-entry failure handling rather than a fallback from an
    // active Zelda session. Active publication failures freeze instead.
    if (!g_qa_video_active) leave_qa_video();
}

void leave_qa_video(bool reset_trigger) {
    // Normally DrawEntity consumed this lease within the same PlayerUpdate.
    // Keep an explicit lifecycle recovery for a skipped source draw/menu/exit
    // boundary so ANIM_SWORD can never remain in PlayerState after a host
    // swing is gone. Reset is special: its new guest bus is already blank.
    restore_player_sword_animation_if_pending();
    gba_mod_clear_foreign_obj_focus();
    gba_mod_clear_foreign_background();
    g_qa_video_active = false;
    g_foreign_menu_suspended = false;
    g_foreign_survival_safe = false;
    g_cave_a_edge.reset();
    g_combat_a_edge.reset();
    g_foreign_combat.reset();
    g_host_sword_swing.reset();
    g_level1_adapter.reset();
    // A failed publish/restore must not leave the trigger latched active:
    // that would clear Zelda pixels now yet prevent the documented portal
    // from ever entering again. The explicit exit chord is different: its
    // state machine already made the trigger inactive and must retain its
    // latch until the user releases, preventing a held chord from reentering.
    if (reset_trigger) g_qa_room.reset();
    g_native_portal.reset();
    g_combat_frame_seen = false;
    g_locomotion_frame_seen = false;
    g_presentation_restore_pending = false;
    g_foreign_session_frozen = false;
    g_last_published_background = nullptr;
    g_last_published_focus = nullptr;
    foreign_world_native_state().set_zelda1_presentation_active(false);
    // Do not publish immediately: the next fully-ready native update owns the
    // portal redraw, so an exit cannot place an overlay in a stale room/menu.
    clear_native_portal_overlay();
}

void freeze_active_foreign_session() {
    if (!g_qa_room.active()) return;
    g_foreign_session_frozen = true;
    g_foreign_survival_safe = true;
    // Preserve logical activity as well as the existing PPU pointers so a
    // checkpoint cannot reinterpret an internal error as user-returned.
    foreign_world_native_state().set_zelda1_presentation_active(true);
}

// This is an observer at the source-proven pause-menu initialization boundary.
// It owns only foreign presentation publication: MenuFadeIn proceeds normally,
// while the host's own pause subtask owns all menu input, graphics, and actions.
// In particular this does not reset, serialize, or otherwise alter Zelda state.
int observe_init_pause_menu(std::uint32_t, int, ArmCpuState*) {
    // Do not let a paused source frame retain an otherwise paired resolver
    // request if a menu is entered at an unusual instruction boundary.
    restore_player_sword_animation_if_pending();
    // This hook applies to both native and foreign menus. A cached field portal
    // must not sit over the pause menu while GameMain skips UpdateEntities.
    clear_native_portal_overlay();
    // A held A while native menu input owns the game must not become an
    // immediate portal interaction at the first post-menu UpdateEntities.
    g_native_portal.reset();
    if (!g_asset_available || g_foreign_menu_suspended || !g_qa_room.active() ||
        !g_qa_video_active || !g_overworld_session.loaded())
        return 0;
    gba_mod_clear_foreign_obj_focus();
    gba_mod_clear_foreign_background();
    g_qa_video_active = false;
    g_foreign_menu_suspended = true;
    // Presentation activity is persisted with the foreign session.  A native
    // menu is only a temporary compositor suspension, not a world exit, so
    // retain that logical activity for save/load made while the menu is open.
    return 0;
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
    // The provider has already replaced the guest bus with a snapshot.  A
    // pre-resolver lease belongs to the discarded timeline, so writing its
    // saved value here would corrupt the restored player state.  Treat this
    // exactly like reset: drop host bookkeeping without a guest write.
    g_player_sword_animation_restore_pending = false;
    g_player_sword_animation_before = 0;
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
    g_native_portal.reset();
    g_host_sword_swing.reset();
    g_combat_frame_seen = false;
    g_locomotion_frame_seen = false;
    g_qa_room.restore_active(native.zelda1_presentation_active());
    g_qa_video_active = false;
    // Provider restore is an authoritative lifecycle boundary. A snapshot
    // made while the native Start menu temporarily owned the compositor must
    // not retain the ephemeral menu-suspension latch and suppress every
    // later inactive portal observation.
    g_foreign_menu_suspended = false;
    g_presentation_restore_pending = native.zelda1_presentation_active();
    g_foreign_session_frozen = false;
    if (!g_presentation_restore_pending) {
        g_obj_focus.reset();
        g_last_published_background = nullptr;
        g_last_published_focus = nullptr;
        gba_mod_clear_foreign_obj_focus();
        gba_mod_clear_foreign_background();
        clear_native_portal_overlay();
    }
    // Do not consume a restore generation before every validation and session
    // mutation has succeeded. A failed restore remains visibly frozen in its
    // existing foreign state and can be retried; it never becomes a covert
    // teleport back to native Minish presentation.
    g_seen_native_restore_generation = generation;
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
    // Zelda-origin swords intentionally never manufacture a Minish ItemSword,
    // so native A produces no guest animation when Link has no native sword.
    // Give the selected exact-origin host sword a short compositor-only swing
    // instead. It has no bus write and the existing adapters retain all hit
    // policy. A source ItemSword remains visually owned by Minish.
    g_host_sword_swing.advance();
    if (selected_sword_edge && !is_active_minish_sword(sword) &&
        resolve_foreign_sword(foreign_world_native_state().inventory(), LoadoutId::A)) {
        auto facing = foreign_sword_facing(bus_read_u8(kPlayerAnimationState));
        if (!facing)
            facing = foreign_sword_facing_from_direction(bus_read_u8(kPlayerDirection));
        if (facing) g_host_sword_swing.begin(*facing);
    }
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
            freeze_active_foreign_session();
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
        selected_sword_edge, bus_read_u8(kPlayerAnimationState),
        bus_read_u8(kPlayerDirection)};
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
    // Z_01:UpdatePersonState_Textbox advances independently of input: its
    // timer emits the verified PersonText glyph stream then UnhaltLink runs
    // from the final `$C0` marker.  Afterwards
    // UpdateCavePersonState_TalkOrShopOrDoorCharge compares Link's exact X
    // with CaveWareXs and abs(ObjY-$98) < $06, then takes the giveaway
    // immediately.  There is no A-button gate on this source pickup.
    // Cave text and type-$40 standing fires are both source per-frame object
    // updates. The session stages their control state and framebuffer as one
    // failure-atomic cave frame.
    (void)g_overworld_session.tick_cave_frame();
    // Retain the edge sampler solely so a held/released A remains harmless
    // across the cave boundary; do not make source touch acquisition depend
    // on it.
    (void)g_cave_a_edge.observe(keyinput, kGbaKeyA);
    if (g_overworld_session.cave_dialogue_acknowledged()) {
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
            freeze_active_foreign_session();
            return false;
        }
        if (result != Level1LiveResult::kMoved) return false;
        std::string error;
        const auto collected = g_level1_adapter.collect_room_item(&inventory, &error);
        if (collected == Level1LiveResult::kInvalid ||
            collected == Level1LiveResult::kInventoryRejected) {
            freeze_active_foreign_session();
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

// PlayerRollUpdate can emit up to 0x300 Q8.8 (three source pixels) per
// source frame.  Re-play that bounded output as the same one-pixel Zelda
// probes used by walking: each emitted pixel observes collision and portals
// before the next one, so a roll cannot skip a wall, cave, or Level entrance.
// The source guest's current Entity::direction owns the diagonal; preserving
// the established X-then-Y host ordering keeps a diagonal's first blocked
// axis from incorrectly sliding it through a corner.
bool move_foreign_roll_delta(const LinearMoveDelta& delta) {
    constexpr std::int16_t kMaximumRollPixelsPerAxis = 3;
    if (delta.x < -kMaximumRollPixelsPerAxis || delta.x > kMaximumRollPixelsPerAxis ||
        delta.y < -kMaximumRollPixelsPerAxis || delta.y > kMaximumRollPixelsPerAxis)
        return false;
    const auto move_axis = [](std::int16_t distance, bool horizontal) {
        const std::int16_t pixel = distance < 0 ? -1 : 1;
        const unsigned count = static_cast<unsigned>(distance < 0 ? -distance : distance);
        for (unsigned i = 0; i < count; ++i)
            if (!move_foreign_one_pixel(horizontal ? pixel : 0,
                                        horizontal ? 0 : pixel))
                return false;
        return true;
    };
    if (delta.x != 0 && !move_axis(delta.x, true)) return false;
    if (g_overworld_session.in_cave() || g_level1_adapter.active()) return false;
    return delta.y == 0 || move_axis(delta.y, false);
}

void tick_foreign_locomotion_once(std::uint16_t keyinput) {
    if (g_foreign_session_frozen) return;
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
    if (g_foreign_session_frozen) return;
    if (!g_qa_room.active() || !g_qa_video_active || !g_foreign_survival_safe)
        return;
    const MinishRollMotionSample roll_sample{
        bus_read_u8(kPlayerAction), bus_read_u8(kPlayerDirection),
        bus_read_u16(kPlayerSpeed), bus_read_u32(kPlayerMoveLocks),
    };
    LinearMoveDelta roll_delta{};
    const auto roll_result = g_roll_motion.step(roll_sample, &roll_delta);
    if (roll_result == MinishRollMotionAccumulator::Result::kMoved) {
        (void)move_foreign_roll_delta(roll_delta);
    } else if (roll_result == MinishRollMotionAccumulator::Result::kNotRolling) {
        // Walking remains controlled by ordinary D-pad intent.  Roll output
        // deliberately has no KEYINPUT fallback: PlayerRollUpdate's speed=0
        // frame must remain stationary even if a direction is held.
        if (dx != 0 && move_foreign_one_pixel(dx, 0) &&
            !g_overworld_session.in_cave() && !g_level1_adapter.active())
            (void)move_foreign_one_pixel(0, dy);
        else if (dx == 0)
            (void)move_foreign_one_pixel(0, dy);
    }
    if (g_foreign_session_frozen) return;
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
        freeze_active_foreign_session();
        return;
    }
    if (g_qa_room.active() && g_qa_video_active && !publish_current_foreign_video()) {
        freeze_active_foreign_session();
    }
}

int observe_update_entities(std::uint32_t, int, ArmCpuState*) {
    // Read only documented global fields through the runtime's guest bus API.
    // Do not allocate entities, call guest code, or modify ArmCpuState.
    if (g_asset_available) {
        const auto& native = foreign_world_native_state();
        const bool restore_arrived =
            native.restore_generation() != g_seen_native_restore_generation;
        // A provider restore has already replaced the guest bus.  Let its
        // handler discard any stale pre-resolver lease before considering the
        // ordinary missed-post-draw repair for this timeline.
        if (!restore_arrived) {
            // A successful PlayerUpdate always reaches DrawEntity immediately
            // after sub_08078FB0. If a nonstandard lifecycle skipped that
            // draw, restore before the next source update can serialize or
            // reuse it.
            restore_player_sword_animation_if_pending();
        }
        if (restore_arrived && !apply_pending_native_restore()) {
            freeze_active_foreign_session();
            // A corrupt/restoration-incompatible provider record may not
            // silently expose Minish, and it may not confiscate the user's
            // sole explicit exit. Continue only far enough for the
            // release-latched QA chord below; frozen locomotion prevents any
            // world/session mutation while this generation remains invalid.
        }
        // `InitPauseMenu` is reached only after the source Start eligibility
        // check and the pause subtask contains no UpdateEntities calls.  Its
        // first subsequent observation is therefore the native menu-close
        // boundary.  Re-publish the same host-owned Zelda frame without
        // consulting transient player/menu state or rebuilding the session.
        if (g_foreign_menu_suspended) {
            if (g_qa_room.active() && g_overworld_session.loaded() &&
                publish_current_foreign_video()) {
                g_qa_video_active = true;
                g_foreign_menu_suspended = false;
                // A successful re-publication is the menu-close lifecycle
                // boundary: resume the same logical Zelda lease rather than
                // retaining an error freeze from a rejected retry.
                g_foreign_session_frozen = false;
                foreign_world_native_state().set_zelda1_presentation_active(true);
            } else if (g_qa_room.active()) {
                // InitPauseMenu deliberately cleared the foreign pair for the
                // native menu.  If its first menu-close publication rejects,
                // do not convert that temporary compositor gap into native
                // movement or a silent world return.  Retain the logical
                // foreign shield, freeze mutations, and leave the suspension
                // latch set so a later source frame can retry publication.
                g_qa_video_active = true;
                freeze_active_foreign_session();
                // A freeze never confiscates the documented explicit exit.
                // Feed only the edge/latch machine here (never portal
                // readiness or locomotion): a release clears a prior entry
                // latch, and a subsequent held chord can intentionally leave
                // even if the compositor refuses every retry.
                if (g_qa_room.update(true, bus_read_u16(0x04000130)) ==
                    QaRoomEvent::ExitedByTrigger)
                    leave_qa_video(false);
            }
            return 0;
        }
        const std::uint8_t area = bus_read_u8(kRoomControlsArea);
        const std::uint8_t room = bus_read_u8(kRoomControlsRoom);
        const bool transitioning_out = bus_read_u8(kTransitioningOut) != 0;
        const bool player_entity_alive =
            bus_read_u8(kPlayerKind) == 1 &&
            (bus_read_u8(kPlayerFlags) & 0x10) == 0 &&
            bus_read_u8(kPlayerKilled) == 0;
        const std::uint32_t frame = bus_read_u32(kRoomFrameCount);
        const bool active_lease = g_qa_room.active();
        if (active_lease && !restore_arrived && g_lifecycle_frame_seen &&
            g_last_lifecycle_frame == frame)
            return 0;
        if (active_lease) {
            g_lifecycle_frame_seen = true;
            g_last_lifecycle_frame = frame;
        }
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
            player_entity_alive,
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
        // Entry requires the whole source-mapped normal-control gate and an
        // outside-to-inside contact crossing. The controller observes both
        // relocated callback phases; a late phase that only lacks normal
        // control must not erase an arm from its stable peer.
        // Once active, transient native state can never dismiss the session.
        // The active-world lease survives every native action/lifecycle
        // sample. This foreign world has no implicit safety exit: only the
        // explicit, release-latched QA portal chord (or reset/load lifecycle)
        // may leave it. Native room/transition/death data remain entry facts
        // and diagnostics, never an automatic teleport-back trigger.
        const bool survival_safe = true;
        g_foreign_survival_safe = survival_safe;
        const std::uint16_t keyinput = bus_read_u16(0x04000130);
        // A provider restore has rebuilt Z1OS before this point. Rebind its
        // lifecycle-owned presentation only after the normal read-only
        // survival gate is available; inactive snapshots explicitly clear.
        if (g_presentation_restore_pending) {
            g_presentation_restore_pending = false;
            if (survival_safe) {
                if (publish_current_foreign_video()) {
                    g_qa_video_active = true;
                } else {
                    // Preserve the active lease even if a restored frame
                    // cannot publish. The cached PPU pair (if any) remains
                    // visible, frozen sessions reject host locomotion, and
                    // the linear-move hook must continue shielding the
                    // native player instead of permitting an unseen return.
                    g_qa_video_active = true;
                    freeze_active_foreign_session();
                }
            } else {
                freeze_active_foreign_session();
            }
        }
        QaRoomEvent qa_event = QaRoomEvent::None;
        if (active_lease) {
            clear_native_portal_overlay();
            qa_event = g_qa_room.update(survival_safe, keyinput);
        } else {
            const bool entry_ready = g_portal_tracker.state().ready();
            const bool portal_hard_safe = portal_input.verified_zelda1_asset &&
                portal_input.area == 3 && portal_input.room == 1 &&
                portal_input.game_main_update && !portal_input.room_transitioning_out &&
                portal_input.player_entity_alive && portal_input.player_entity_drawn &&
                !portal_input.script_or_cutscene_locked;
            const auto portal_event = g_native_portal.observe({
                entry_ready,
                portal_hard_safe,
                frame,
                read_guest_s16(kPlayerFeetX),
                read_guest_s16(kPlayerFeetY),
                read_guest_s16(kRoomControlsOriginX),
                read_guest_s16(kRoomControlsOriginY),
            });
            if (entry_ready) {
                if (publish_native_portal_overlay(
                        frame, read_guest_s16(kRoomControlsOriginX),
                        read_guest_s16(kRoomControlsOriginY))) {
                    g_native_portal_published_frame_seen = true;
                    g_last_native_portal_published_frame = frame;
                }
            } else if (!g_native_portal_published_frame_seen ||
                       g_last_native_portal_published_frame != frame) {
                clear_native_portal_overlay();
            }
            if (portal_event == MinishPortalEvent::Entered) {
                clear_native_portal_overlay();
                g_qa_room.enter();
                qa_event = QaRoomEvent::Entered;
            }
        }
        switch (qa_event) {
        case QaRoomEvent::Entered:
            enter_qa_video();
            // An entry publishes its immutable first Zelda frame and must not
            // let the other relocated hook immediately advance it in the
            // same source frame.
            g_lifecycle_frame_seen = true;
            g_last_lifecycle_frame = frame;
            break;
        case QaRoomEvent::None:
            break;
        case QaRoomEvent::ExitedByTrigger:
            leave_qa_video(false);
            break;
        case QaRoomEvent::ExitedBecauseNormalControlEnded:
            freeze_active_foreign_session();
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

// A room transition begins after UpdateEntities in the native source order.
// Remove the native-only compositor layer at that precise boundary: it has no
// guest side effects and the next eligible room observation may republish it.
int observe_exit_transition(std::uint32_t, int, ArmCpuState*) {
    restore_player_sword_animation_if_pending();
    g_native_portal.reset();
    clear_native_portal_overlay();
    return 0;
}

bool should_request_minish_sword_body_animation(std::uint32_t player_address) {
    // The body pose is valid only while the same bounded host swing that
    // owns the compositor blade is still active.  Never synthesize a TMC
    // ItemSword: a real source sword keeps its own animation/entity path.
    if (player_address != kMinishPlayerEntityAddress || !g_qa_room.active() ||
        !g_qa_video_active || g_foreign_menu_suspended ||
        g_foreign_session_frozen || !g_foreign_survival_safe ||
        !g_overworld_session.loaded() || !g_host_sword_swing.active())
        return false;
    // Any live native item animation owns this source resolver.  The main
    // lane check below is still used for the sword-specific collision bridge,
    // but this broader four-lane gate prevents a Zelda visual request from
    // overriding a native bow, boots, lantern, or other item animation.
    for (std::uint32_t lane = 0; lane != 4; ++lane) {
        const std::uint32_t item = kActiveItem0 + lane * kActiveItemStride;
        if (bus_read_u8(item + (kActiveItemBehaviorId - kActiveItem0)) != 0 &&
            (bus_read_u8(item + (kActiveItemPriority - kActiveItem0)) != 0 ||
             bus_read_u8(item + (kActiveItemAnimationPriority - kActiveItem0)) != 0))
            return false;
    }
    const MinishSwordSample source_sword{
        bus_read_u8(kActiveItemBehaviorId),
        bus_read_u8(kActiveItemPriority),
        bus_read_u8(kActiveItemPlayerAnimationState),
        bus_read_u8(kPlayerSwordState),
        bus_read_u8(kPlayerAttackStatus),
    };
    if (is_active_minish_sword(source_sword)) return false;
    const auto selected = resolve_foreign_sword(
        foreign_world_native_state().inventory(), LoadoutId::A);
    // Same-number Native and Zelda records deliberately remain distinct.  A
    // host blade caused by an arbitrary compatible sword must not modify this
    // Minish resolver; this narrow presentation bridge is Zelda1/01 only.
    return selected && selected->acquired_id ==
        CrossWorldItemId{WorldId::Zelda1, 0x01};
}

int observe_player_sprite_resolve(std::uint32_t, int, ArmCpuState* cpu) {
    // `PlayerUpdate` supplies R0=&gPlayerEntity.  This is intentionally a
    // declining hook: apart from one u16 ephemeral animation request, the
    // original Minish resolver and DrawEntity execute unchanged and the CPU
    // context is byte-identical.  It makes Link use the genuine ANIM_SWORD
    // body frames while the existing compositor supplies the NES-styled blade.
    if (cpu && should_request_minish_sword_body_animation(cpu->R[0])) {
        // A previous missed DrawEntity is repaired before taking a new lease;
        // the hook never stacks or loses the exact native field it replaces.
        restore_player_sword_animation_if_pending();
        g_player_sword_animation_before = bus_read_u16(kPlayerAnimation);
        bus_write_u16(kPlayerAnimation, kMinishSwordAnimation);
        g_player_sword_animation_restore_pending = true;
    }
    return 0;
}

int observe_player_draw(std::uint32_t, int, ArmCpuState* cpu) {
    // Pinned PlayerUpdate calls DrawEntity(&gPlayerEntity) immediately after
    // sub_08078FB0.  A different entity's draw cannot consume the lease.
    if (cpu && cpu->R[0] == kMinishPlayerEntityAddress)
        restore_player_sword_animation_if_pending();
    return 0;
}

int replace_player_linear_move(std::uint32_t, int, ArmCpuState* cpu) {
    if (g_asset_available && !apply_pending_native_restore()) {
        freeze_active_foreign_session();
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
    g_native_portal.reset();
    g_native_portal_renderer.reset();
    g_native_portal_overlays = {};
    g_next_native_portal_overlay = 0;
    g_native_portal_published_frame_seen = false;
    // A reset recreates the bus/PPU before the mod callback is invoked, so
    // restoring old VRAM here would be invalid.  The next session begins
    // inactive; normal exit is handled by leave_qa_video().
    // Clear direct PPU publication before releasing session-owned double
    // buffers. This is required even when the runtime normally pre-clears:
    // the raw background/focus addresses must never dangle during reset.
    gba_mod_clear_foreign_obj_focus();
    gba_mod_clear_foreign_background();
    clear_native_portal_overlay();
    g_qa_video_active = false;
    g_foreign_menu_suspended = false;
    g_foreign_survival_safe = false;
    g_overworld_session = {};
    g_level1_adapter.reset();
    g_obj_focus.reset();
    g_cave_a_edge.reset();
    g_combat_a_edge.reset();
    g_foreign_combat.reset();
    g_host_sword_swing.reset();
    // The reset callback observes a freshly recreated guest bus. Writing the
    // old field here would corrupt reset state; discard only host bookkeeping.
    g_player_sword_animation_restore_pending = false;
    g_player_sword_animation_before = 0;
    g_combat_frame_seen = false;
    g_locomotion_frame_seen = false;
    g_lifecycle_frame_seen = false;
    g_presentation_restore_pending = false;
    g_foreign_session_frozen = false;
    g_last_published_background = nullptr;
    g_last_published_focus = nullptr;
    g_seen_native_restore_generation = foreign_world_native_state().restore_generation();
    foreign_world_native_state().set_zelda1_presentation_active(false);
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
    g_host_sword_swing.reset();
    g_foreign_menu_suspended = false;
    g_native_portal.reset();
    g_native_portal_renderer.reset();
    g_native_portal_overlays = {};
    g_next_native_portal_overlay = 0;
    g_native_portal_published_frame_seen = false;
    clear_native_portal_overlay();
    g_lifecycle_frame_seen = false;
    g_foreign_session_frozen = false;
    g_last_published_background = nullptr;
    g_last_published_focus = nullptr;
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
        kZelda1ForeignWorldPluginId, kInitPauseMenu, 1, observe_init_pause_menu);
    (void)gba_mod_register_function_entry_plugin(
        kZelda1ForeignWorldPluginId, kDoExitTransition, 1,
        observe_exit_transition);
    (void)gba_mod_register_function_entry_plugin(
        kZelda1ForeignWorldPluginId, kLinearMoveDirectionOld, 1,
        replace_player_linear_move);
    (void)gba_mod_register_function_entry_plugin(
        kZelda1ForeignWorldPluginId, kResolvePlayerSprite, 1,
        observe_player_sprite_resolve);
    (void)gba_mod_register_function_entry_plugin(
        kZelda1ForeignWorldPluginId, kDrawEntity, 1, observe_player_draw);
    (void)gba_mod_register_activation_plugin(
        kZelda1ForeignWorldPluginId, activate_smith_yard_readiness_plugin);
}
