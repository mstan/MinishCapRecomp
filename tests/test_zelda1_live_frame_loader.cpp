#include "foreign_worlds/zelda1/zelda1_live_frame_loader.h"
#include "foreign_worlds/zelda1/zelda1_overworld_session.h"
#include "foreign_worlds/zelda1/zelda1_octorok_runtime.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

namespace z1 = minish::foreign_world::zelda1;

namespace {

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

bool write_file(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool write_frame_ppm(const std::filesystem::path& path, const z1::OwFramebuffer& frame) {
    std::ofstream ppm(path, std::ios::binary);
    if (!ppm) return false;
    ppm << "P6\n240 160\n255\n";
    for (const auto pixel : frame) {
        const unsigned r = pixel & 31, g = (pixel >> 5) & 31, b = (pixel >> 10) & 31;
        ppm.put(static_cast<char>((r << 3) | (r >> 2)));
        ppm.put(static_cast<char>((g << 3) | (g >> 2)));
        ppm.put(static_cast<char>((b << 3) | (b >> 2)));
    }
    return static_cast<bool>(ppm);
}

std::vector<std::uint8_t> synthetic_prg0_ines() {
    std::vector<std::uint8_t> bytes(16 + 128 * 1024);
    constexpr std::array<std::uint8_t, 16> header{{
        'N', 'E', 'S', 0x1a, 8, 0, 0x12, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    }};
    std::copy(header.begin(), header.end(), bytes.begin());
    return bytes;
}

std::uint64_t framebuffer_hash(const z1::OwFramebuffer& frame) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto pixel : frame) { hash ^= pixel; hash *= 1099511628211ull; }
    return hash;
}

bool same_rectangle(const z1::OwFramebuffer& first, const z1::OwFramebuffer& second,
                    unsigned x, unsigned y, unsigned width, unsigned height) {
    for (unsigned py = y; py < y + height; ++py)
        for (unsigned px = x; px < x + width; ++px)
            if (first[py * z1::kOwRenderWidth + px] !=
                second[py * z1::kOwRenderWidth + px])
                return false;
    return true;
}

bool footprint_walkable(const z1::OwRoomGeometry& geometry, std::uint8_t x, std::uint8_t y) {
    if (x > z1::kOwRenderWidth - z1::Zelda1OverworldSession::kLinkFootprintWidth ||
        y > z1::kOwRenderHeight - z1::Zelda1OverworldSession::kLinkFootprintHeight) return false;
    for (std::size_t py = y; py < static_cast<std::size_t>(y) + z1::Zelda1OverworldSession::kLinkFootprintHeight; ++py)
        for (std::size_t px = x; px < static_cast<std::size_t>(x) + z1::Zelda1OverworldSession::kLinkFootprintWidth; ++px)
            if (geometry.at(px, py) != z1::OwGeometryClass::kWalkable) return false;
    return true;
}

bool move_to(z1::Zelda1OverworldSession* session, std::uint8_t target_x, std::uint8_t target_y) {
    if (!session || !footprint_walkable(session->geometry(), target_x, target_y)) return false;
    constexpr std::size_t kWidth = z1::kOwRenderWidth - z1::Zelda1OverworldSession::kLinkFootprintWidth + 1;
    constexpr std::size_t kHeight = z1::kOwRenderHeight - z1::Zelda1OverworldSession::kLinkFootprintHeight + 1;
    const auto index = [](std::uint8_t x, std::uint8_t y) { return static_cast<std::size_t>(y) * kWidth + x; };
    const auto start = session->position();
    std::vector<int> previous(kWidth * kHeight, -2);
    std::queue<std::pair<std::uint8_t, std::uint8_t>> pending;
    previous[index(start.x, start.y)] = -1;
    pending.push({start.x, start.y});
    constexpr std::array<std::pair<int, int>, 4> kSteps{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
    while (!pending.empty() && previous[index(target_x, target_y)] == -2) {
        const auto [x, y] = pending.front(); pending.pop();
        for (const auto [dx, dy] : kSteps) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= static_cast<int>(kWidth) || ny >= static_cast<int>(kHeight) ||
                previous[index(static_cast<std::uint8_t>(nx), static_cast<std::uint8_t>(ny))] != -2 ||
                !footprint_walkable(session->geometry(), static_cast<std::uint8_t>(nx), static_cast<std::uint8_t>(ny))) continue;
            previous[index(static_cast<std::uint8_t>(nx), static_cast<std::uint8_t>(ny))] = static_cast<int>(index(x, y));
            pending.push({static_cast<std::uint8_t>(nx), static_cast<std::uint8_t>(ny)});
        }
    }
    const auto target = index(target_x, target_y);
    if (previous[target] == -2) return false;
    std::vector<std::size_t> path;
    for (auto cursor = target; previous[cursor] != -1; cursor = static_cast<std::size_t>(previous[cursor])) path.push_back(cursor);
    std::reverse(path.begin(), path.end());
    for (const auto point : path) {
        const auto x = static_cast<std::uint8_t>(point % kWidth);
        const auto y = static_cast<std::uint8_t>(point / kWidth);
        const auto current = session->position();
        if (session->move_by(static_cast<std::int16_t>(x - current.x), static_cast<std::int16_t>(y - current.y)) !=
            z1::OverworldSessionMoveResult::kMoved) return false;
    }
    return true;
}

// Mirrors the live LinearMoveDirectionOLD adapter's source movement policy:
// decompose a host delta into one-pixel cardinal probes and test source warp
// hotspots after each successful probe.  This regression protects exact
// Zelda-1 entrances from a fixed-speed Minish two-pixel movement update.
bool move_overworld_delta_with_entry_checks(z1::Zelda1OverworldSession* session,
                                            std::int16_t delta_x,
                                            std::int16_t delta_y) {
    if (!session || session->in_cave()) return false;
    const auto move_axis = [&](std::int16_t amount, bool horizontal) {
        if (amount == 0) return true;
        const std::int16_t step = amount < 0 ? -1 : 1;
        const unsigned count = static_cast<unsigned>(amount < 0 ? -amount : amount);
        for (unsigned pixel = 0; pixel < count; ++pixel) {
            const auto movement = session->move_by(horizontal ? step : 0,
                                                   horizontal ? 0 : step);
            if (movement != z1::OverworldSessionMoveResult::kMoved) return false;
            if (session->try_enter_cave() == z1::OverworldSessionCaveResult::kEntered)
                return false;
        }
        return true;
    };
    if (!move_axis(delta_x, true) || session->in_cave()) return session->in_cave();
    (void)move_axis(delta_y, false);
    return session->in_cave();
}

bool cross_at_matching_opening(z1::Zelda1OverworldSession* session, z1::OwScreenEdge edge) {
    if (!session) return false;
    std::uint8_t neighbor = 0;
    if (!z1::overworld_neighbor(session->position().room_id, edge, &neighbor)) return false;
    // Stage an exact source grid point one pixel before a PlayerScreenEdge-
    // Bounds value. This tests raw edge semantics without the old cropped
    // opening-search policy (which cannot represent north/west collars).
    auto state = session->serialize();
    std::int16_t x = 0x80, y = 0x9d, dx = 0, dy = 0;
    switch (edge) {
    case z1::OwScreenEdge::kNorth: y = 0x3e; dy = -1; break;
    case z1::OwScreenEdge::kSouth: y = 0xdc; dy = 1; break;
    case z1::OwScreenEdge::kWest: x = 1; dx = -1; break;
    case z1::OwScreenEdge::kEast: x = 0xef; dx = 1; break;
    }
    state[7] = static_cast<std::uint8_t>(x); state[8] = 0;
    state[9] = static_cast<std::uint8_t>(y); state[10] = 0;
    if (!session->restore(state)) return false;
    const auto result = session->move_by(dx, dy);
    return result == z1::OverworldSessionMoveResult::kCrossedRoom &&
           session->position().room_id == neighbor;
}

bool can_prove_solid_block(z1::Zelda1OverworldSession* session) {
    if (!session) return false;
    const auto saved = session->serialize();
    // Set up a source-grid point one pixel before a proven Z_07 right probe
    // into a solid final tile. The host must stop on the aligned sample, not
    // bypass it because the starting presentation coordinate is unaligned.
    const auto& tiles = session->geometry().final_tiles;
    for (unsigned y = 0x45; y < 0xdd; y += 8) for (unsigned x = 8; x < 0xe0; x += 8) {
        const unsigned probe_x = x + 0x10, probe_y = y + 0x0b;
        if (probe_x >= 256 || probe_y >= 0xf0) continue;
        const auto tile = tiles[(probe_y - 0x40) / 8 * z1::kOwPlayfieldTileWidth + probe_x / 8];
        if (z1::ow_final_tile_is_walkable(tile)) continue;
        auto candidate = saved;
        candidate[7] = static_cast<std::uint8_t>(x - 1); candidate[8] = 0;
        candidate[9] = static_cast<std::uint8_t>(y); candidate[10] = 0;
        if (!session->restore(candidate) ||
            session->move_by(1, 0) != z1::OverworldSessionMoveResult::kBlocked ||
            session->source_position().obj_x != static_cast<std::int16_t>(x - 1) ||
            !session->restore(saved)) return false;
        return true;
    }
    return false;
}

int test_synthetic_rejections() {
    const auto directory = std::filesystem::current_path() / "build";
    std::filesystem::create_directories(directory);
    const auto good_path = directory / "zelda1_live_loader_valid_shape.nes";
    const auto bad_header_path = directory / "zelda1_live_loader_bad_header.nes";
    const auto bad_size_path = directory / "zelda1_live_loader_bad_size.nes";
    auto valid = synthetic_prg0_ines();
    if (!write_file(good_path, valid)) return fail("could not write synthetic iNES input");

    z1::Zelda1LiveFrameLoader loader;
    std::string error;
    if (!loader.load_hash_validated_ines(good_path, &error) || !loader.loaded() ||
        !loader.first_quest_data())
        return fail("canonical synthetic iNES shape was rejected: " + error);
    const auto stable_frame = loader.framebuffer();

    auto bad_header = valid;
    bad_header[0] = 'X';
    if (!write_file(bad_header_path, bad_header) ||
        loader.load_hash_validated_ines(bad_header_path, &error) || error.empty() ||
        !loader.loaded() || loader.framebuffer() != stable_frame)
        return fail("bad iNES header was accepted or changed live loader state");

    valid.pop_back();
    if (!write_file(bad_size_path, valid) ||
        loader.load_hash_validated_ines(bad_size_path, &error) || error.empty() ||
        !loader.loaded() || loader.framebuffer() != stable_frame)
        return fail("bad iNES size was accepted or changed live loader state");
    return 0;
}

int test_actual_ines_path(const char* path) {
    z1::Zelda1LiveFrameLoader loader;
    std::string error;
    if (!loader.load_and_render_hash_validated_ines(path, 0x77, &error))
        return fail("actual hash-validated PRG0 iNES did not load/render: " + error);
    z1::OverworldRoomView start_room{};
    z1::OverworldCaveItemsView start_cave_items{};
    if (!loader.first_quest_data() ||
        !loader.first_quest_data()->overworld_room(7, 7, &start_room, &error) ||
        !loader.first_quest_data()->overworld_cave_items(start_room, &start_cave_items, &error) ||
        start_cave_items.source_cave_slot != 0 ||
        start_cave_items.raw_item_ids != std::array<std::uint8_t, 3>{{0x3f, 0x01, 0x7f}})
        return fail("actual OW77 cave-item record was not source-decoded: " + error);

    std::uint64_t hash = 1469598103934665603ull;
    for (const auto pixel : loader.framebuffer()) {
        hash ^= pixel;
        hash *= 1099511628211ull;
    }
    if (hash != 10743561982003409067ull)
        return fail("actual PRG0 OW77 framebuffer hash changed: " + std::to_string(hash));
    if (!write_frame_ppm("build/zelda1_live_loader_ow77_validation.ppm", loader.framebuffer()))
        return fail("could not create live-loader OW77 QA artifact");
    { // Actual-ROM bounded OW66 source roster/combat/persistence QA.
        z1::Ow66OctorokRuntime octoroks;
        if (!octoroks.initialize(*loader.first_quest_data(), 0))
            return fail("actual OW66 red-Octorok source roster did not initialize");
        if (!loader.render_overworld_room(0x66, &error)) return fail("OW66 terrain render failed: " + error);
        auto before = loader.framebuffer(); octoroks.render_qa_markers(&before);
        if (!write_frame_ppm("build/zelda1_ow66_octorok_before.ppm", before)) return fail("OW66 before artifact failed");
        octoroks.tick();
        std::uint8_t contact = 0;
        if (!octoroks.contact(0, octoroks.actors()[0].x, octoroks.actors()[0].y, &contact) || contact != 1 ||
            !octoroks.sword_hit(0, 3) || !octoroks.actors()[0].defeated || !octoroks.actors()[0].pending_drop)
            return fail("OW66 Octorok contact/hit/defeat contract failed");
        auto combat = loader.framebuffer(); octoroks.render_qa_markers(&combat);
        if (!write_frame_ppm("build/zelda1_ow66_octorok_combat.ppm", combat)) return fail("OW66 combat artifact failed");
        auto drop = loader.framebuffer(); octoroks.render_qa_markers(&drop);
        if (!write_frame_ppm("build/zelda1_ow66_octorok_drop.ppm", drop) || !octoroks.collect_drop(0)) return fail("OW66 drop artifact failed");
        auto defeat = loader.framebuffer(); octoroks.render_qa_markers(&defeat);
        if (!write_frame_ppm("build/zelda1_ow66_octorok_defeat.ppm", defeat)) return fail("OW66 defeat artifact failed");
        const auto saved = octoroks.serialize(); auto corrupt = saved; corrupt[29] = 1;
        if (octoroks.restore(corrupt) || octoroks.serialize() != saved || !octoroks.restore(saved))
            return fail("Octorok persistence corruption/nonmutation failed");
        if (!loader.render_overworld_room(0x77, &error)) return fail("could not restore OW77 after Octorok QA");
    }

    const auto stable_frame = loader.framebuffer();
    if (loader.load_and_render_hash_validated_ines(path, 0x80, &error) || error.empty() ||
        loader.framebuffer() != stable_frame)
        return fail("failed load-and-render changed the stable framebuffer");
    if (loader.render_overworld_room(0x80, &error) || error.empty() ||
        loader.framebuffer() != stable_frame)
        return fail("failed room render changed the stable framebuffer");
    z1::Zelda1OverworldSession session;
    if (!session.load_hash_validated_ines(path, z1::Zelda1OverworldSession::kInitialRoom, &error) || !session.loaded() ||
        session.position().room_id != 0x77 || session.position().x != 120 ||
        session.position().y != 85 || session.framebuffer() != stable_frame)
        return fail("OW session did not atomically initialize from the live loader: " + error);
    const auto initial_state = session.serialize();
    if (!z1::Zelda1OverworldSession::validate_serialized(initial_state) ||
        !can_prove_solid_block(&session) || session.position().room_id != 0x77 ||
        session.framebuffer() != stable_frame)
        return fail("OW session did not reject a solid final-tile footprint atomically");
    // Z_05:HandleWarpOW accepts the $24 mouth only when the source hotspot is
    // grid-aligned. The virtual target (56,21) converts to ObjX=$40,
    // ObjY=$5D: InitMode2's source cave-walk-out start coordinate. Starting
    // one pixel west and supplying a two-pixel host delta must enter at the
    // first probe rather than stepping over the exact hotspot.
    struct PortalCrossingProbe { std::uint8_t x, y; std::int16_t dx, dy; };
    constexpr std::array<PortalCrossingProbe, 4> kPortalCrossingProbes{{
        {54, 21, 2, 0}, {58, 21, -2, 0}, {56, 19, 0, 2}, {56, 23, 0, -2},
    }};
    bool crossed_cave_hotspot = false;
    for (const auto probe : kPortalCrossingProbes) {
        if (!session.restore(initial_state, &error))
            return fail("could not reset OW77 before two-pixel portal probe: " + error);
        if (move_to(&session, probe.x, probe.y) &&
            move_overworld_delta_with_entry_checks(&session, probe.dx, probe.dy) &&
            session.in_cave()) {
            crossed_cave_hotspot = true;
            break;
        }
    }
    if (!crossed_cave_hotspot || !session.restore(initial_state, &error) ||
        !move_to(&session, 56, 21) ||
        session.try_enter_cave() != z1::OverworldSessionCaveResult::kEntered ||
        !session.in_cave() || session.area() != z1::OverworldSessionArea::kCave ||
        session.cave_index() != 16 ||
        session.move_by(1, 0) != z1::OverworldSessionMoveResult::kBlocked)
        return fail("OW77 source cave mouth did not enter the normal-cave state");
    const auto cave_before_sword = session.framebuffer();
    if (framebuffer_hash(cave_before_sword) != 15688873919820252475ull ||
        !write_frame_ppm("build/zelda1_start_sword_cave_before.ppm", cave_before_sword))
        return fail("start sword cave did not produce its pinned source framebuffer");
    // InitCave creates the Old Man and both fires before Zelda's first
    // textbox is acknowledged. The static source renderer therefore has no
    // before/after-dialogue visual difference.
    std::vector<std::uint8_t> cave_patterns;
    z1::OwFramebuffer cave_dialogue_complete{};
    if (!z1::copy_overworld_background_from_verified_prg(*loader.first_quest_data(),
                                                         &cave_patterns, &error) ||
        !z1::render_first_quest_start_cave(*loader.first_quest_data(), cave_patterns,
                                           {false, true}, &cave_dialogue_complete) ||
        cave_dialogue_complete != cave_before_sword ||
        !write_frame_ppm("build/zelda1_start_sword_cave_dialogue_complete.ppm",
                         cave_dialogue_complete))
        return fail("source cave dialogue acknowledgement changed static sprites: " + error);
    const auto cave_entry_state = session.serialize();
    const auto cave_entry_position = session.cave_source_position();
    const auto cave_entry_presentation = session.cave_presentation_position();
    if (!cave_entry_position || !cave_entry_presentation || cave_entry_position->x != 0x70 ||
        cave_entry_position->y != 0xad || cave_entry_presentation->x != 0x68 ||
        cave_entry_presentation->y != 0x65 || !session.cave_entry_settled() ||
        session.cave_dialogue_acknowledged() ||
        session.move_cave_one(z1::CaveDirection::kDown) !=
            z1::StartCaveMoveResult::kDialogueBlocked ||
        !session.restore(cave_entry_state, &error))
        return fail("start cave did not settle, expose mapped position, or preserve dialogue gate");
    minish::foreign_world::InventoryCore inventory;
    if (!session.acknowledge_cave_dialogue() ||
        session.move_cave_by(0, static_cast<std::int16_t>(0x98 - 0xad)) !=
            z1::StartCaveMoveResult::kMoved ||
        session.move_cave_by(0x78 - 0x70, 0) != z1::StartCaveMoveResult::kMoved)
        return fail("cave dialogue acknowledgement or source navigation changed");
    const minish::foreign_world::CrossWorldItemId wooden_sword{
        minish::foreign_world::WorldId::Zelda1, 0x01};
    minish::foreign_world::InventoryCore rejected_inventory;
    const minish::foreign_world::ItemTraits conflicting_traits{
        2, {minish::foreign_world::WorldId::Zelda1, 0x02},
        minish::foreign_world::ItemUseKind::Equip,
        minish::foreign_world::ResourcePoolProvenance::None};
    if (!rejected_inventory.set_item(wooden_sword,
                                     minish::foreign_world::OwnershipFlags::Owned,
                                     minish::foreign_world::Capability::Sword,
                                     conflicting_traits, &error))
        return fail("could not construct atomic-pickup rejection fixture: " + error);
    const auto before_rejected_pickup = session.serialize();
    const auto before_rejected_inventory = rejected_inventory.serialize();
    if (session.try_take_start_sword(&rejected_inventory, &error) !=
            z1::OverworldSessionSwordResult::kInventoryRejected ||
        session.serialize() != before_rejected_pickup ||
        rejected_inventory.serialize() != before_rejected_inventory)
        return fail("rejected cave sword acquisition was not failure-atomic");
    if (
        session.try_take_start_sword(&inventory, &error) !=
            z1::OverworldSessionSwordResult::kAcquired || !session.start_sword_acquired())
        return fail("current-position-only source sword pickup did not commit Zelda1:01");
    if (!minish::foreign_world::has_flag(inventory.ownership(wooden_sword),
                                         minish::foreign_world::OwnershipFlags::Owned) ||
        inventory.loadout_slot(minish::foreign_world::LoadoutId::A, 0) != wooden_sword)
        return fail("starting sword did not atomically acquire and select its origin-qualified fact: " + error);
    const auto sword_use = inventory.resolve_loadout_use(minish::foreign_world::LoadoutId::A, 0);
    if (!sword_use || sword_use->acquired_id != wooden_sword || sword_use->behavior_id != wooden_sword ||
        sword_use->use_kind != minish::foreign_world::ItemUseKind::Equip ||
        sword_use->resource_pool != minish::foreign_world::ResourcePoolProvenance::None)
        return fail("starting sword did not preserve explicit cross-world use semantics");
    // A source cave pickup must never replace an already selected compatible
    // Native sword; ownership of Zelda1/01 remains an independent record.
    z1::Zelda1OverworldSession preserve_session;
    if (!preserve_session.load_hash_validated_ines(path, z1::Zelda1OverworldSession::kInitialRoom, &error) ||
        !preserve_session.restore(cave_entry_state, &error) ||
        !preserve_session.acknowledge_cave_dialogue() ||
        preserve_session.move_cave_by(0, static_cast<std::int16_t>(0x98 - 0xad)) !=
            z1::StartCaveMoveResult::kMoved ||
        preserve_session.move_cave_by(0x78 - 0x70, 0) != z1::StartCaveMoveResult::kMoved)
        return fail("could not restore source cave preserve-selection fixture: " + error);
    minish::foreign_world::InventoryCore preserve_inventory;
    const minish::foreign_world::CrossWorldItemId native_sword{
        minish::foreign_world::WorldId::Native, 1};
    const minish::foreign_world::ItemTraits native_traits{
        1, native_sword, minish::foreign_world::ItemUseKind::Equip,
        minish::foreign_world::ResourcePoolProvenance::None};
    if (!preserve_inventory.set_item(native_sword,
                                     minish::foreign_world::OwnershipFlags::Owned,
                                     minish::foreign_world::Capability::Sword,
                                     native_traits, &error) ||
        !preserve_inventory.set_loadout_slot(minish::foreign_world::LoadoutId::A, 2,
                                             native_sword, &error) ||
        preserve_session.try_take_start_sword(&preserve_inventory, &error) !=
            z1::OverworldSessionSwordResult::kAcquired ||
        preserve_inventory.loadout_slot(minish::foreign_world::LoadoutId::A, 2) != native_sword ||
        preserve_inventory.loadout_slot(minish::foreign_world::LoadoutId::A, 0) ||
        !minish::foreign_world::has_flag(preserve_inventory.ownership(wooden_sword),
                                          minish::foreign_world::OwnershipFlags::Owned))
        return fail("starting sword replaced the selected Native sword: " + error);
    // If A has no compatible sword and no empty slot, the staged Zelda record
    // and source cave state both roll back rather than producing an unusable
    // half-pickup.
    z1::Zelda1OverworldSession full_loadout_session;
    if (!full_loadout_session.load_hash_validated_ines(path, z1::Zelda1OverworldSession::kInitialRoom, &error) ||
        !full_loadout_session.restore(cave_entry_state, &error) ||
        !full_loadout_session.acknowledge_cave_dialogue() ||
        full_loadout_session.move_cave_by(0, static_cast<std::int16_t>(0x98 - 0xad)) !=
            z1::StartCaveMoveResult::kMoved ||
        full_loadout_session.move_cave_by(0x78 - 0x70, 0) != z1::StartCaveMoveResult::kMoved)
        return fail("could not restore full-loadout cave fixture: " + error);
    minish::foreign_world::InventoryCore full_loadout_inventory;
    for (std::size_t slot = 0; slot < minish::foreign_world::kLoadoutSlots; ++slot) {
        const minish::foreign_world::CrossWorldItemId passive{
            minish::foreign_world::WorldId::Native,
            static_cast<std::uint32_t>(0x40 + slot)};
        const minish::foreign_world::ItemTraits passive_traits{
            0, passive, minish::foreign_world::ItemUseKind::Passive,
            minish::foreign_world::ResourcePoolProvenance::None};
        if (!full_loadout_inventory.set_item(passive,
                                              minish::foreign_world::OwnershipFlags::Owned,
                                              minish::foreign_world::Capability::None,
                                              passive_traits, &error) ||
            !full_loadout_inventory.set_loadout_slot(minish::foreign_world::LoadoutId::A,
                                                      slot, passive, &error))
            return fail("could not construct full A loadout fixture: " + error);
    }
    const auto full_loadout_state_before = full_loadout_session.serialize();
    const auto full_loadout_inventory_before = full_loadout_inventory.serialize();
    if (full_loadout_session.try_take_start_sword(&full_loadout_inventory, &error) !=
            z1::OverworldSessionSwordResult::kInventoryRejected ||
        full_loadout_session.serialize() != full_loadout_state_before ||
        full_loadout_inventory.serialize() != full_loadout_inventory_before)
        return fail("full A loadout did not failure-atomically reject start sword selection");
    const auto cave_after_sword = session.framebuffer();
    if (framebuffer_hash(cave_after_sword) != 17774150873886944455ull || cave_after_sword == cave_before_sword ||
        !write_frame_ppm("build/zelda1_start_sword_cave_after.ppm", cave_after_sword))
        return fail("taken sword did not change the cave's pinned source framebuffer");
    const auto acquired_cave_state = session.serialize();
    if (!session.restore(acquired_cave_state, &error) || !session.start_sword_acquired() ||
        session.framebuffer() != cave_after_sword ||
        session.try_take_start_sword(&inventory, &error) !=
            z1::OverworldSessionSwordResult::kAlreadyAcquired)
        return fail("saved starting-sword state did not restore without respawning");
    auto corrupt_cave = acquired_cave_state;
    corrupt_cave[15] = 1;  // entering cannot coexist with a taken sword.
    const auto stable_cave = session.serialize();
    if (session.restore(corrupt_cave, &error) || session.serialize() != stable_cave)
        return fail("corrupt cave control state mutated the live session");
    const auto cave_state = session.serialize();
    if (!z1::Zelda1OverworldSession::validate_serialized(cave_state) ||
        !session.restore(cave_state, &error) || !session.in_cave() ||
        session.cave_index() != 16 ||
        session.move_cave_by(0, static_cast<std::int16_t>(0xdd - 0x98)) !=
            z1::StartCaveMoveResult::kMoved ||
        session.move_cave_one(z1::CaveDirection::kDown) != z1::StartCaveMoveResult::kExited ||
        session.in_cave() || session.position().room_id != 0x77 ||
        session.position().x != 56 || session.position().y != 21 ||
        session.framebuffer() != stable_frame)
        return fail("OW77 source cave return did not preserve the verified terrain frame: " + error);
    if (session.try_enter_cave() != z1::OverworldSessionCaveResult::kEntered ||
        session.framebuffer() != cave_after_sword ||
        !same_rectangle(session.framebuffer(), cave_before_sword, 0x48 - 8, 0x80 - 72, 16, 8) ||
        !same_rectangle(session.framebuffer(), cave_before_sword, 0xa8 - 8, 0x80 - 72, 16, 8) ||
        same_rectangle(session.framebuffer(), cave_before_sword, 0x78 - 8, 0x80 - 72, 16, 8))
        return fail("re-entered start cave did not retain fires while hiding sword and Old Man");
    if (!session.restore(initial_state, &error) || session.in_cave() ||
        session.framebuffer() != stable_frame)
        return fail("OW session did not restore before edge traversal: " + error);
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kNorth))
        return fail("OW session could not cross OW77 north into OW67");
    const auto north_state = session.serialize();
    const auto north_frame = session.framebuffer();
    if (framebuffer_hash(north_frame) != 11311219386543151198ull || session.position().x != 120 ||
        session.position().y != 149)
        return fail("OW67 frame or opposite-edge entry position changed");
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kWest) || session.position().room_id != 0x66)
        return fail("OW session could not cross OW67 west into OW66");
    const auto room66_frame = session.framebuffer();
    if (!session.octoroks().initialized() || session.position().x != 232 ||
        session.position().y != 85)
        return fail("OW66 frame or opposite-edge entry position changed");
    const auto session66_state = session.serialize();
    if (!session.tick_octoroks())
        return fail("session OW66 Octorok tick failed");
    auto clean_ow66_loader = loader;
    if (!clean_ow66_loader.render_overworld_room(0x66, &error))
        return fail("could not render clean OW66 comparison frame: " + error);
    auto expected_clean_ow66 = clean_ow66_loader.framebuffer();
    session.octoroks().render_qa_markers(&expected_clean_ow66);
    if (session.framebuffer() != expected_clean_ow66)
        return fail("moving OW66 QA actors accumulated trails over source terrain");
    if (!session.restore(session66_state, &error))
        return fail("session could not restore after clean Octorok redraw check: " + error);
    if (!session.hit_octorok(0, 1) || !session.hit_octorok(0, 1) ||
        !session.hit_octorok(0, 1) || !session.octoroks().actors()[0].defeated ||
        !session.octoroks().actors()[0].pending_drop)
        return fail("session fast Octorok did not require three source HP hits");
    const auto defeated66_state = session.serialize();
    auto corrupt_combat = defeated66_state;
    corrupt_combat[14] = 0;
    const auto stable_combat = session.serialize();
    if (session.restore(corrupt_combat, &error) || session.serialize() != stable_combat ||
        !session.restore(defeated66_state, &error) || !session.octoroks().actors()[0].defeated)
        return fail("session OW66 Octorok v4 restore/corruption contract failed");
    // Combat remains resident away from OW66, so a source-host defeated actor
    // cannot respawn on an east/west revisit or a v6 save/restore in between.
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kEast) ||
        session.position().room_id != 0x67 || !session.octoroks().initialized())
        return fail("leaving OW66 discarded retained combat state");
    const auto away66_state = session.serialize();
    if (!session.restore(away66_state, &error) ||
        !cross_at_matching_opening(&session, z1::OwScreenEdge::kWest) ||
        session.position().room_id != 0x66 || !session.octoroks().actors()[0].defeated)
        return fail("OW66 combat did not persist across away/reentry restore");
    if (!session.restore(north_state, &error) || session.position().room_id != 0x67 ||
        session.framebuffer() != north_frame)
        return fail("OW session failed to atomically restore a mid-route room: " + error);
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kSouth) || session.position().room_id != 0x77)
        return fail("OW session could not cross OW67 south into OW77");
    if (framebuffer_hash(session.framebuffer()) != 10743561982003409067ull || session.position().x != 120 ||
        session.position().y != -11)
        return fail("OW77 south-entry frame or position changed");
    if (!session.restore(initial_state, &error) || session.position().room_id != 0x77 ||
        session.framebuffer() != stable_frame ||
        !cross_at_matching_opening(&session, z1::OwScreenEdge::kWest) || session.position().room_id != 0x76)
        return fail("OW session could not restore/cross OW77 west into OW76: " + error);
    const auto west_frame = session.framebuffer();
    if (framebuffer_hash(west_frame) != 14736087833302446610ull || session.position().x != 232 ||
        session.position().y != 85)
        return fail("OW76 frame or opposite-edge entry position changed");
    if (!cross_at_matching_opening(&session, z1::OwScreenEdge::kEast) || session.position().room_id != 0x77)
        return fail("OW session could not cross OW76 east into OW77");
    if (framebuffer_hash(session.framebuffer()) != 10743561982003409067ull || session.position().x != -8 ||
        session.position().y != 85)
        return fail("OW77 east-entry frame or position changed");
    auto corrupt_session = session.serialize();
    corrupt_session[6] = 0x80;
    const auto stable_session = session.serialize();
    if (session.restore(corrupt_session, &error) || session.serialize() != stable_session)
        return fail("OW session accepted corrupt state or changed on failed restore");
    corrupt_session = stable_session;
    corrupt_session[11] = 2;
    if (session.restore(corrupt_session, &error) || session.serialize() != stable_session)
        return fail("OW session accepted an invalid cave-area enum or changed on failed restore");
    corrupt_session = stable_session;
    corrupt_session[11] = static_cast<std::uint8_t>(z1::OverworldSessionArea::kCave);
    corrupt_session[12] = 0x11;
    if (session.restore(corrupt_session, &error) || session.serialize() != stable_session)
        return fail("OW session accepted a cave index that disagrees with source room facts");
    std::cout << "Actual PRG0 iNES live OW77 frame passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (const int result = test_synthetic_rejections(); result != 0) return result;
    if (argc == 1) {
        std::cout << "Zelda 1 live iNES loader shape tests passed without ROM assets\n";
        return 0;
    }
    if (argc != 2) return fail("usage: test_zelda1_live_frame_loader [hash-validated-ines-path]");
    return test_actual_ines_path(argv[1]);
}
