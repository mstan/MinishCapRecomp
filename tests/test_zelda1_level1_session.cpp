#include "foreign_worlds/zelda1/zelda1_first_quest_data.h"
#include "foreign_worlds/zelda1/zelda1_level1_session.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace z1 = minish::foreign_world::zelda1;

namespace {
int fail(const std::string& message) { std::cerr << "FAIL: " << message << "\n"; return 1; }

void set_doors(std::vector<std::uint8_t>* bytes, std::uint8_t room,
               z1::DoorType east, z1::DoorType west, z1::DoorType south,
               z1::DoorType north) {
    constexpr std::size_t kBlock = 100096;
    (*bytes)[kBlock + room] = static_cast<std::uint8_t>((static_cast<unsigned>(south) << 2) |
                                                          (static_cast<unsigned>(north) << 5));
    (*bytes)[kBlock + 128 + room] = static_cast<std::uint8_t>((static_cast<unsigned>(east) << 2) |
                                                                (static_cast<unsigned>(west) << 5));
}

std::optional<z1::FirstQuestData> make_synthetic(std::string* error,
                                                  bool fail_start_render = false,
                                                  bool fail_north_render = false) {
    std::vector<std::uint8_t> bytes(z1::kVerifiedPrgSize, 0);
    constexpr std::size_t kBlock = 100096, kInfo = 103420;
    constexpr std::size_t kOwBlock = 99328, kLayouts = 90334;
    constexpr std::size_t kDirectory = 91908, kPrimary = 91928, kBank5 = 81920;
    // All unused rooms are sealed. The path is the real Level 1 arithmetic
    // topology and uses the same source DoorType encodings as the actual PRG.
    for (unsigned room = 0; room < 128; ++room)
        set_doors(&bytes, static_cast<std::uint8_t>(room), z1::DoorType::kWall,
                  z1::DoorType::kWall, z1::DoorType::kWall, z1::DoorType::kWall);
    // Z_05:CreateRoomObjects uses packed room-item ID $03 as no-item.
    for (unsigned room = 0; room < 128; ++room) bytes[kBlock + 4 * 128 + room] = 0x03;
    set_doors(&bytes, 0x73, z1::DoorType::kOpen, z1::DoorType::kOpen,
              z1::DoorType::kOpen, z1::DoorType::kKey);
    set_doors(&bytes, 0x74, z1::DoorType::kWall, z1::DoorType::kOpen,
              z1::DoorType::kWall, z1::DoorType::kWall);
    set_doors(&bytes, 0x63, z1::DoorType::kWall, z1::DoorType::kWall,
              z1::DoorType::kKey, z1::DoorType::kOpen);
    set_doors(&bytes, 0x53, z1::DoorType::kOpen, z1::DoorType::kWall,
              z1::DoorType::kOpen, z1::DoorType::kWall);
    set_doors(&bytes, 0x54, z1::DoorType::kWall, z1::DoorType::kOpen,
              z1::DoorType::kWall, z1::DoorType::kBombable);
    set_doors(&bytes, 0x44, z1::DoorType::kOpen, z1::DoorType::kWall,
              z1::DoorType::kBombable, z1::DoorType::kWall);
    set_doors(&bytes, 0x45, z1::DoorType::kWall, z1::DoorType::kOpen,
              z1::DoorType::kWall, z1::DoorType::kKey);
    set_doors(&bytes, 0x35, z1::DoorType::kShutter, z1::DoorType::kWall,
              z1::DoorType::kKey, z1::DoorType::kWall);
    bytes[kBlock + 4 * 128 + 0x74] = z1::Zelda1Level1Session::kKeyItemId;
    bytes[kBlock + 4 * 128 + 0x45] = z1::Zelda1Level1Session::kKeyItemId;
    bytes[kBlock + 4 * 128 + 0x35] = 0x1a;  // source heart-container ID
    bytes[kBlock + 2 * 128 + 0x35] = z1::Zelda1Level1Session::kAquamentusTemplate;
    bytes[kBlock + 5 * 128 + 0x35] = 7;     // foes-for-item secret action
    bytes[kInfo + 0] = 0x3f; bytes[kInfo + 0x2f] = 0x73;
    bytes[kInfo + 0x33] = 1; bytes[kInfo + 0x3e] = 0x35;
    bytes[kOwBlock + 128 + 0x37] = 0x04; // AttrB: entrance index one
    // Enough source-shaped data to render the static UW tile plane.
    for (unsigned i = 0; i < 12; ++i) bytes[kLayouts + i] = 0;
    bytes[kDirectory] = 0; bytes[kDirectory + 1] = 0x80;
    bytes[kPrimary] = 0x70;
    for (unsigned i = 0; i < 7; ++i) bytes[kBank5 + i] = 0x80;
    // Make exactly the selected room's static layout unreadable after prior
    // rooms have rendered. This is a renderer failure injection through the
    // same verified-PRG table boundary, not a session test backdoor.
    if (fail_start_render || fail_north_render) {
        const std::uint8_t failing_room = fail_start_render ? 0x73 : 0x63;
        bytes[kBlock + 3 * 128 + failing_room] = 1;
        bytes[kLayouts + 12] = 0xf0;  // directory entry 15 -> CPU address $0000
    }
    auto prg = z1::VerifiedPrg::from_verified_bytes(std::move(bytes), error);
    return prg ? z1::FirstQuestData::create(std::move(*prg), error) : std::nullopt;
}

bool write_ppm(const char* path, const z1::OwFramebuffer& frame) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "P6\n240 160\n255\n";
    for (const auto pixel : frame) {
        const unsigned r = pixel & 31, g = (pixel >> 5) & 31, b = (pixel >> 10) & 31;
        out.put(static_cast<char>((r << 3) | (r >> 2)));
        out.put(static_cast<char>((g << 3) | (g >> 2)));
        out.put(static_cast<char>((b << 3) | (b >> 2)));
    }
    return static_cast<bool>(out);
}

int run_route(const z1::FirstQuestData& data, bool emit_artifacts) {
    z1::Zelda1Level1Session session(data);
    if (session.enter_from_overworld(0x36) != z1::Level1Result::kInvalid ||
        session.enter_from_overworld(0x37) != z1::Level1Result::kEntered ||
        session.room_id() != 0x73) return fail("source Level 1 entrance did not start in $73");
    if (emit_artifacts && !write_ppm("build/zelda1_l1_entry.ppm", session.framebuffer()))
        return fail("could not write Level 1 entry screenshot");
    if (session.try_transition(z1::Level1Edge::kNorth) != z1::Level1Result::kNeedsKey ||
        session.try_transition(z1::Level1Edge::kEast) != z1::Level1Result::kMoved ||
        session.room_id() != 0x74 || !session.available_room_item() ||
        session.collect_room_item() != z1::Level1Result::kCollected || session.key_count() != 1 ||
        session.try_transition(z1::Level1Edge::kWest) != z1::Level1Result::kMoved ||
        session.try_transition(z1::Level1Edge::kNorth) != z1::Level1Result::kMoved ||
        session.room_id() != 0x63 || session.key_count() != 0 ||
        session.try_transition(z1::Level1Edge::kNorth) != z1::Level1Result::kMoved ||
        session.try_transition(z1::Level1Edge::kEast) != z1::Level1Result::kMoved ||
        session.room_id() != 0x54 ||
        (emit_artifacts && !write_ppm("build/zelda1_l1_intermediate.ppm", session.framebuffer())) ||
        session.try_transition(z1::Level1Edge::kNorth) != z1::Level1Result::kNeedsBomb)
        return fail("actual Level 1 door/key route did not reach the bomb boundary");
    // A Native-origin bomb proves that capability resolution is intentionally
    // origin-agnostic when crossing worlds; the item identity stays distinct.
    minish::foreign_world::InventoryCore inventory;
    const minish::foreign_world::CrossWorldItemId native_bomb{
        minish::foreign_world::WorldId::Native, 0x42};
    const minish::foreign_world::ItemTraits native_bomb_traits{
        1, native_bomb, minish::foreign_world::ItemUseKind::Equip,
        minish::foreign_world::ResourcePoolProvenance::Native};
    std::string inventory_error;
    if (!inventory.acquire_item(native_bomb, minish::foreign_world::OwnershipFlags::Owned,
                                minish::foreign_world::Capability::Bomb,
                                native_bomb_traits, &inventory_error) ||
        !inventory.set_loadout_slot(minish::foreign_world::LoadoutId::A, 0,
                                    native_bomb, &inventory_error) ||
        z1::Zelda1Level1Session::resolve_door_tool(
            inventory, minish::foreign_world::LoadoutId::A) != z1::Level1DoorTool::kBomb ||
        session.try_transition(z1::Level1Edge::kNorth, inventory,
                               minish::foreign_world::LoadoutId::A) != z1::Level1Result::kMoved ||
        session.room_id() != 0x44 || session.try_transition(z1::Level1Edge::kEast) != z1::Level1Result::kMoved ||
        session.room_id() != 0x45 || session.collect_room_item() != z1::Level1Result::kCollected ||
        session.key_count() != 1 || session.try_transition(z1::Level1Edge::kNorth) != z1::Level1Result::kMoved ||
        session.room_id() != 0x35) return fail("actual Level 1 door/key/bomb route did not reach Aquamentus");
    if (emit_artifacts && !write_ppm("build/zelda1_l1_boss_before.ppm", session.framebuffer()))
        return fail("could not write Level 1 boss-before screenshot");
    const auto state = session.boss();
    if (!state.present || state.source_template_id != 0x3d || state.hit_points != 6 ||
        state.x != 0xb0 || state.y != 0x80 || session.contact_boss() != z1::Level1Result::kContact)
        return fail("Aquamentus source template/HP/spawn facts were not retained");
    const auto boss_before = session.serialize();
    const auto reject_without_mutation = [&](std::vector<std::uint8_t> corrupt,
                                              const char* detail) {
        return !session.restore(corrupt) && session.serialize() == boss_before
            ? 0 : fail(detail);
    };
    auto corrupt = boss_before;
    corrupt[12 + 0x73] |= 1; // $03 is CreateRoomObjects' packed no-item sentinel.
    if (const int result = reject_without_mutation(corrupt, "restore accepted an item flag for a no-item room")) return result;
    corrupt = boss_before;
    corrupt[12 + 0x35] |= 1; // Boss reward cannot be collected before defeat.
    if (const int result = reject_without_mutation(corrupt, "restore accepted premature boss reward collection")) return result;
    corrupt = boss_before;
    constexpr std::size_t kOpenedDoors = 140;
    constexpr std::size_t kStartNorthBit = 0x73 * 4 + 3;
    corrupt[kOpenedDoors + (kStartNorthBit / 8)] &= static_cast<std::uint8_t>(~(1u << (kStartNorthBit % 8)));
    if (const int result = reject_without_mutation(corrupt, "restore accepted asymmetric opened-door state")) return result;
    for (unsigned hit = 0; hit < 5; ++hit)
        if (session.sword_hit_boss(1) != z1::Level1Result::kContact) return fail("boss hit was rejected");
    if (session.sword_hit_boss(1) != z1::Level1Result::kDefeated || !session.boss().defeated ||
        !session.available_room_item() || session.available_room_item()->source_item_id != 0x1a ||
        session.collect_room_item() != z1::Level1Result::kCollected)
        return fail("boss defeat did not reveal/collect its source room item");
    if (emit_artifacts && !write_ppm("build/zelda1_l1_boss_defeated.ppm", session.framebuffer()))
        return fail("could not write Level 1 boss-defeated screenshot");
    std::uint8_t exit_room = 0;
    if (session.exit_to_overworld(&exit_room) != z1::Level1Result::kInvalid ||
        session.try_transition(z1::Level1Edge::kSouth) != z1::Level1Result::kMoved ||
        session.try_transition(z1::Level1Edge::kWest) != z1::Level1Result::kMoved ||
        session.try_transition(z1::Level1Edge::kSouth) != z1::Level1Result::kMoved ||
        session.try_transition(z1::Level1Edge::kWest) != z1::Level1Result::kMoved ||
        session.try_transition(z1::Level1Edge::kSouth) != z1::Level1Result::kMoved ||
        session.try_transition(z1::Level1Edge::kSouth) != z1::Level1Result::kMoved ||
        session.room_id() != 0x73 || session.exit_to_overworld(&exit_room) != z1::Level1Result::kReturned || exit_room != 0x37)
        return fail("Level 1 return portal did not preserve OW $37");
    auto bytes = session.serialize(), unchanged = bytes;
    bytes[11] = 1;
    if (session.restore(bytes) || session.serialize() != unchanged) return fail("persistence restore was not failure-atomic");
    if (!session.restore(unchanged) || session.active()) return fail("valid inactive Level 1 session did not restore");
    return 0;
}

int test_render_failure_atomicity() {
    std::string error;
    auto start_failure = make_synthetic(&error, true, false);
    if (!start_failure) return fail("start failure synthetic data rejected: " + error);
    z1::Zelda1Level1Session entry(*start_failure);
    const auto pristine = entry.serialize();
    if (entry.enter_from_overworld(0x37) != z1::Level1Result::kInvalid || entry.serialize() != pristine)
        return fail("failed entry mutated the Level 1 session");
    auto north_failure = make_synthetic(&error, false, true);
    if (!north_failure) return fail("north failure synthetic data rejected: " + error);
    z1::Zelda1Level1Session session(*north_failure);
    if (session.enter_from_overworld(0x37) != z1::Level1Result::kEntered ||
        session.try_transition(z1::Level1Edge::kEast) != z1::Level1Result::kMoved ||
        session.collect_room_item() != z1::Level1Result::kCollected ||
        session.try_transition(z1::Level1Edge::kWest) != z1::Level1Result::kMoved)
        return fail("could not prepare source key transition failure test");
    const auto before_key_door = session.serialize();
    if (session.try_transition(z1::Level1Edge::kNorth) != z1::Level1Result::kInvalid ||
        session.serialize() != before_key_door)
        return fail("failed room render consumed a key or opened its door");
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) return fail("optional argument is an iNES Zelda 1 ROM path");
    std::string error;
    auto synthetic = make_synthetic(&error);
    if (!synthetic) return fail("synthetic source data rejected: " + error);
    if (const int result = run_route(*synthetic, false)) return result;
    if (const int result = test_render_failure_atomicity()) return result;
    if (argc == 1) { std::cout << "Level 1 pure host session passed\n"; return 0; }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<std::uint8_t> ines((std::istreambuf_iterator<char>(input)), {});
    if (ines.size() != 16 + z1::kVerifiedPrgSize) return fail("actual ROM does not have the expected iNES PRG0 shape");
    std::vector<std::uint8_t> prg(ines.begin() + 16, ines.end());
    auto verified = z1::VerifiedPrg::from_verified_bytes(std::move(prg), &error);
    auto actual = verified ? z1::FirstQuestData::create(std::move(*verified), &error) : std::nullopt;
    if (!actual) return fail("actual ROM source data rejected: " + error);
    const int result = run_route(*actual, true);
    if (!result) std::cout << "Actual-ROM Level 1 route and screenshots passed\n";
    return result;
}
