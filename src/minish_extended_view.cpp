#include "minish_extended_view.h"

#include <cstdint>
#include <cstdio>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_bus_bridge.h"

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
constexpr std::uint32_t kRoomControls = 0x03000BF0u;
constexpr std::uint32_t kScreen = 0x03000F50u;
constexpr int kFullMapStride = 128;

struct ScanlineState {
    gba::GbaBus* bus = nullptr;
    int screen_y = -1;
    std::uint32_t full_map[4]{};
    int camera_x = 0;
    int camera_y = 0;
    int room_width = 0;
    int room_height = 0;
};

ScanlineState g_state;

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
}

extern "C" int minish_tilemap_provider(int bg, int hardware_x, int screen_y,
                                        std::uint16_t* out_entry) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !out_entry || bg < 0 || bg > 3) return 0;
    refresh_scanline(bus, screen_y);

    const std::uint32_t full_map = g_state.full_map[bg];
    if (!full_map) {
        // UI and other non-room layers have no authored continuation. Returning
        // false makes only their expanded-margin pixels transparent instead of
        // repeating the 256px hardware ring.
        return 0;
    }

    // Fail closed while a room is absent/loading or if guest state is outside
    // the format's 64x64-metatile maximum.
    if (g_state.room_width <= 0 || g_state.room_width > 1024 ||
        g_state.room_height <= 0 || g_state.room_height > 1024) {
        return 0;
    }

    const int room_x = g_state.camera_x + hardware_x;
    const int room_y = g_state.camera_y + screen_y;
    if (room_x < 0 || room_y < 0 ||
        room_x >= g_state.room_width || room_y >= g_state.room_height) {
        return 0;
    }

    const std::uint32_t index = static_cast<std::uint32_t>(
        (room_y >> 3) * kFullMapStride + (room_x >> 3));
    *out_entry = rd16(bus, full_map + index * 2u);
    return 1;
}

}  // namespace

void install_extended_view(std::uint32_t, std::uint32_t) {
    g_state = {};
    gba::g_ws_tilemap_provider = &minish_tilemap_provider;
    gba::g_ws_authored_margin_layers = 1;
    std::fprintf(stderr,
        "[minish:view] authentic room-map margin source enabled\n");
}

}  // namespace minish
