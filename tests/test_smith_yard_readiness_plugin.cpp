#include "foreign_worlds/zelda1/smith_yard_readiness_plugin.h"
#include "foreign_worlds/zelda1/smith_yard_qa_room.h"
#include "foreign_worlds/zelda1/minish_linear_move_adapter.h"
#include "foreign_worlds/zelda1/minish_foreign_portal.h"
#include "foreign_worlds/zelda1/zelda1_overworld_session.h"
#include "foreign_worlds/zelda1/zelda1_level1_live_adapter.h"
#include "foreign_worlds/foreign_world_native_state.h"

#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"
#include "foreign_screen_overlay.h"

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <string>

namespace z1 = minish::foreign_world::zelda1;

namespace {

const char* g_asset_path = nullptr;
std::uint8_t g_area = 0;
std::uint8_t g_room = 0;
std::uint8_t g_transitioning_out = 0;
std::uint16_t g_keyinput = 0x03ff;
std::uint8_t g_main_substate = 2, g_player_kind = 1, g_player_flags = 0xA0;
std::uint8_t g_player_action = 1, g_player_draw = 0x13, g_player_killed = 0;
std::uint8_t g_player_direction = 8;
std::uint16_t g_player_speed = 0;
std::uint8_t g_pause_player = 0, g_message_state = 0;
std::uint16_t g_priority_timer = 0;
std::uint32_t g_move_locks = 0x8, g_player_macro = 0;
std::uint32_t g_frame = 0;
std::uint8_t g_active_item_behavior = 0, g_active_item_priority = 0;
std::uint8_t g_active_item_animation = 0, g_player_sword_state = 0;
std::uint8_t g_player_attack_status = 0, g_player_animation_state = 0;
std::uint16_t g_player_sprite_vram_offset = 0x160;
std::int16_t g_player_feet_x = z1::kMinishPortalAnchorLocalX;
std::int16_t g_player_feet_y = z1::kMinishPortalAnchorLocalY;
std::int16_t g_room_origin_x = 0, g_room_origin_y = 0;
unsigned g_bus_write_count = 0;
bool g_background_publish_allowed = true;
bool g_focus_publish_allowed = true;

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

bool same_cpu(const ArmCpuState& left, const ArmCpuState& right) {
    for (unsigned i = 0; i < 16; ++i)
        if (left.R[i] != right.R[i]) return false;
    return left.cpsr == right.cpsr;
}

bool same_cave_nonfire_pixels(const std::uint16_t* first,
                              const std::uint16_t* second) {
    if (!first || !second) return false;
    constexpr unsigned kWidth = z1::kOwRenderWidth;
    for (unsigned y = 0; y < z1::kOwRenderHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x) {
            const bool left_fire = x >= 0x48 - 8 && x < 0x48 + 8 &&
                                   y >= 0x80 - 72 && y < 0x80 - 56;
            const bool right_fire = x >= 0xa8 - 8 && x < 0xa8 + 8 &&
                                    y >= 0x80 - 72 && y < 0x80 - 56;
            if (!left_fire && !right_fire && first[y * kWidth + x] != second[y * kWidth + x])
                return false;
        }
    return true;
}

struct PortalCrossingProbe {
    std::uint8_t x, y;
    std::int16_t dx, dy;
    std::uint16_t key;
};

// Z1OS v7 persists signed raw Zelda ObjX/ObjY at [7..10] plus the source
// ObjGridOffset/ObjDir segment at [17]/[20]. These test-only fixture helpers
// preserve crop-space probes through the renderer's fixed (+8,+72) inverse;
// they do not add a production staging API.
void stage_source_position(
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state,
    std::int16_t x, std::int16_t y) {
    if (!state) return;
    (*state)[7] = static_cast<std::uint8_t>(x & 0xff);
    (*state)[8] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(x) >> 8) & 0xff);
    (*state)[9] = static_cast<std::uint8_t>(y & 0xff);
    (*state)[10] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(y) >> 8) & 0xff);
    (*state)[17] = 0;
    (*state)[20] = 0;
}

void stage_source_segment(
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state,
    std::int16_t x, std::int16_t y, std::int8_t offset,
    z1::OverworldWalkDirection direction) {
    stage_source_position(state, x, y);
    (*state)[17] = static_cast<std::uint8_t>(offset);
    (*state)[20] = static_cast<std::uint8_t>(direction);
}

void stage_crop_position(
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state,
    std::int16_t x, std::int16_t y) {
    stage_source_position(state, static_cast<std::int16_t>(x + 8),
                          static_cast<std::int16_t>(y + 72));
}

bool source_cave_crossing_start(
    const char* path, std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state,
    std::uint16_t* key, unsigned* steps) {
    if (!path || !state || !key || !steps) return false;
    z1::Zelda1OverworldSession session;
    std::string error;
    if (!session.load_hash_validated_ines(path, z1::Zelda1OverworldSession::kInitialRoom, &error))
        return false;
    const auto initial = session.serialize();
    constexpr std::array<PortalCrossingProbe, 4> kProbes{{
        {54, 5, 2, 0, z1::kGbaKeyRight},
        {58, 5, -2, 0, z1::kGbaKeyLeft},
        {56, 3, 0, 2, z1::kGbaKeyDown},
        {56, 7, 0, -2, z1::kGbaKeyUp},
    }};
    for (const auto probe : kProbes) {
        auto candidate = initial;
        const auto x = static_cast<std::int16_t>(probe.x + 8);
        const auto y = static_cast<std::int16_t>(probe.y + 72);
        const auto direction = probe.dx > 0 ? z1::OverworldWalkDirection::kRight :
            probe.dx < 0 ? z1::OverworldWalkDirection::kLeft :
            probe.dy > 0 ? z1::OverworldWalkDirection::kDown :
                            z1::OverworldWalkDirection::kUp;
        // Real Walker state reaching an aligned mouth from two source pixels
        // away: phase +/-6 advances to +/-7 then grid-zero at the doorway.
        stage_source_segment(&candidate, x, y,
                             static_cast<std::int8_t>((probe.dx < 0 || probe.dy < 0) ? -6 : 6),
                             direction);
        if (!session.restore(candidate, &error)) continue;
        const unsigned count = static_cast<unsigned>(probe.dx != 0
            ? (probe.dx < 0 ? -probe.dx : probe.dx)
            : (probe.dy < 0 ? -probe.dy : probe.dy));
        bool entered = false;
        for (unsigned i = 0; i < count; ++i) {
            if (session.move_by(probe.dx == 0 ? 0 : (probe.dx < 0 ? -1 : 1),
                                probe.dy == 0 ? 0 : (probe.dy < 0 ? -1 : 1)) !=
                z1::OverworldSessionMoveResult::kMoved)
                break;
            if (session.try_enter_cave() == z1::OverworldSessionCaveResult::kEntered) {
                entered = true;
                break;
            }
        }
        if (entered) {
            *state = candidate;
            *key = probe.key;
            *steps = count;
            return true;
        }
    }
    return false;
}

// These are the actual CheckWarps object coordinates, rather than the
// renderer tile's top-left coordinate.  GetCollidableTileStill samples the
// final map at ObjY+$0b, so OW77's $24 mouth is Obj=($40,$4d).  Keep the
// fixture ROM-backed: any decoder/layout change must still prove that exact
// source point produces the normal cave transition.
bool source_cave_stationary_hotspot(
    const char* path,
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state) {
    if (!path || !state) return false;
    z1::Zelda1OverworldSession session;
    std::string error;
    if (!session.load_hash_validated_ines(path, z1::Zelda1OverworldSession::kInitialRoom,
                                          &error)) return false;
    auto candidate = session.serialize();
    stage_source_position(&candidate, 0x40, 0x4d);
    if (!session.restore(candidate, &error) ||
        session.try_enter_cave() != z1::OverworldSessionCaveResult::kEntered)
        return false;
    *state = candidate;
    return true;
}

// Z_01's cave-person state checks the current ObjX and abs(ObjY-$98) each
// ordinary update; produce that exact ROM-backed, post-text touch position so
// the plugin test can prove no A edge is needed to take the wooden sword.
bool source_start_sword_touch_hotspot(
    const char* path,
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state) {
    if (!path || !state) return false;
    z1::Zelda1OverworldSession session;
    std::string error;
    if (!session.load_hash_validated_ines(path, z1::Zelda1OverworldSession::kInitialRoom,
                                          &error)) return false;
    auto candidate = session.serialize();
    stage_source_position(&candidate, 0x40, 0x4d);
    if (!session.restore(candidate, &error) ||
        session.try_enter_cave() != z1::OverworldSessionCaveResult::kEntered)
        return false;
    while (!session.cave_dialogue_acknowledged())
        if (!session.tick_cave_frame()) return false;
    for (unsigned i = 0; i != 0x15; ++i)
        if (session.move_cave_one(z1::CaveDirection::kUp) != z1::StartCaveMoveResult::kMoved)
            return false;
    for (unsigned i = 0; i != 8; ++i)
        if (session.move_cave_one(z1::CaveDirection::kRight) != z1::StartCaveMoveResult::kMoved)
            return false;
    const auto position = session.cave_source_position();
    if (!position || position->x != 0x78 || position->y != 0x98) return false;
    *state = session.serialize();
    return true;
}

bool source_level1_crossing_start(
    const char* path, std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state,
    std::uint16_t* key) {
    if (!path || !state || !key) return false;
    z1::Zelda1OverworldSession session;
    std::string error;
    if (!session.load_hash_validated_ines(path, 0x37, &error)) return false;
    const auto initial = session.serialize();
    // Level entrances are aligned source warp triggers. There are only 150
    // such grid candidates in the virtual crop, so find the actual decoded
    // Level-1 trigger rather than inventing coordinates.
    for (unsigned y = 5; y < z1::kOwRenderHeight; y += 16) {
        for (unsigned x = 8; x < z1::kOwRenderWidth; x += 16) {
            auto target = initial;
            stage_crop_position(&target, static_cast<std::int16_t>(x),
                                static_cast<std::int16_t>(y));
            if (!session.restore(target, &error)) continue;
            z1::Zelda1Level1LiveAdapter adapter;
            if (adapter.try_enter_from_overworld(session) != z1::Level1LiveResult::kEntered)
                continue;
            constexpr std::array<PortalCrossingProbe, 4> kNeighbors{{
                {0, 0, 1, 0, z1::kGbaKeyRight}, {0, 0, -1, 0, z1::kGbaKeyLeft},
                {0, 0, 0, 1, z1::kGbaKeyDown}, {0, 0, 0, -1, z1::kGbaKeyUp},
            }};
            for (const auto neighbor : kNeighbors) {
                auto candidate = initial;
                const int px = static_cast<int>(x) - neighbor.dx;
                const int py = static_cast<int>(y) - neighbor.dy;
                if (px < 0 || py < 0) continue;
                const auto direction = neighbor.dx > 0 ? z1::OverworldWalkDirection::kRight :
                    neighbor.dx < 0 ? z1::OverworldWalkDirection::kLeft :
                    neighbor.dy > 0 ? z1::OverworldWalkDirection::kDown :
                                      z1::OverworldWalkDirection::kUp;
                stage_source_segment(&candidate, static_cast<std::int16_t>(px + 8),
                                     static_cast<std::int16_t>(py + 72),
                                     static_cast<std::int8_t>((neighbor.dx < 0 || neighbor.dy < 0) ? -7 : 7),
                                     direction);
                if (!session.restore(candidate, &error) ||
                    session.move_by(neighbor.dx, neighbor.dy) !=
                        z1::OverworldSessionMoveResult::kMoved)
                    continue;
                z1::Zelda1Level1LiveAdapter crossing_adapter;
                if (crossing_adapter.try_enter_from_overworld(session) ==
                    z1::Level1LiveResult::kEntered) {
                    *state = candidate;
                    *key = neighbor.key;
                    return true;
                }
            }
        }
    }
    return false;
}

// OW37's Level 1 entrance is likewise an aligned object point whose visible
// $24 final tile is read at ObjY+$0b: Obj=($70,$7d).  Do not derive this from
// an input trace; ask the real verified First Quest decoder and live adapter.
bool source_level1_stationary_hotspot(
    const char* path,
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize>* state) {
    if (!path || !state) return false;
    z1::Zelda1OverworldSession session;
    std::string error;
    if (!session.load_hash_validated_ines(path, 0x37, &error)) return false;
    auto candidate = session.serialize();
    stage_source_position(&candidate, 0x70, 0x7d);
    if (!session.restore(candidate, &error)) return false;
    z1::Zelda1Level1LiveAdapter adapter;
    if (adapter.try_enter_from_overworld(session) != z1::Level1LiveResult::kEntered)
        return false;
    *state = candidate;
    return true;
}

}  // namespace

const std::uint16_t* g_foreign_background = nullptr;
const GbaForeignObjFocusTransform* g_foreign_focus = nullptr;
const GbaForeignScreenOverlay* g_foreign_overlay = nullptr;

extern "C" int gba_mod_register_activation_plugin(const char*,
                                                    GBAModActivationCallback) {
    return 1;
}
extern "C" int gba_mod_register_reset_callback(GBAModActivationCallback) {
    return 1;
}
extern "C" int gba_mod_publish_foreign_background(const char* plugin_id,
                                                    const std::uint16_t* pixels) {
    if (!g_background_publish_allowed ||
        std::string(plugin_id) != z1::kZelda1ForeignWorldPluginId || !pixels)
        return 0;
    g_foreign_background = pixels;
    return 1;
}
extern "C" void gba_mod_clear_foreign_background() {
    g_foreign_background = nullptr;
}
extern "C" int gba_mod_publish_foreign_obj_focus(
    const char* plugin_id, const GbaForeignObjFocusTransform* focus) {
    if (!g_focus_publish_allowed ||
        std::string(plugin_id) != z1::kZelda1ForeignWorldPluginId || !focus)
        return 0;
    g_foreign_focus = focus;
    return 1;
}
extern "C" void gba_mod_clear_foreign_obj_focus() { g_foreign_focus = nullptr; }
extern "C" int gba_mod_publish_foreign_screen_overlay(
    const char* plugin_id, const GbaForeignScreenOverlay* overlay) {
    if (std::string(plugin_id) != z1::kZelda1ForeignWorldPluginId || !overlay ||
        overlay->abi_version != GBA_FOREIGN_SCREEN_OVERLAY_ABI_VERSION ||
        !overlay->pixels || !overlay->alpha_q4)
        return 0;
    g_foreign_overlay = overlay;
    return 1;
}
extern "C" void gba_mod_clear_foreign_screen_overlay() { g_foreign_overlay = nullptr; }
extern "C" const char* gba_mod_required_asset_path(const char* package_id,
                                                     const char* asset_id) {
    if (std::string(package_id) != z1::kZelda1ForeignWorldPackageId ||
        std::string(asset_id) != z1::kZelda1ForeignWorldAssetId)
        return nullptr;
    return g_asset_path;
}
extern "C" std::uint8_t bus_read_u8(std::uint32_t address) {
    switch (address) {
    case 0x03000BF4: return g_area;
    case 0x03000BF5: return g_room;
    case 0x030010A8: return g_transitioning_out;
    case 0x03001004: return g_main_substate;
    case 0x03001168: return g_player_kind;
    case 0x03001170: return g_player_flags; // 0xA0 is valid Player state.
    case 0x0300116C: return g_player_action;
    case 0x03001175: return g_player_direction;
    case 0x03001178: return g_player_draw;
    case 0x03003FBC: return g_player_killed;
    case 0x03003F8A: return g_pause_player;
    case 0x02000050: return g_message_state;
    case 0x03000B81: return g_active_item_behavior;
    case 0x03000B89: return g_active_item_priority;
    case 0x03000B8A: return g_active_item_animation;
    case 0x03003F9B: return g_player_sword_state;
    case 0x03003F84: return g_player_attack_status;
    case 0x0300117C: return g_player_animation_state;
    default: return 0;
    }
}
extern "C" std::uint16_t bus_read_u16(std::uint32_t address) {
    if (address == 0x04000130) return g_keyinput;
    if (address == 0x03003DC8) return g_priority_timer;
    if (address == 0x03001184) return g_player_speed;
    if (address == 0x030011C0) return g_player_sprite_vram_offset;
    if (address == 0x0300118E) return static_cast<std::uint16_t>(g_player_feet_x);
    if (address == 0x03001192) return static_cast<std::uint16_t>(g_player_feet_y);
    if (address == 0x03000BF6) return static_cast<std::uint16_t>(g_room_origin_x);
    if (address == 0x03000BF8) return static_cast<std::uint16_t>(g_room_origin_y);
    return 0;  // The saved Mode 3 VRAM snapshot is irrelevant to this bridge test.
}
extern "C" std::uint32_t bus_read_u32(std::uint32_t address) {
    if (address == 0x030010A0) return g_frame;
    if (address == 0x03003FB0) return g_move_locks;
    if (address == 0x0300401C) return g_player_macro;
    return 0;
}
extern "C" void bus_write_u16(std::uint32_t, std::uint16_t) { ++g_bus_write_count; }

bool enter_portal(ArmCpuState* cpu) {
    if (!cpu) return false;
    g_player_feet_x = static_cast<std::int16_t>(g_room_origin_x +
                                                z1::kMinishPortalAnchorLocalX);
    g_player_feet_y = static_cast<std::int16_t>(g_room_origin_y +
                                                z1::kMinishPortalAnchorLocalY);
    // First source frame arms the release gate and visibly publishes the native
    // portal. The next new A press is the only native-world entry action.
    g_keyinput = 0x03ff;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, cpu);
    if (!g_foreign_overlay || g_foreign_background || g_foreign_focus) return false;
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kGbaKeyA);
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, cpu);
    return g_foreign_background && g_foreign_focus && !g_foreign_overlay;
}

int main(int argc, char** argv) {
    if (argc > 2) return fail("optional argument is the validated Zelda 1 iNES path");
    z1::reset_smith_yard_readiness_plugin();
    z1::activate_smith_yard_readiness_plugin();
    if (gba_mod_function_hook_enabled(z1::kZelda1ForeignWorldPluginId))
        return fail("missing required asset enabled the observation hook");

    g_asset_path = "C:/user/zelda1.nes";
    z1::activate_smith_yard_readiness_plugin();
    if (gba_mod_function_hook_enabled(z1::kZelda1ForeignWorldPluginId))
        return fail("unreadable asset path enabled the observation hook");

    // Hash ownership is checked by the package runtime in the integration
    // path. This unit fixture deliberately has no user ROM; it verifies that
    // the second, canonical-iNES/decoder check also fails closed. The live
    // loader test exercises actual OW77 decoding separately.
    if (argc == 1) {
        std::cout << "Smith-yard plugin fails closed before hooks on unreadable asset\n";
        return 0;
    }

    // Optional actual-ROM integration replay: it uses only the registered
    // UpdateEntities and exact Player LinearMoveDirectionOLD hooks. It proves
    // the handled movement preserves every guest register except PC, while
    // the plugin publishes the immutable foreign frame/focus pair.
    z1::reset_smith_yard_readiness_plugin();
    g_asset_path = argv[1];
    g_area = 3; g_room = 1; g_transitioning_out = 0; g_main_substate = 2;
    g_player_kind = 1; g_player_flags = 0xA0; g_player_action = 1;
    g_player_direction = 8; g_player_speed = 0;
    g_player_draw = 0x13; g_player_killed = 0; g_pause_player = 0;
    g_message_state = 0; g_priority_timer = 0; g_move_locks = 0;
    g_player_macro = 0; g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    z1::activate_smith_yard_readiness_plugin();
    if (!gba_mod_function_hook_enabled(z1::kZelda1ForeignWorldPluginId))
        return fail("actual validated iNES asset did not enable the trusted hooks");
    ArmCpuState observe_cpu{};
    const auto observe_before = observe_cpu;
    const unsigned writes_before_phase_order_entry = g_bus_write_count;
    const auto make_unready_phase = [] {
        g_main_substate = 0; g_player_draw = 0; g_player_action = 4;
        g_pause_player = 0x80; g_message_state = 1; g_priority_timer = 1;
        g_move_locks = 0x22189B75u; g_player_macro = 1;
    };
    const auto make_ready_phase = [] {
        g_main_substate = 2; g_player_draw = 0x13; g_player_action = 1;
        g_pause_player = 0; g_message_state = 0; g_priority_timer = 0;
        g_move_locks = 0; g_player_macro = 0;
    };
    /* The two observed UpdateEntities entries can expose different native
    // phases for one gRoomTransition.frameCount. A transient ROM phase must
    // not consume the source-frame activation heartbeat before the IWRAM
    // phase satisfies every documented portal predicate. Conversely, this is
    // still exactly 18 *frames*, not 36 callbacks.
    for (unsigned i = 0; i < z1::SmithYardQaRoom::kActivationUpdates; ++i) {
        ++g_frame;
        g_main_substate = 0; g_player_draw = 0; g_player_action = 4;
        g_pause_player = 0x80; g_message_state = 1; g_priority_timer = 1;
        g_move_locks = 0x22189B75u; g_player_macro = 1;
        if (gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu) != 0)
            return fail("read-only UpdateEntities observer unexpectedly replaced guest control");
        if (g_foreign_background || g_foreign_focus)
            return fail("unready ROM phase entered Zelda before its ready IWRAM peer");
        g_main_substate = 2; g_player_draw = 0x13; g_player_action = 1;
        g_pause_player = 0; g_message_state = 0; g_priority_timer = 0;
        g_move_locks = 0; g_player_macro = 0;
        if (gba_mod_function_entry(0x03005F40u, 0, &observe_cpu) != 0)
            return fail("read-only IWRAM UpdateEntities observer unexpectedly replaced guest control");
        if (i + 1 < z1::SmithYardQaRoom::kActivationUpdates &&
            (g_foreign_background || g_foreign_focus))
            return fail("duplicate hook callbacks shortened source-frame portal activation");
    }
    if (observe_cpu.R[15] != observe_before.R[15] || !g_foreign_background || !g_foreign_focus ||
        g_bus_write_count != writes_before_phase_order_entry ||
        g_foreign_focus->destination_link_feet_x != 128 ||
        g_foreign_focus->destination_link_feet_y != 101)
        return fail("later ready IWRAM phase did not publish OW exactly on source activation frame");
    const auto make_unready_phase = [] {
        g_main_substate = 0; g_player_draw = 0; g_player_action = 4;
        g_pause_player = 0x80; g_message_state = 1; g_priority_timer = 1;
        g_move_locks = 0x22189B75u; g_player_macro = 1;
    };
    const auto make_ready_phase = [] {
        g_main_substate = 2; g_player_draw = 0x13; g_player_action = 1;
        g_pause_player = 0; g_message_state = 0; g_priority_timer = 0;
        g_move_locks = 0; g_player_macro = 0;
    };
    // Leave that first active lease, then release only during an unready
    // inactive phase. The release must clear the exit latch without becoming
    // an entry heartbeat. Mirror the source phase order afterward: IWRAM
    // transient first, ROM ready second, for exactly 18 source frames.
    g_keyinput = 0x03ff;
    make_unready_phase();
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    make_ready_phase();
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background || g_foreign_focus ||
        minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("explicit chord did not leave the first phase-order Zelda lease");
    g_keyinput = 0x03ff;
    make_unready_phase();
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    for (unsigned i = 0; i < z1::SmithYardQaRoom::kActivationUpdates; ++i) {
        g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
        make_unready_phase();
        ++g_frame;
        (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
        if (g_foreign_background || g_foreign_focus)
            return fail("unready IWRAM phase re-entered Zelda before its ready ROM peer");
        make_ready_phase();
        (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
        if (i + 1 < z1::SmithYardQaRoom::kActivationUpdates &&
            (g_foreign_background || g_foreign_focus))
            return fail("ready ROM second phase shortened source-frame portal activation");
    }
    if (!g_foreign_background || !g_foreign_focus)
        return fail("ready ROM phase did not enter Zelda after 18 mirrored source frames");
    // A partial source-ready hold must be discarded by a later unready
    // release; otherwise the next portal attempt inherits stale heartbeats.
    g_keyinput = 0x03ff;
    make_ready_phase();
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background || g_foreign_focus)
        return fail("second explicit chord did not leave Zelda before partial-hold replay");
    g_keyinput = 0x03ff;
    make_unready_phase();
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    make_ready_phase();
    for (unsigned i = 0; i != 5; ++i) {
        ++g_frame;
        (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    }
    g_keyinput = 0x03ff;
    make_unready_phase();
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    make_ready_phase();
    for (unsigned i = 0; i < z1::SmithYardQaRoom::kActivationUpdates; ++i) {
        ++g_frame;
        (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
        if (i + 1 < z1::SmithYardQaRoom::kActivationUpdates &&
            (g_foreign_background || g_foreign_focus))
            return fail("unready release failed to discard the partial portal hold");
    }
    if (!g_foreign_background || !g_foreign_focus)
        return fail("full ready hold after unready release could not re-enter Zelda"); */
    // A held A never enters after activation until a released sample has
    // armed the native portal. The ROM/IWRAM duplicate hook order may differ
    // within one source frame, so let the transient hook see release and the
    // stable peer consume the press.
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kGbaKeyA);
    make_unready_phase();
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    make_ready_phase();
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (g_foreign_background || g_foreign_focus || !g_foreign_overlay)
        return fail("held A bypassed portal release gate or native portal was not rendered");
    make_unready_phase();
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_overlay || g_foreign_background || g_foreign_focus)
        return fail("unready second hook incorrectly erased same-frame native portal");
    g_keyinput = 0x03ff;
    make_unready_phase();
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (g_foreign_overlay)
        return fail("portal remained visible through an unready source phase");
    make_ready_phase();
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kGbaKeyA);
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (observe_cpu.R[15] != observe_before.R[15] || !g_foreign_background || !g_foreign_focus ||
        g_foreign_overlay || g_bus_write_count != writes_before_phase_order_entry)
        return fail("released-then-A portal interaction did not enter read-only");
    // The retired all-Dpad route remains an explicit exit only. After exit,
    // holding every direction outside/inside the portal never re-enters.
    g_keyinput = 0x03ff;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background || g_foreign_focus)
        return fail("all-Dpad did not remain the explicit active-world exit");
    for (unsigned i = 0; i != 2; ++i) {
        ++g_frame;
        (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
        if (g_foreign_background || g_foreign_focus)
            return fail("all-Dpad incorrectly re-entered Zelda while inactive");
    }
    if (!g_foreign_overlay)
        return fail("inactive ready world stopped rendering the native portal");
    const unsigned writes_before_transition = g_bus_write_count;
    (void)gba_mod_function_entry(0x08080840u, 1, &observe_cpu);
    if (g_foreign_overlay || g_bus_write_count != writes_before_transition)
        return fail("DoExitTransition did not clear only the native portal overlay");
    g_keyinput = 0x03ff;
    if (!enter_portal(&observe_cpu))
        return fail("native portal could not re-enter after all-Dpad/transition regression");
    // TMC player.c:CheckInitPauseMenu calls InitPauseMenu only after validating
    // a new Start press and normal control. The exact InitPauseMenu hook must
    // therefore suspend the foreign PPU pair without treating raw/rejected
    // Start input as a menu, and the next UpdateEntities after native menu
    // close must restore the same host-owned Zelda session.
    const auto menu_blob_before = minish::foreign_world::foreign_world_native_state()
        .zelda1_overworld_session_blob();
    const auto menu_focus_before = *g_foreign_focus;
    const unsigned writes_before_menu = g_bus_write_count;
    if (gba_mod_function_entry(0x080A4D8Au, 1, &observe_cpu) != 0 ||
        !g_foreign_background || !g_foreign_focus)
        return fail("non-matching pause-menu code address changed foreign presentation");
    if (gba_mod_function_entry(0x080A4D88u, 1, &observe_cpu) != 0 ||
        g_foreign_background || g_foreign_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_menu || !same_cpu(observe_cpu, observe_before))
        return fail("eligible native InitPauseMenu did not suspend foreign presentation read-only");
    // A provider snapshot made while native menu composition owns the screen
    // must retain logical Zelda activity.  Restoring it may rebuild the host
    // session, but it must not reinterpret the temporary cleared pointers as
    // an inactive world or lose the exact source position.
    minish::foreign_world::ForeignWorldNativeState menu_open_snapshot;
    std::string menu_snapshot_error;
    if (!menu_open_snapshot.set_zelda1_overworld_session_blob(menu_blob_before,
                                                                &menu_snapshot_error))
        return fail("could not stage menu-open foreign snapshot: " + menu_snapshot_error);
    menu_open_snapshot.set_zelda1_presentation_active(true);
    minish::foreign_world::foreign_world_native_state().replace_after_provider_restore(
        std::move(menu_open_snapshot));
    ++g_frame;
    g_keyinput = 0x03ff;
    if (gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu) != 0 ||
        !g_foreign_background || !g_foreign_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        minish::foreign_world::foreign_world_native_state().zelda1_overworld_session_blob() !=
            menu_blob_before ||
        g_foreign_focus->destination_link_feet_x != menu_focus_before.destination_link_feet_x ||
        g_foreign_focus->destination_link_feet_y != menu_focus_before.destination_link_feet_y ||
        g_bus_write_count != writes_before_menu || !same_cpu(observe_cpu, observe_before))
        return fail("native pause-menu close did not resume the stable Zelda session read-only");
    // A rejected first menu-close publication happens after InitPauseMenu has
    // intentionally cleared the foreign PPU pair. It must still retain the
    // logical Zelda shield (blocking native player motion), freeze/retry on
    // the following source frame, and never turn the transient null pair into
    // a silent world return.
    if (gba_mod_function_entry(0x080A4D88u, 1, &observe_cpu) != 0 ||
        g_foreign_background || g_foreign_focus)
        return fail("menu-close failure fixture could not suspend Zelda presentation");
    g_background_publish_allowed = false;
    ++g_frame;
    g_keyinput = 0x03ff;
    if (gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu) != 0 ||
        g_foreign_background || g_foreign_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("rejected menu-close publication lost the logical Zelda shield");
    ArmCpuState frozen_menu_move{};
    frozen_menu_move.R[0] = z1::kMinishPlayerEntityAddress;
    frozen_menu_move.R[14] = 0x0800ABCDu;
    const auto frozen_menu_move_before = frozen_menu_move;
    if (gba_mod_function_entry(0x080027EAu, 1, &frozen_menu_move) != 1 ||
        frozen_menu_move.R[15] !=
            z1::linear_move_handled_return_pc(frozen_menu_move_before.R[14]))
        return fail("rejected menu-close publication reopened native player movement");
    g_background_publish_allowed = true;
    ++g_frame;
    if (gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu) != 0 ||
        !g_foreign_background || !g_foreign_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("healthy menu-close retry did not resume the frozen Zelda session");
    // A provider restore while the native Start menu owns the compositor may
    // explicitly make the Zelda presentation inactive. That restore is a
    // lifecycle boundary, so it must clear the menu-only suspension latch;
    // otherwise every later observer returns before it can see a legitimate
    // portal chord.
    if (gba_mod_function_entry(0x080A4D88u, 1, &observe_cpu) != 0 ||
        g_foreign_background || g_foreign_focus)
        return fail("second native InitPauseMenu did not suspend Zelda fixture");
    minish::foreign_world::ForeignWorldNativeState inactive_menu_snapshot;
    if (!inactive_menu_snapshot.set_zelda1_overworld_session_blob(
            menu_blob_before, &menu_snapshot_error))
        return fail("could not stage inactive menu provider snapshot: " + menu_snapshot_error);
    inactive_menu_snapshot.set_zelda1_presentation_active(false);
    minish::foreign_world::foreign_world_native_state().replace_after_provider_restore(
        std::move(inactive_menu_snapshot));
    g_keyinput = 0x03ff;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background || g_foreign_focus ||
        minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_menu)
        return fail("inactive menu restore did not clear presentation before re-entry");
    if (!enter_portal(&observe_cpu) ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_menu)
        return fail("inactive menu restore left portal entry permanently suspended");
    // Foreign locomotion is frame/KEYINPUT driven.  Deliberately do not call
    // LinearMoveDirectionOLD here: Minish collision can suppress that
    // callback inside the native starting house, but a held D-pad still must
    // move the virtual Zelda player one pixel. The IWRAM observer is then
    // invoked at the same source frame to prove the dual registration cannot
    // apply a second host step.
    const auto initial_focus = *g_foreign_focus;
    const auto writes_before_locomotion = g_bus_write_count;
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kGbaKeyLeft);
    ++g_frame;
    if (gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu) != 0 ||
        !g_foreign_focus ||
        g_foreign_focus->destination_link_feet_x !=
            static_cast<std::int16_t>(initial_focus.destination_link_feet_x - 1) ||
        g_foreign_focus->destination_link_feet_y != initial_focus.destination_link_feet_y)
        return fail("KEYINPUT-only OW locomotion did not move one virtual pixel");
    const auto after_rom_observer = *g_foreign_focus;
    if (gba_mod_function_entry(0x03005F40u, 0, &observe_cpu) != 0 ||
        !g_foreign_focus ||
        g_foreign_focus->destination_link_feet_x !=
            after_rom_observer.destination_link_feet_x ||
        g_foreign_focus->destination_link_feet_y !=
            after_rom_observer.destination_link_feet_y)
        return fail("ROM/IWRAM observers applied foreign locomotion twice in one frame");
    if (g_bus_write_count != writes_before_locomotion || observe_cpu.R[15] != observe_before.R[15])
        return fail("KEYINPUT-only foreign locomotion wrote guest memory or CPU state");
    // The ROM and IWRAM UpdateEntities hooks can bracket distinct native
    // phases of one gRoomTransition.frameCount.  The active Zelda lease must
    // neither run its lifecycle twice nor treat underlying Minish roll/item/
    // menu/redraw state as an unrequested return portal. Exercise both hook
    // orders, then a long alternate-phase replay, using only read-only guest
    // observations and released input.
    const unsigned writes_before_lifecycle = g_bus_write_count;
    const auto check_foreign_lease = [&]() {
        return g_foreign_background && g_foreign_focus &&
            minish::foreign_world::foreign_world_native_state().zelda1_presentation_active();
    };
    // Transient lock first, then the normal phase on the same source frame.
    ++g_frame;
    g_main_substate = 0; g_player_draw = 0; g_player_action = 4;
    g_pause_player = 0x80; g_message_state = 1; g_priority_timer = 1;
    g_move_locks = 0x22189B75u; g_player_macro = 1; g_keyinput = 0x03ff;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (!check_foreign_lease())
        return fail("IWRAM transient action/menu phase unexpectedly returned from Zelda");
    g_main_substate = 2; g_player_draw = 0x13; g_player_action = 1;
    g_pause_player = 0; g_message_state = 0; g_priority_timer = 0;
    g_move_locks = 0; g_player_macro = 0;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!check_foreign_lease())
        return fail("ROM second hook unexpectedly returned from Zelda");
    // Normal phase first, then the same transient state on the opposite hook.
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    g_main_substate = 0; g_player_draw = 0; g_player_action = 4;
    g_pause_player = 0x80; g_message_state = 1; g_priority_timer = 1;
    g_move_locks = 0x22189B75u; g_player_macro = 1;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (!check_foreign_lease())
        return fail("IWRAM second transient hook unexpectedly returned from Zelda");
    // Headless long-run replay: normal and transient guest action/UI phases
    // alternate for 1,024 native frames, and their hook order alternates too.
    // Neither is an explicit all-D-pad return chord nor a fatal lifecycle.
    for (unsigned frame = 0; frame != 1024; ++frame) {
        ++g_frame;
        const bool transient = (frame & 1) != 0;
        g_main_substate = transient ? 0 : 2;
        g_player_draw = transient ? 0 : 0x13;
        g_player_action = transient ? 4 : 1;
        g_pause_player = transient ? 0x80 : 0;
        g_message_state = transient ? 1 : 0;
        g_priority_timer = transient ? 1 : 0;
        g_move_locks = transient ? 0x22189B75u : 0;
        g_player_macro = transient ? 1 : 0;
        g_keyinput = 0x03ff;
        const std::uint32_t first_hook =
            (frame & 2) == 0 ? 0x0805E5C0u : 0x03005F40u;
        const int first_phase = first_hook == 0x0805E5C0u ? 1 : 0;
        const std::uint32_t second_hook =
            first_hook == 0x0805E5C0u ? 0x03005F40u : 0x0805E5C0u;
        const int second_phase = second_hook == 0x0805E5C0u ? 1 : 0;
        (void)gba_mod_function_entry(first_hook, first_phase, &observe_cpu);
        (void)gba_mod_function_entry(second_hook, second_phase, &observe_cpu);
        if (!check_foreign_lease())
            return fail("long-run transient native state returned from active Zelda session");
    }
    g_main_substate = 2; g_player_draw = 0x13; g_player_action = 1;
    g_pause_player = 0; g_message_state = 0; g_priority_timer = 0;
    g_move_locks = 0; g_player_macro = 0; g_keyinput = 0x03ff;
    if (g_bus_write_count != writes_before_lifecycle || !same_cpu(observe_cpu, observe_before))
        return fail("lifecycle replay wrote guest memory or CPU state");
    // Exercise the provider-restore generation bridge in both directions.
    // The active snapshot must rebuild/publish despite the controller being
    // inactive at restore time; an inactive snapshot must clear an active
    // controller's PPU pointers without any guest write.
    std::string persistence_error;
    minish::foreign_world::ForeignWorldNativeState active_snapshot;
    if (!active_snapshot.set_zelda1_overworld_session_blob(
            minish::foreign_world::foreign_world_native_state()
                .zelda1_overworld_session_blob(), &persistence_error))
        return fail("could not stage active provider snapshot: " + persistence_error);
    active_snapshot.set_zelda1_presentation_active(true);
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background || g_foreign_focus ||
        minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("normal chord exit did not checkpoint inactive presentation lifecycle");
    // Holding the explicit exit chord must remain inactive until a release;
    // otherwise an apparent return can immediately re-enter on a later hook.
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (g_foreign_background || g_foreign_focus ||
        minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("held explicit exit chord re-entered Zelda without a release");
    const auto writes_before_restore = g_bus_write_count;
    minish::foreign_world::foreign_world_native_state().replace_after_provider_restore(
        std::move(active_snapshot));
    g_keyinput = 0x03ff;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_background || !g_foreign_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_restore)
        return fail("active provider restore did not republish without guest mutation");
    minish::foreign_world::ForeignWorldNativeState inactive_snapshot;
    if (!inactive_snapshot.set_zelda1_overworld_session_blob(
            minish::foreign_world::foreign_world_native_state()
                .zelda1_overworld_session_blob(), &persistence_error))
        return fail("could not stage inactive provider snapshot: " + persistence_error);
    inactive_snapshot.set_zelda1_presentation_active(false);
    minish::foreign_world::foreign_world_native_state().replace_after_provider_restore(
        std::move(inactive_snapshot));
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background || g_foreign_focus ||
        minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_restore)
        return fail("inactive provider restore did not clear active presentation safely");
    // Re-enter before the independent source-cave and Level-1 input replays.
    if (!enter_portal(&observe_cpu))
        return fail("inactive provider restore could not re-enter through native portal");
    // Feed a source-valid OW77 pre-mouth state through the native provider,
    // then enter solely through held KEYINPUT frames. This explicitly proves
    // the observer owns exact cave entry even when no LinearMove hook occurs.
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize> cave_start{};
    std::uint16_t cave_direction = 0;
    unsigned cave_steps = 0;
    if (!source_cave_crossing_start(argv[1], &cave_start, &cave_direction, &cave_steps))
        return fail("could not derive an actual-ROM OW77 source cave crossing probe");
    z1::reset_smith_yard_readiness_plugin();
    if (!minish::foreign_world::foreign_world_native_state()
             .set_zelda1_overworld_session_blob(cave_start, &persistence_error))
        return fail("could not stage source cave crossing state: " + persistence_error);
    g_keyinput = 0x03ff;
    z1::activate_smith_yard_readiness_plugin();
    if (!enter_portal(&observe_cpu))
        return fail("source cave fixture could not enter through native portal");
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~cave_direction);
    for (unsigned i = 0; i < cave_steps; ++i) {
        ++g_frame;
        (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    }
    const auto& cave_after_input = minish::foreign_world::foreign_world_native_state()
        .zelda1_overworld_session_blob();
    if (cave_after_input.size() != z1::Zelda1OverworldSession::kSerializedSize ||
        cave_after_input[11] != static_cast<std::uint8_t>(z1::OverworldSessionArea::kCave))
        return fail("KEYINPUT-only exact OW77 cave crossing did not enter the foreign cave");
    // CheckWarps also runs while Link is standing still. The old adapter only
    // attempted entrances after a successful virtual movement probe, so a
    // human releasing the D-pad on a source doorway could be stranded. Feed
    // the real OW77 mouth through the plugin, release every direction, and
    // require the next foreign update to enter without any guest write.
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize> cave_stationary{};
    if (!source_cave_stationary_hotspot(argv[1], &cave_stationary))
        return fail("could not verify the actual-ROM OW77 stationary cave hotspot");
    z1::reset_smith_yard_readiness_plugin();
    if (!minish::foreign_world::foreign_world_native_state()
             .set_zelda1_overworld_session_blob(cave_stationary, &persistence_error))
        return fail("could not stage OW77 stationary cave hotspot: " + persistence_error);
    g_keyinput = 0x03ff;
    z1::activate_smith_yard_readiness_plugin();
    if (!enter_portal(&observe_cpu))
        return fail("stationary cave fixture could not enter through native portal");
    g_keyinput = 0x03ff;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    const auto& cave_after_stationary = minish::foreign_world::foreign_world_native_state()
        .zelda1_overworld_session_blob();
    if (cave_after_stationary.size() != z1::Zelda1OverworldSession::kSerializedSize ||
        cave_after_stationary[11] != static_cast<std::uint8_t>(z1::OverworldSessionArea::kCave))
        return fail("stationary OW77 mouth did not enter through the plugin observer");
    // The PPU consumes this pointer directly. Complete the automatic textbox,
    // then move Right through ordinary UpdateEntities frames: a cave fire
    // tick must publish a fully rendered *new* buffer while retaining the
    // former one unchanged through that next update. This is intentionally a
    // plugin seam test, not a staged renderer call; it covers source movement,
    // persistence sync, focus publication, and the live pointer lifetime.
    for (unsigned i = 0;
         i != z1::Zelda1StartCaveControl::kFirstQuestStartDialogueGlyphCount *
                  z1::Zelda1StartCaveControl::kTextboxFramesPerGlyph + 1;
         ++i) {
        ++g_frame;
        (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    }
    if (!g_foreign_background || !g_foreign_focus ||
        minish::foreign_world::foreign_world_native_state()
                .zelda1_overworld_session_blob()[16] != 1)
        return fail("source cave textbox did not remain visibly published before movement");
    const std::uint16_t* const before_right = g_foreign_background;
    const auto retained_before_right =
        std::make_unique<std::array<std::uint16_t, z1::kOwRenderWidth * z1::kOwRenderHeight>>();
    std::copy_n(before_right, retained_before_right->size(), retained_before_right->begin());
    const auto focus_x_before_right = g_foreign_focus->destination_link_feet_x;
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~0x0010u); // KEYINPUT Right.
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_background || !g_foreign_focus ||
        g_foreign_background == before_right ||
        !same_cave_nonfire_pixels(retained_before_right->data(), g_foreign_background) ||
        !same_cave_nonfire_pixels(retained_before_right->data(), before_right) ||
        g_foreign_focus->destination_link_feet_x != focus_x_before_right + 1 ||
        minish::foreign_world::foreign_world_native_state()
                .zelda1_overworld_session_blob()[11] !=
            static_cast<std::uint8_t>(z1::OverworldSessionArea::kCave))
        return fail("Right cave update did not retain an immutable complete foreign frame/focus");
    // PlayerRollUpdate's live Entity::speed/direction—not held KEYINPUT—owns
    // foreign momentum.  Use an opposing Left input to prove the 0x300 Q8.8
    // source sample still emits three separately collision-checked pixels.
    const auto roll_focus_x = g_foreign_focus->destination_link_feet_x;
    const auto roll_cpu_before = observe_cpu;
    const auto writes_before_roll = g_bus_write_count;
    g_player_action = z1::kMinishPlayerRollAction;
    g_player_direction = 8; // TMC DirectionEast / LinearMove table index 8.
    g_player_speed = 0x300;
    g_move_locks = z1::kMinishPlayerRollingFlag;
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kGbaKeyLeft);
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_focus ||
        g_foreign_focus->destination_link_feet_x != roll_focus_x + 3 ||
        !same_cpu(observe_cpu, roll_cpu_before) || g_bus_write_count != writes_before_roll)
        return fail("source roll did not preserve 3px momentum without guest writes");
    // The source's frame&0xf==3 phase writes speed=0.  It must remain still
    // even with a direction held, then ordinary walking resumes cleanly when
    // PlayerRoll terminates (including a transient action/lock change).
    const auto stopped_roll_x = g_foreign_focus->destination_link_feet_x;
    g_player_speed = 0;
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kGbaKeyRight);
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_focus || g_foreign_focus->destination_link_feet_x != stopped_roll_x)
        return fail("source zero-speed roll phase moved through foreign collision");
    g_player_action = 1;
    g_player_speed = 0;
    g_move_locks = 0;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_focus || g_foreign_focus->destination_link_feet_x != stopped_roll_x + 1 ||
        !g_foreign_background)
        return fail("roll termination did not resume ordinary foreign walk safely");
    g_keyinput = 0x03ff;
    // Actual-ROM source touch pickup: restore the completed start cave at
    // Obj=($78,$98), do not press A, and let one ordinary UpdateEntities frame
    // run the documented cave-person touch check. The persisted flag and exact
    // Zelda1/01 InventoryCore identity must commit together without a guest
    // write or a forged Native sword.
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize> sword_touch{};
    if (!source_start_sword_touch_hotspot(argv[1], &sword_touch))
        return fail("could not derive actual-ROM start-sword touch hotspot");
    z1::reset_smith_yard_readiness_plugin();
    auto& touch_native = minish::foreign_world::foreign_world_native_state();
    touch_native.inventory() = {};
    if (!touch_native.set_zelda1_overworld_session_blob(sword_touch, &persistence_error))
        return fail("could not stage source start-sword touch state: " + persistence_error);
    g_keyinput = 0x03ff;
    z1::activate_smith_yard_readiness_plugin();
    if (!enter_portal(&observe_cpu))
        return fail("start-sword fixture could not enter through native portal");
    const unsigned writes_before_touch = g_bus_write_count;
    g_keyinput = 0x03ff; // Released A remains part of this source touch replay.
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    const minish::foreign_world::CrossWorldItemId zelda_start_sword{
        minish::foreign_world::WorldId::Zelda1, 1};
    const auto& sword_touch_after = touch_native.zelda1_overworld_session_blob();
    if (sword_touch_after.size() != z1::Zelda1OverworldSession::kSerializedSize ||
        sword_touch_after[13] != 1 ||
        touch_native.inventory().ownership(zelda_start_sword) !=
            minish::foreign_world::OwnershipFlags::Owned ||
        touch_native.inventory().loadout_slot(minish::foreign_world::LoadoutId::A, 0) !=
            zelda_start_sword ||
        touch_native.inventory().ownership({minish::foreign_world::WorldId::Native, 1}) !=
            minish::foreign_world::OwnershipFlags::None ||
        g_bus_write_count != writes_before_touch)
        return fail("start-cave source touch did not atomically acquire exact Zelda1/01 sword");
    // The same per-pixel observer seam must activate decoded OW37 Level 1
    // entrances. A changed published framebuffer pointer is the plugin's
    // public proof that it switched from OW terrain to the Level-1 session.
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize> level_start{};
    std::uint16_t level_direction = 0;
    if (!source_level1_crossing_start(argv[1], &level_start, &level_direction))
        return fail("could not derive an actual-ROM OW37 Level-1 crossing probe");
    z1::reset_smith_yard_readiness_plugin();
    if (!minish::foreign_world::foreign_world_native_state()
             .set_zelda1_overworld_session_blob(level_start, &persistence_error))
        return fail("could not stage source Level-1 crossing state: " + persistence_error);
    g_keyinput = 0x03ff;
    z1::activate_smith_yard_readiness_plugin();
    if (!enter_portal(&observe_cpu))
        return fail("Level-1 fixture could not enter through native portal");
    const auto* overworld_pixels_before_level = g_foreign_background;
    // The source fixture is one legal pixel before the exact OW37 warp. A
    // 0x300 roll with KEYINPUT released must still enter on that first
    // emitted pixel, proving the host did not batch/skip the entrance.
    const auto source_direction_for_key = [](std::uint16_t key) -> std::uint8_t {
        if (key == z1::kGbaKeyRight) return 8;
        if (key == z1::kGbaKeyLeft) return 24;
        if (key == z1::kGbaKeyDown) return 16;
        return 0; // kGbaKeyUp
    };
    g_player_action = z1::kMinishPlayerRollAction;
    g_player_direction = source_direction_for_key(level_direction);
    g_player_speed = 0x300;
    g_move_locks = z1::kMinishPlayerRollingFlag;
    g_keyinput = 0x03ff;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_background || g_foreign_background == overworld_pixels_before_level)
        return fail("3px source roll tunneled past exact OW37 Level-1 entrance");
    // Mirror the cave proof for the exact OW37 ($70,$7d) source entrance:
    // a released D-pad still takes the Level 1 doorway during CheckWarps.
    std::array<std::uint8_t, z1::Zelda1OverworldSession::kSerializedSize> level_stationary{};
    if (!source_level1_stationary_hotspot(argv[1], &level_stationary))
        return fail("could not verify the actual-ROM OW37 stationary Level-1 hotspot");
    z1::reset_smith_yard_readiness_plugin();
    g_player_action = 1;
    g_player_direction = 8;
    g_player_speed = 0;
    g_move_locks = 0;
    if (!minish::foreign_world::foreign_world_native_state()
             .set_zelda1_overworld_session_blob(level_stationary, &persistence_error))
        return fail("could not stage OW37 stationary Level-1 hotspot: " + persistence_error);
    g_keyinput = 0x03ff;
    z1::activate_smith_yard_readiness_plugin();
    if (!enter_portal(&observe_cpu))
        return fail("stationary Level-1 fixture could not enter through native portal");
    const auto* overworld_pixels_before_stationary_level = g_foreign_background;
    g_keyinput = 0x03ff;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_background ||
        g_foreign_background == overworld_pixels_before_stationary_level)
        return fail("stationary OW37 doorway did not publish the Level-1 session");
    g_keyinput = 0x03ff;
    ArmCpuState movement_cpu{};
    movement_cpu.R[0] = z1::kMinishPlayerEntityAddress;
    movement_cpu.R[1] = 256;      // Q8.8 one-pixel source speed.
    movement_cpu.R[2] = 0;        // Source table direction: upward.
    movement_cpu.R[14] = 0x0800ABCDu;
    const auto movement_before = movement_cpu;
    if (gba_mod_function_entry(0x080027EAu, 1, &movement_cpu) != 1 ||
        movement_cpu.R[15] != z1::linear_move_handled_return_pc(movement_before.R[14]))
        return fail("trusted player movement hook did not route foreign movement");
    for (unsigned i = 0; i < 15; ++i)
        if (movement_cpu.R[i] != movement_before.R[i])
            return fail("handled player movement changed a guest GPR outside PC");
    if (movement_cpu.cpsr != movement_before.cpsr)
        return fail("handled player movement changed CPSR");
    // Actual-ROM selected-Zelda-sword replay. Use a source-decoded OW66
    // session whose virtual Link position puts the first source roster actor
    // directly north of a documented host hitbox. The only input is the
    // active-low A edge; all guest item fields remain inactive/read-only.
    z1::Zelda1OverworldSession ow66;
    if (!ow66.load_hash_validated_ines(argv[1], 0x66, &persistence_error) ||
        !ow66.octoroks().initialized())
        return fail("could not load the actual-ROM OW66 combat fixture: " + persistence_error);
    auto ow66_state = ow66.serialize();
    const auto first_actor = ow66.octoroks().actors()[0];
    if (first_actor.x < 8 || first_actor.y < 48)
        return fail("source OW66 actor cannot form a bounded host-hit fixture");
    // Preserve the previous crop fixture exactly: old crop=(actor.x-8,
    // actor.y-48) maps to v6 raw source=(actor.x, actor.y+24).
    stage_source_position(&ow66_state, first_actor.x,
                          static_cast<std::int16_t>(first_actor.y + 24));
    if (!ow66.restore(ow66_state, &persistence_error))
        return fail("source OW66 host-hit fixture was rejected: " + persistence_error);
    z1::reset_smith_yard_readiness_plugin();
    if (!minish::foreign_world::foreign_world_native_state()
             .set_zelda1_overworld_session_blob(ow66.serialize(), &persistence_error))
        return fail("could not stage OW66 combat provider state: " + persistence_error);
    auto& combat_inventory = minish::foreign_world::foreign_world_native_state().inventory();
    const minish::foreign_world::CrossWorldItemId zelda_sword{
        minish::foreign_world::WorldId::Zelda1, 1};
    const minish::foreign_world::ItemTraits zelda_traits{
        1, zelda_sword, minish::foreign_world::ItemUseKind::Equip,
        minish::foreign_world::ResourcePoolProvenance::None};
    if (!combat_inventory.acquire_item(zelda_sword,
                                       minish::foreign_world::OwnershipFlags::Owned,
                                       minish::foreign_world::Capability::Sword,
                                       zelda_traits, &persistence_error) ||
        !combat_inventory.set_loadout_slot(minish::foreign_world::LoadoutId::A, 0,
                                           zelda_sword, &persistence_error))
        return fail("could not stage selected Zelda1/01 sword: " + persistence_error);
    g_active_item_behavior = 0; g_active_item_priority = 0;
    g_active_item_animation = 0; g_player_sword_state = 0;
    g_player_attack_status = 0; g_player_animation_state = 0; // Entity IdleNorth.
    g_keyinput = 0x03ff;
    z1::activate_smith_yard_readiness_plugin();
    if (!enter_portal(&observe_cpu))
        return fail("combat fixture could not enter through native portal");
    // Establish the released state for the independent active-low combat edge.
    g_keyinput = 0x03ff;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    const auto before_host_edge = minish::foreign_world::foreign_world_native_state()
        .zelda1_overworld_session_blob();
    constexpr std::size_t kOw66Actor0HpOffset = 32 + 9 + 1;
    if (before_host_edge.size() != z1::Zelda1OverworldSession::kSerializedSize ||
        before_host_edge[kOw66Actor0HpOffset] == 0)
        return fail("OW66 fixture did not retain a live first Octorok");
    const unsigned writes_before_host_edge = g_bus_write_count;
    const ArmCpuState host_edge_before = observe_cpu;
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kGbaKeyA);
    ++g_frame;
    if (gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu) != 0)
        return fail("host-only Zelda sword edge replaced guest control");
    const auto after_host_edge = minish::foreign_world::foreign_world_native_state()
        .zelda1_overworld_session_blob();
    // The Zelda1/01 record cannot start a native TMC ItemSword on a save that
    // has no native sword. Its host A edge must therefore also publish a
    // visible, compositor-only blade from the focused Link feet. IdleNorth
    // begins the original host blade at feetY-16; this checks presentation,
    // not guest OAM/animation state.
    const bool host_swing_visible = g_foreign_background && g_foreign_focus &&
        g_foreign_focus->destination_link_feet_x >= 0 &&
        g_foreign_focus->destination_link_feet_x < 240 &&
        g_foreign_focus->destination_link_feet_y >= 16 &&
        g_foreign_focus->destination_link_feet_y < 160 &&
        g_foreign_background[
            static_cast<std::size_t>(g_foreign_focus->destination_link_feet_y - 16) *
                240 + static_cast<std::size_t>(g_foreign_focus->destination_link_feet_x)] ==
            0x7fffu;
    const bool host_edge_ok = after_host_edge.size() == before_host_edge.size() &&
        after_host_edge[kOw66Actor0HpOffset] + 1 == before_host_edge[kOw66Actor0HpOffset] &&
        g_bus_write_count == writes_before_host_edge && same_cpu(observe_cpu, host_edge_before) &&
        combat_inventory.loadout_slot(minish::foreign_world::LoadoutId::A, 0) == zelda_sword &&
        combat_inventory.ownership({minish::foreign_world::WorldId::Native, 1}) ==
            minish::foreign_world::OwnershipFlags::None && host_swing_visible;
    if (!host_edge_ok) {
        std::cerr << "host-edge diagnostic: before_hp="
                  << unsigned(before_host_edge[kOw66Actor0HpOffset]) << " after_hp="
                  << unsigned(after_host_edge[kOw66Actor0HpOffset]) << " writes="
                  << g_bus_write_count << '/' << writes_before_host_edge << " cpu="
                  << same_cpu(observe_cpu, host_edge_before) << " slot="
                  << (combat_inventory.loadout_slot(minish::foreign_world::LoadoutId::A, 0) == zelda_sword)
                  << " native=" << unsigned(static_cast<std::uint8_t>(
                      combat_inventory.ownership({minish::foreign_world::WorldId::Native, 1}))) << "\n";
        return fail("selected Zelda A edge did not publish a visible host-only exact-origin swing");
    }
    // Same source frame through the IWRAM entry cannot produce a second hit.
    if (gba_mod_function_entry(0x03005F40u, 0, &observe_cpu) != 0 ||
        minish::foreign_world::foreign_world_native_state().zelda1_overworld_session_blob() !=
            after_host_edge || g_bus_write_count != writes_before_host_edge ||
        !same_cpu(observe_cpu, host_edge_before))
        return fail("dual UpdateEntities observation duplicated the host Zelda sword hit");
    // Native room changes, transitions, and even a dead-host sample are not
    // foreign-world return portals. Exercise them in both hook orders: the
    // only user-visible exit remains the explicit release-latched all-D-pad
    // chord already covered by the provider snapshot replay above.
    g_keyinput = 0x03ff;
    g_transitioning_out = 1;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_background || !g_foreign_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("native transition on ROM hook unexpectedly returned from Zelda");
    g_transitioning_out = 0;
    g_area = 2; g_room = 0; g_player_killed = 1;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    g_transitioning_out = 1;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (!g_foreign_background || !g_foreign_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("native room/death/transition on IWRAM hook unexpectedly returned from Zelda");
    g_area = 3; g_room = 1; g_player_killed = 0; g_transitioning_out = 0;
    // A publication failure while already active freezes the last immutable
    // Zelda frame/focus rather than clearing to native Minish. It must also
    // remain frozen on later healthy callbacks until the explicit chord exit.
    const auto* frozen_background = g_foreign_background;
    const auto* frozen_focus = g_foreign_focus;
    const unsigned writes_before_publish_fault = g_bus_write_count;
    g_focus_publish_allowed = false;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background != frozen_background || g_foreign_focus != frozen_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_publish_fault)
        return fail("active publication failure exposed native Minish instead of freezing Zelda");
    g_focus_publish_allowed = true;
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (g_foreign_background != frozen_background || g_foreign_focus != frozen_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_publish_fault)
        return fail("frozen Zelda session resumed mutation without explicit lifecycle boundary");
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background || g_foreign_focus ||
        minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("explicit chord could not leave frozen Zelda presentation");
    z1::reset_smith_yard_readiness_plugin();
    // Complement the rejected-second-call focus test above with a rejected
    // first-call background test. A fresh activation is required because the
    // source policy intentionally freezes a faulted active session until an
    // explicit user exit/reset boundary.
    g_background_publish_allowed = true;
    g_focus_publish_allowed = true;
    g_keyinput = 0x03ff;
    z1::activate_smith_yard_readiness_plugin();
    if (!enter_portal(&observe_cpu))
        return fail("background-fault fixture could not enter through native portal");
    if (!g_foreign_background || !g_foreign_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("background-fault fixture could not activate Zelda presentation");
    const auto* first_call_frozen_background = g_foreign_background;
    const auto* first_call_frozen_focus = g_foreign_focus;
    const unsigned writes_before_background_fault = g_bus_write_count;
    g_keyinput = 0x03ff;
    g_background_publish_allowed = false;
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (g_foreign_background != first_call_frozen_background ||
        g_foreign_focus != first_call_frozen_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_background_fault)
        return fail("first publication rejection exposed native Minish instead of freezing Zelda");
    g_background_publish_allowed = true;
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background != first_call_frozen_background ||
        g_foreign_focus != first_call_frozen_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_background_fault)
        return fail("first-call frozen Zelda session resumed mutation without explicit exit");
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (g_foreign_background || g_foreign_focus ||
        minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("explicit chord could not leave first-call-frozen Zelda presentation");
    z1::reset_smith_yard_readiness_plugin();
    // A provider record can be rejected after it reaches the host seam (for
    // example, an older/corrupt external save). That is also an active-session
    // failure, not a hidden Minish return. It freezes the prior pair and still
    // leaves the documented chord available to the player.
    g_keyinput = 0x03ff;
    z1::activate_smith_yard_readiness_plugin();
    if (!enter_portal(&observe_cpu))
        return fail("restore-fault fixture could not enter through native portal");
    g_keyinput = 0x03ff;  // Release the entering chord before testing exit.
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (!g_foreign_background || !g_foreign_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("restore-fault fixture could not activate Zelda presentation");
    const auto* restore_frozen_background = g_foreign_background;
    const auto* restore_frozen_focus = g_foreign_focus;
    const unsigned writes_before_restore_fault = g_bus_write_count;
    minish::foreign_world::ForeignWorldNativeState malformed_active_snapshot;
    malformed_active_snapshot.set_zelda1_presentation_active(true);
    minish::foreign_world::foreign_world_native_state().replace_after_provider_restore(
        std::move(malformed_active_snapshot));
    ++g_frame;
    (void)gba_mod_function_entry(0x03005F40u, 0, &observe_cpu);
    if (g_foreign_background != restore_frozen_background ||
        g_foreign_focus != restore_frozen_focus ||
        !minish::foreign_world::foreign_world_native_state().zelda1_presentation_active() ||
        g_bus_write_count != writes_before_restore_fault)
        return fail("invalid active provider restore exposed native Minish instead of freezing Zelda");
    g_keyinput = static_cast<std::uint16_t>(0x03ff & ~z1::kQaChord);
    ++g_frame;
    (void)gba_mod_function_entry(0x0805E5C0u, 1, &observe_cpu);
    if (g_foreign_background || g_foreign_focus ||
        minish::foreign_world::foreign_world_native_state().zelda1_presentation_active())
        return fail("explicit chord could not leave restore-frozen Zelda presentation");
    z1::reset_smith_yard_readiness_plugin();
    if (g_foreign_background || g_foreign_focus)
        return fail("plugin lifecycle reset did not clear foreign presentation");
    std::cout << "Smith-yard plugin actual-ROM mode/focus/CPU routing passed\n";
    return 0;

}
