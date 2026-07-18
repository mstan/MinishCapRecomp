#include "minish_extended_view.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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
constexpr std::uint32_t kPlayerEntity = 0x03001160u;
constexpr std::uint32_t kAuxPlayerEntities = 0x030011E8u;
constexpr std::uint32_t kEntities = 0x030015A0u;
constexpr int kFullMapStride = 128;
constexpr int kHudElementsOffset = 0x34;
constexpr int kHudElementSize = 0x20;
constexpr int kEntitySize = 0x88;

struct ScanlineState {
    gba::GbaBus* bus = nullptr;
    int screen_y = -1;
    std::uint32_t full_map[4]{};
    int camera_x = 0;
    int camera_y = 0;
    int ring_bias_x = 0;
    int ring_bias_y = 0;
    int room_width = 0;
    int room_height = 0;
    std::uint8_t hud_hide_flags = 0;
    bool message_active = false;
    bool cloud_overlay = false;
};

ScanlineState g_state;
gba::GbaBus* g_bus = nullptr;
int g_ring_bias_x = 0;
int g_ring_bias_y = 0;
// MC-WS-002 seam trace: resolver scores exposed for the per-frame trace row.
int g_bias_best_score = -1;
int g_bias_zero_score = -1;

// ── MC-WS-002 single-clock margin camera ─────────────────────────────────
// Measured defect (camtrace 2026-07-17): sampling the EWRAM camera at render
// time runs one game-tick ahead of the latched HOFS/VOFS on ~12% of walking
// frames — the margins visibly oscillate 1-2px against the always-latched
// center. Fix: anchor the absolute room camera from EWRAM once, then advance
// it every frame by the HARDWARE scroll deltas the center itself is composed
// from — margins and center then share one clock by construction. The game's
// 16px tile-ring rotation adds exact ±16 jumps to HOFS/VOFS; per-frame camera
// motion is far below 8px, so unwrapping deltas mod 16 recovers pure motion.
// Re-anchor whenever the unwrapped prediction and the EWRAM camera disagree
// by a full metatile (room load, door warp, cutscene camera snap).
// GBARECOMP_WS_CAM_EWRAM=1 restores the old render-time EWRAM sampling (A/B).
struct IntegratedCamera {
    bool anchored = false;
    int cam_x = 0;
    int cam_y = 0;
    int last_hofs = 0;
    int last_vofs = 0;
    int last_origin_x = 0;
    int last_origin_y = 0;
    bool used_ewram = false;  // what the last frame actually used (trace)
};
IntegratedCamera g_icam;

int unwrap16(int d) {  // signed mod-16 unwrap of a mod-512 scroll delta
    d &= 15;
    return d > 8 ? d - 16 : d;
}

int wrap512(int d) {  // signed mod-512 delta
    d &= 511;
    return d > 256 ? d - 512 : d;
}

struct ObjXCacheEntry {
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    int out_x = 0;
    int action = 0;
    std::uint32_t generation = 0;
};

std::array<ObjXCacheEntry, 128> g_obj_x_cache{};
std::uint32_t g_obj_x_cache_generation = 1;

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
        case 0x080B291Cu:  // ROM source of sub_080B2874's OBJ clip.
        case 0x03006690u:  // Live IWRAM copy of that ARM instruction.
            if (original != 0xF0u) return 0;
            *out_value = original + g_ws_extra_right;
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

std::uint16_t hardware_ring_entry(gba::GbaBus* bus, int bg,
                                  int hardware_x, int screen_y) {
    const std::uint16_t bgcnt = bus->io().read16(0x08u + bg * 2u);
    const int hofs = bus->io().read16(0x10u + bg * 4u) & 0x1FFu;
    const int vofs = bus->io().read16(0x12u + bg * 4u) & 0x1FFu;
    const int width_tiles = (bgcnt & 0x4000u) ? 64 : 32;
    const int height_tiles = (bgcnt & 0x8000u) ? 64 : 32;
    const int tile_x = ((hardware_x + hofs) & (width_tiles * 8 - 1)) >> 3;
    const int tile_y = ((screen_y + vofs) & (height_tiles * 8 - 1)) >> 3;
    const int block = (tile_x >> 5) +
        (tile_y >> 5) * (width_tiles >> 5);
    const std::uint32_t offset = ((bgcnt >> 8) & 0x1Fu) * 0x800u +
        static_cast<std::uint32_t>(block) * 0x800u +
        static_cast<std::uint32_t>(
            ((tile_y & 31) * 32 + (tile_x & 31)) * 2);
    const std::uint8_t* vram = bus->vram_ptr();
    return static_cast<std::uint16_t>(vram[offset]) |
        static_cast<std::uint16_t>(vram[offset + 1u] << 8);
}

void resolve_ring_bias(gba::GbaBus* bus) {
    // UpdateScrollVram rotates its 16px-metatile ring one VBlank behind the
    // new HOFS/VOFS phase. Rather than predicting that transient from CPU
    // timing, match a small set of authentic ring entries against the full
    // room map once per frame. This selects the exact block represented by
    // the latched PPU state and costs far less than one scanline's pixels.
    constexpr int biases[] = {0, -16, 16};
    constexpr int probes_x[] = {0, 47, 95, 143, 191, 239};
    constexpr int probes_y[] = {0, 31, 63, 95, 127, 159};
    int best_score = -1;
    int zero_score = -1;
    int best_x = 0;
    int best_y = 0;
    for (int bias_x : biases) {
        for (int bias_y : biases) {
            int score = 0;
            for (int bg = 0; bg < 4; ++bg) {
                const std::uint32_t full_map = g_state.full_map[bg];
                if (!full_map) continue;
                for (int screen_y : probes_y) {
                    const int room_y = g_state.camera_y + screen_y + bias_y;
                    if (room_y < 0 || room_y >= g_state.room_height) continue;
                    for (int hardware_x : probes_x) {
                        const int room_x =
                            g_state.camera_x + hardware_x + bias_x;
                        if (room_x < 0 || room_x >= g_state.room_width)
                            continue;
                        const std::uint32_t index = static_cast<std::uint32_t>(
                            (room_y >> 3) * kFullMapStride + (room_x >> 3));
                        if (rd16(bus, full_map + index * 2u) ==
                            hardware_ring_entry(
                                bus, bg, hardware_x, screen_y)) {
                            ++score;
                        }
                    }
                }
            }
            if (bias_x == 0 && bias_y == 0) zero_score = score;
            if (score > best_score) {
                best_score = score;
                best_x = bias_x;
                best_y = bias_y;
            }
        }
    }
    // Repeated/transparent scenery can produce weak accidental matches.
    // Require a clear multi-probe win before departing from the nominal map.
    const bool confident = best_score >= zero_score + 8;
    g_ring_bias_x = confident ? best_x : 0;
    g_ring_bias_y = confident ? best_y : 0;
    g_bias_best_score = best_score;
    g_bias_zero_score = zero_score;
}

// ── MC-WS-002 seam trace ─────────────────────────────────────────────────
// One CSV row per frame latch, armed by GBARECOMP_WS_CAMTRACE=<path>. Records
// the margin provider's EWRAM-derived camera next to the hardware HOFS/VOFS
// the center 240px is composed from, plus VCOUNT at latch time (WHEN in the
// frame the margin camera was sampled) and the ring-bias resolver decision.
// The margin/center seam is real iff, frame-over-frame, d(camera) diverges
// from d(HOFS/VOFS) or the bias flips while the camera moves smoothly.
void camtrace_frame(gba::GbaBus* bus, int scroll_x, int scroll_y,
                    int origin_x, int origin_y, int shake_x, int shake_y) {
    static std::FILE* f = nullptr;
    static bool tried = false;
    static unsigned long long n = 0;
    if (!tried) {
        tried = true;
        const char* p = std::getenv("GBARECOMP_WS_CAMTRACE");
        if (p && *p) {
            f = std::fopen(p, "w");
            if (f) {
                std::fprintf(f,
                    "n,vcount,cam_x,cam_y,scroll_x,scroll_y,origin_x,origin_y,"
                    "shake_x,shake_y,hofs0,vofs0,hofs1,vofs1,hofs2,vofs2,"
                    "hofs3,vofs3,bias_x,bias_y,bias_best,bias_zero,"
                    "room_w,room_h,maps,cammode\n");
            }
        }
    }
    if (!f) return;
    const unsigned vcount = bus->io().read16(0x006u);
    int hofs[4];
    int vofs[4];
    for (int bg = 0; bg < 4; ++bg) {
        hofs[bg] = bus->io().read16(0x10u + bg * 4u) & 0x1FF;
        vofs[bg] = bus->io().read16(0x12u + bg * 4u) & 0x1FF;
    }
    int maps = 0;
    for (int bg = 0; bg < 4; ++bg)
        if (g_state.full_map[bg]) maps |= 1 << bg;
    std::fprintf(f,
        "%llu,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d\n",
        n++, vcount, g_state.camera_x, g_state.camera_y, scroll_x, scroll_y,
        origin_x, origin_y, shake_x, shake_y,
        hofs[0], vofs[0], hofs[1], vofs[1], hofs[2], vofs[2],
        hofs[3], vofs[3],
        g_state.ring_bias_x, g_state.ring_bias_y,
        g_bias_best_score, g_bias_zero_score,
        g_state.room_width, g_state.room_height, maps,
        g_icam.used_ewram ? 1 : 0);
    std::fflush(f);
}

void refresh_scanline(gba::GbaBus* bus, int screen_y) {
    if (g_state.bus == bus && g_state.screen_y == screen_y) return;

    const bool new_frame = g_state.bus != bus || screen_y <= g_state.screen_y;
    if (!new_frame) {
        // Guest gameplay updates run during visible scanlines, but the GBA's
        // displayed ring/scroll registers remain latched until VBlank. Keep
        // the authored-margin camera and metadata latched for the same whole
        // frame instead of sampling changing EWRAM on every scanline.
        g_state.screen_y = screen_y;
        return;
    }

    if (++g_obj_x_cache_generation == 0) {
        g_obj_x_cache = {};
        g_obj_x_cache_generation = 1;
    }

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
    const int base_camera_x = scroll_x - origin_x;
    const int base_camera_y = scroll_y - origin_y;
    const int shake_x = static_cast<std::int8_t>(
        rd8(bus, kRoomControls + 0x24u));
    const int shake_y = static_cast<std::int8_t>(
        rd8(bus, kRoomControls + 0x25u));
    const int ewram_cam_x = base_camera_x + shake_x;
    const int ewram_cam_y = base_camera_y + shake_y;
    // Single-clock margin camera (see IntegratedCamera above): advance the
    // absolute room camera by the hardware scroll deltas the center is
    // composed from; the render-time EWRAM sample only anchors/re-anchors.
    static const bool use_ewram_cam = [] {
        const char* e = std::getenv("GBARECOMP_WS_CAM_EWRAM");
        return e && *e && *e != '0';
    }();
    const int ref_bg = bottom_bg >= 0 ? bottom_bg : top_bg;
    if (use_ewram_cam || ref_bg < 0) {
        g_state.camera_x = ewram_cam_x;
        g_state.camera_y = ewram_cam_y;
        g_icam.anchored = false;
        g_icam.used_ewram = true;
    } else {
        const int hofs = bus->io().read16(0x10u + ref_bg * 4u) & 0x1FF;
        const int vofs = bus->io().read16(0x12u + ref_bg * 4u) & 0x1FF;
        const bool room_changed =
            origin_x != g_icam.last_origin_x ||
            origin_y != g_icam.last_origin_y;
        if (g_icam.anchored && !room_changed) {
            g_icam.cam_x += unwrap16(wrap512(hofs - g_icam.last_hofs));
            g_icam.cam_y += unwrap16(wrap512(vofs - g_icam.last_vofs));
            // A fast pan (>8px/frame) breaks the mod-16 unwrap; the EWRAM
            // camera catches it within a metatile and re-anchors.
            if (std::abs(g_icam.cam_x - ewram_cam_x) > 16 ||
                std::abs(g_icam.cam_y - ewram_cam_y) > 16) {
                g_icam.cam_x = ewram_cam_x;
                g_icam.cam_y = ewram_cam_y;
            }
        } else {
            g_icam.cam_x = ewram_cam_x;
            g_icam.cam_y = ewram_cam_y;
            g_icam.anchored = true;
        }
        g_icam.last_hofs = hofs;
        g_icam.last_vofs = vofs;
        g_icam.last_origin_x = origin_x;
        g_icam.last_origin_y = origin_y;
        g_state.camera_x = g_icam.cam_x;
        g_state.camera_y = g_icam.cam_y;
        g_icam.used_ewram = false;
    }
    g_state.room_width = static_cast<int>(
        rd16(bus, kRoomControls + 0x1Eu));
    g_state.room_height = static_cast<int>(
        rd16(bus, kRoomControls + 0x20u));
    if (new_frame) resolve_ring_bias(bus);
    g_state.ring_bias_x = g_ring_bias_x;
    g_state.ring_bias_y = g_ring_bias_y;
    g_state.hud_hide_flags = rd8(bus, kHud + 1u);
    g_state.message_active = (rd8(bus, kMessage) & 0x7Fu) != 0;
    g_state.cloud_overlay = is_hyrule_cloud_overlay(bus);
    if (new_frame)
        camtrace_frame(bus, scroll_x, scroll_y, origin_x, origin_y,
                       shake_x, shake_y);
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

    const int room_x = g_state.camera_x + hardware_x + g_state.ring_bias_x;
    const int room_y = g_state.camera_y + screen_y + g_state.ring_bias_y;
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

bool match_world_entity_x(gba::GbaBus* bus, std::uint32_t entity,
                          int raw_x, int raw_y, int* best_score,
                          int* best_x) {
    const unsigned kind = rd8(bus, entity + 0x08u);
    const unsigned flags = rd8(bus, entity + 0x10u);
    const unsigned draw = rd8(bus, entity + 0x18u) & 3u;
    if (kind == 0 || kind > 9 || (flags & 0x10u) != 0 || draw == 0)
        return false;

    const int entity_x = static_cast<std::int16_t>(rd16(bus, entity + 0x2Eu));
    const int entity_y = static_cast<std::int16_t>(rd16(bus, entity + 0x32u));
    const int entity_z = static_cast<std::int16_t>(rd16(bus, entity + 0x36u));
    const int scroll_x = static_cast<std::int16_t>(
        rd16(bus, kRoomControls + 0x0Au));
    const int scroll_y = static_cast<std::int16_t>(
        rd16(bus, kRoomControls + 0x0Cu));
    const int offset_x = static_cast<std::int8_t>(rd8(bus, entity + 0x62u));
    const int offset_y = static_cast<std::int8_t>(rd8(bus, entity + 0x63u));
    const int expected_x = entity_x - scroll_x + offset_x;
    const int expected_y = entity_y - scroll_y - entity_z + offset_y;
    if (distance(raw_y, expected_y) > 64) return false;

    const int candidates[] = {raw_x, raw_x - 512};
    bool matched = false;
    for (int candidate : candidates) {
        const int dx = distance(candidate, expected_x);
        if (dx > 64) continue;
        const int score = dx + distance(raw_y, expected_y);
        if (score < *best_score) {
            *best_score = score;
            *best_x = candidate;
            matched = true;
        }
    }
    return matched;
}

extern "C" int minish_hud_obj_x_provider(int oam_index, std::uint16_t attr0,
                                          std::uint16_t attr1,
                                          std::uint16_t attr2, int* out_x) {
    gba::GbaBus* bus = active_game_bus();
    if (!bus || !out_x || (rd8(bus, kMessage) & 0x7Fu) != 0) return 0;

    ObjXCacheEntry* cache = oam_index >= 0 && oam_index < 128
        ? &g_obj_x_cache[static_cast<std::size_t>(oam_index)] : nullptr;
    if (cache && cache->generation == g_obj_x_cache_generation &&
        cache->attr0 == attr0 && cache->attr1 == attr1 &&
        cache->attr2 == attr2) {
        if (cache->action > 0) *out_x = cache->out_x;
        return cache->action;
    }
    auto finish = [&](int action, int x) {
        if (action > 0) *out_x = x;
        if (cache) {
            cache->attr0 = attr0;
            cache->attr1 = attr1;
            cache->attr2 = attr2;
            cache->out_x = x;
            cache->action = action;
            cache->generation = g_obj_x_cache_generation;
        }
        return action;
    };

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

        const int x = raw_x + (right_hud
            ? static_cast<int>(g_ws_extra_right)
            : -static_cast<int>(g_ws_extra_left));
        return finish(1, x);
    }

    // World OBJ X is stored as unsigned 9-bit OAM. Native hardware interprets
    // 256..511 as off-left, which aliases authored objects that are genuinely
    // at X >= 256 in the expanded right margin. Match OAM pieces to live world
    // entities and choose the signed/unwrapped coordinate nearest the entity's
    // actual camera-relative anchor. This also preserves negative left-margin
    // pieces without changing ordinary 0..255 sprites.
    int best_score = 0x7FFFFFFF;
    int best_x = raw_x;
    for (int index = 0; index < 8; ++index) {
        const std::uint32_t entity = index == 0
            ? kPlayerEntity
            : kAuxPlayerEntities + static_cast<std::uint32_t>(
                (index - 1) * kEntitySize);
        match_world_entity_x(
            bus, entity, raw_x, raw_y, &best_score, &best_x);
    }
    for (int index = 0; index < 72; ++index) {
        match_world_entity_x(
            bus, kEntities + static_cast<std::uint32_t>(index * kEntitySize),
            raw_x, raw_y, &best_score, &best_x);
    }
    if (best_score != 0x7FFFFFFF) {
        return finish(1, best_x);
    }
    return finish(0, 0);
}

}  // namespace

void install_extended_view(std::uint32_t, std::uint32_t) {
    g_state = {};
    g_ring_bias_x = 0;
    g_ring_bias_y = 0;
    g_bus = nullptr;
    g_obj_x_cache = {};
    g_obj_x_cache_generation = 1;
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
