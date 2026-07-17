#include "minish_extended_view.h"

#include <cstdint>
#include <cstdio>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" unsigned g_ws_active;
extern "C" unsigned g_ws_extra_left;
extern "C" unsigned g_ws_extra_right;

namespace minish {
namespace {

// Pinned USA-ROM data symbols imported from zeldaret/tmc. The two Special
// buffers are not hardware tilemaps: they are the game's complete rendered
// room layers (128x128 8px tiles). The original game copies only the visible
// 32x32 ring from these buffers to VRAM as the camera moves.
constexpr std::uint32_t kMapTop = 0x0200B650u;
constexpr std::uint32_t kMapBottom = 0x02025EB0u;
constexpr std::uint32_t kMapDataTopSpecial = 0x02002F00u;
constexpr std::uint32_t kMapDataBottomSpecial = 0x02019EE0u;
constexpr std::uint32_t kMessage = 0x02000050u;
constexpr std::uint32_t kHud = 0x0200AF00u;
constexpr std::uint32_t kRoomControls = 0x03000BF0u;
constexpr std::uint32_t kScreen = 0x03000F50u;
constexpr int kFullMapStride = 128;
constexpr int kHudElementsOffset = 0x34;
constexpr int kHudElementSize = 0x20;

struct ScanlineState {
    gba::GbaBus* bus = nullptr;
    int screen_y = -1;
    std::uint32_t full_map[4]{};
    int camera_x = 0;
    int camera_y = 0;
    int room_width = 0;
    int room_height = 0;
    std::uint8_t hud_hide_flags = 0;
    bool message_active = false;
    bool cloud_overlay = false;
};

ScanlineState g_state;
gba::GbaBus* g_bus = nullptr;

// Horizontal visibility helpers in the original game use the native 240px
// viewport as an immediate/literal. These exact sites retain their original
// constants unless the adaptive view has actually opened margins.
extern "C" int minish_view_alu_immediate(std::uint32_t pc,
                                           std::uint32_t original,
                                           std::uint32_t* out_value) {
    if (!out_value || !g_ws_active) return 0;

    switch (pc) {
        case 0x080040B2u:  // CheckOnScreen: preserve its 63px left guard.
            if (original != 0x3Fu) return 0;
            *out_value = original + g_ws_extra_left;
            return 1;
        case 0x080562DAu:  // CheckRegionOnScreen: right viewport edge.
            if (original != 0xF0u) return 0;
            *out_value = original + g_ws_extra_right;
            return 1;
        case 0x080562E8u:  // CheckRegionOnScreen: total comparison span.
            if (original != 0xF0u) return 0;
            *out_value = original + g_ws_extra_left + g_ws_extra_right;
            return 1;
        default:
            return 0;
    }
}

extern "C" int minish_view_rom_read32(std::uint32_t address,
                                        std::uint32_t original,
                                        std::uint32_t* out_value) {
    if (!out_value || !g_ws_active || address != 0x0800436Cu ||
        original != 0x0000016Eu) {
        return 0;
    }

    // CheckOnScreen's upper bound includes the original 63px guard on both
    // sides. Widen only its horizontal span; vertical culling is unchanged.
    *out_value = original + g_ws_extra_left + g_ws_extra_right;
    return 1;
}

gba::GbaBus* active_game_bus() {
    if (!g_bus) g_bus = gbarecomp::active_bus();
    return g_bus;
}

std::uint8_t rd8(gba::GbaBus* bus, std::uint32_t addr) {
    if (addr >= 0x02000000u && addr < 0x02040000u)
        return bus->ewram_ptr()[addr - 0x02000000u];
    if (addr >= 0x03000000u && addr < 0x03008000u)
        return bus->iwram_ptr()[addr - 0x03000000u];
    return 0;
}

std::uint16_t rd16(gba::GbaBus* bus, std::uint32_t addr) {
    return static_cast<std::uint16_t>(rd8(bus, addr)) |
           static_cast<std::uint16_t>(rd8(bus, addr + 1u) << 8);
}

std::uint32_t rd32(gba::GbaBus* bus, std::uint32_t addr) {
    return static_cast<std::uint32_t>(rd16(bus, addr)) |
           (static_cast<std::uint32_t>(rd16(bus, addr + 2u)) << 16);
}

int bg_for_settings(std::uint32_t settings) {
    // Screen::bg0/bg1/bg2/bg3 are twelve-byte records at these offsets.
    constexpr std::uint32_t offsets[] = {0x08u, 0x14u, 0x20u, 0x2Cu};
    for (int bg = 0; bg < 4; ++bg) {
        if (settings == kScreen + offsets[bg]) return bg;
    }
    return -1;
}

bool is_hyrule_cloud_overlay(gba::GbaBus* bus) {
    // CloudOverlayManager configures BG3 as a screen-block-30, char-block-1
    // alpha-blended texture and scrolls it diagonally. Its 256px map is meant
    // to tile; unlike a room layer, retaining the hardware wrap is authentic.
    const std::uint16_t bg_control = rd16(bus, kScreen + 0x2Cu);
    const std::uint16_t blend_control = rd16(bus, kScreen + 0x66u);
    return (bg_control & 0x1F0Cu) == 0x1E04u &&
           (blend_control & 0x00C8u) == 0x0048u;
}

void refresh_scanline(gba::GbaBus* bus, int screen_y) {
    if (g_state.bus == bus && g_state.screen_y == screen_y) return;

    g_state = {};
    g_state.bus = bus;
    g_state.screen_y = screen_y;

    const int bottom_bg = bg_for_settings(rd32(bus, kMapBottom));
    const int top_bg = bg_for_settings(rd32(bus, kMapTop));
    if (bottom_bg >= 0) g_state.full_map[bottom_bg] = kMapDataBottomSpecial;
    if (top_bg >= 0) g_state.full_map[top_bg] = kMapDataTopSpecial;

    const int origin_x = static_cast<int>(rd16(bus, kRoomControls + 0x06u));
    const int origin_y = static_cast<int>(rd16(bus, kRoomControls + 0x08u));
    const int scroll_x = static_cast<std::int16_t>(
        rd16(bus, kRoomControls + 0x0Au));
    const int scroll_y = static_cast<std::int16_t>(
        rd16(bus, kRoomControls + 0x0Cu));
    const int shake_x = static_cast<std::int8_t>(
        rd8(bus, kRoomControls + 0x24u));
    const int shake_y = static_cast<std::int8_t>(
        rd8(bus, kRoomControls + 0x25u));
    g_state.camera_x = scroll_x - origin_x + shake_x;
    g_state.camera_y = scroll_y - origin_y + shake_y;
    g_state.room_width = static_cast<int>(
        rd16(bus, kRoomControls + 0x1Eu));
    g_state.room_height = static_cast<int>(
        rd16(bus, kRoomControls + 0x20u));
    g_state.hud_hide_flags = rd8(bus, kHud + 1u);
    g_state.message_active = (rd8(bus, kMessage) & 0x7Fu) != 0;
    g_state.cloud_overlay = is_hyrule_cloud_overlay(bus);
}

extern "C" int minish_tilemap_provider(int bg, int hardware_x, int screen_y,
                                        std::uint16_t* out_entry) {
    gba::GbaBus* bus = active_game_bus();
    if (!bus || !out_entry || bg < 0 || bg > 3) return 0;
    refresh_scanline(bus, screen_y);

    const std::uint32_t full_map = g_state.full_map[bg];
    if (!full_map) {
        if (bg == 3 && g_state.cloud_overlay)
            return gba::kWsTilemapKeepWrapped;
        // UI and other non-room layers have no authored continuation. Returning
        // false makes only their expanded-margin pixels transparent instead of
        // repeating the 256px hardware ring.
        return gba::kWsTilemapUnavailable;
    }

    // Fail closed while a room is absent/loading or if guest state is outside
    // the format's 64x64-metatile maximum.
    if (g_state.room_width <= 0 || g_state.room_width > 1024 ||
        g_state.room_height <= 0 || g_state.room_height > 1024) {
        return gba::kWsTilemapUnavailable;
    }

    const int room_x = g_state.camera_x + hardware_x;
    const int room_y = g_state.camera_y + screen_y;
    if (room_x < 0 || room_y < 0 ||
        room_x >= g_state.room_width || room_y >= g_state.room_height) {
        return gba::kWsTilemapUnavailable;
    }

    const std::uint32_t index = static_cast<std::uint32_t>(
        (room_y >> 3) * kFullMapStride + (room_x >> 3));
    *out_entry = rd16(bus, full_map + index * 2u);
    return gba::kWsTilemapReplace;
}

extern "C" int minish_hud_bg_x_provider(int bg, int output_x, int screen_y,
                                         int* out_hardware_x) {
    if (!out_hardware_x || bg != 0) return 0;
    if (screen_y < 8 || (screen_y >= 32 && screen_y < 128)) return 0;
    gba::GbaBus* bus = active_game_bus();
    if (!bus) return 0;
    refresh_scanline(bus, screen_y);
    if (g_state.message_active) return 0;

    const int left = static_cast<int>(g_ws_extra_left);
    const int right = static_cast<int>(g_ws_extra_right);
    const int extra = left + right;
    const int hide_flags = g_state.hud_hide_flags;
    const bool hearts = (hide_flags & 0x10) == 0 &&
        screen_y >= 8 && screen_y < 32;
    const bool keys = (hide_flags & 0x80) == 0 &&
        screen_y >= 128 && screen_y < 144;
    const bool rupees = (hide_flags & 0x40) == 0 &&
        screen_y >= 144 && screen_y < 160;

    if (output_x < left) {
        // Hearts and the sword charge bar occupy BG0 rows 1..3, columns
        // 0..11. Remap those native samples to the physical left edge.
        const int native_x = output_x;
        if (hearts && native_x < 96) {
            *out_hardware_x = native_x;
            return 1;
        }
    } else if (output_x >= left + 240) {
        // Dungeon keys use rows 16..17; rupees use rows 18..19. Preserve
        // their distance from the physical right edge at every view width.
        const int native_x = output_x - extra;
        if ((keys && native_x >= 200 && native_x < 232) ||
            (rupees && native_x >= 192 && native_x < 232)) {
            *out_hardware_x = native_x;
            return 1;
        }
    } else {
        // The expanded renderer normally centers the complete native BG0.
        // Once a HUD region has an edge-anchored copy, suppress that centered
        // source region so the player never sees both versions at once.
        const int native_x = output_x - left;
        if ((left > 0 && hearts && native_x >= 0 && native_x < 96) ||
            (right > 0 && keys && native_x >= 200 && native_x < 232) ||
            (right > 0 && rupees && native_x >= 192 && native_x < 232)) {
            return -1;
        }
    }
    return 0;
}

int signed_oam_y(std::uint16_t attr0) {
    int y = attr0 & 0xFFu;
    return y >= 160 ? y - 256 : y;
}

int distance(int a, int b) {
    const int d = a - b;
    return d < 0 ? -d : d;
}

extern "C" int minish_hud_obj_x_provider(int, std::uint16_t attr0,
                                          std::uint16_t attr1,
                                          std::uint16_t attr2, int* out_x) {
    gba::GbaBus* bus = active_game_bus();
    if (!bus || !out_x || (rd8(bus, kMessage) & 0x7Fu) != 0) return 0;

    const int raw_x = attr1 & 0x1FFu;
    const int raw_y = signed_oam_y(attr0);
    const unsigned tile = attr2 & 0x3FFu;
    for (int index = 0; index < 24; ++index) {
        const std::uint32_t element = kHud + kHudElementsOffset +
            static_cast<std::uint32_t>(index * kHudElementSize);
        const std::uint8_t flags = rd8(bus, element);
        if ((flags & 3u) != 3u) continue;  // allocated and currently drawn

        const unsigned type = rd8(bus, element + 1u);
        const bool right_hud = type <= 5u || type == 9u || type == 10u;
        const bool left_hud = type >= 6u && type <= 8u;
        if (!right_hud && !left_hud) continue;

        const unsigned base_tile = rd16(bus, element + 0x1Au) & 0x3FFu;
        const unsigned num_tiles = rd8(bus, element + 0x19u);
        // The three button elements use a shared dynamic tile allocation and
        // report numTiles=0 even while DrawDirect emits their OAM pieces.
        // Position matching keeps the conservative fallback span tied to the
        // active element instead of treating every early OAM entry as HUD.
        const unsigned tile_span = num_tiles != 0 ? num_tiles : 0x20u;
        if (((tile - base_tile) & 0x3FFu) >= tile_span)
            continue;

        const int element_x = static_cast<std::int16_t>(
            rd16(bus, element + 0x0Cu));
        const int element_y = static_cast<std::int16_t>(
            rd16(bus, element + 0x0Eu));
        if (distance(raw_x, element_x) > 32 ||
            distance(raw_y, element_y) > 32) {
            continue;
        }

        *out_x = raw_x + (right_hud
            ? static_cast<int>(g_ws_extra_right)
            : -static_cast<int>(g_ws_extra_left));
        return 1;
    }
    return 0;
}

}  // namespace

void install_extended_view(std::uint32_t, std::uint32_t) {
    g_state = {};
    g_bus = nullptr;
    gba::g_ws_tilemap_provider = &minish_tilemap_provider;
    gba::g_ws_authored_margin_layers = 1;
    gba::g_ws_bg_x_provider = &minish_hud_bg_x_provider;
    gba::g_ws_bg_x_provider_layers = 1u << 0;
    gba::g_ws_obj_attr_x_provider = &minish_hud_obj_x_provider;
    g_runtime_thumb_alu_imm_override = &minish_view_alu_immediate;
    gba::g_rom_read32_override = &minish_view_rom_read32;
    std::fprintf(stderr,
        "[minish:view] authentic room-map margin source enabled\n");
}

}  // namespace minish
