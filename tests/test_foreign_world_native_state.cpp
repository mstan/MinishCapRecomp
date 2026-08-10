#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "foreign_worlds/foreign_world_native_state.h"
#include "foreign_worlds/zelda1/zelda1_overworld_music.h"

namespace fw = minish::foreign_world;

namespace {
int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

bool same_bytes(std::span<const std::uint8_t> left,
                std::span<const std::uint8_t> right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

fw::ItemTraits traits(fw::WorldId origin, fw::ResourcePoolProvenance pool) {
    return {2, {origin, 0x91}, fw::ItemUseKind::Equip, pool};
}

std::vector<std::uint8_t> partial_aquamentus_v2() {
    // Matches Zelda1WorldModel's fixed v2 layout: an alive Aquamentus with
    // only three of its six health points remaining, before its opaque drop.
    std::vector<std::uint8_t> world(fw::kZelda1WorldBlobBytes, 0);
    world[0] = 'Z'; world[1] = '1'; world[2] = 'W'; world[3] = 'M'; world[4] = 2;
    world[5] = 2;       // Level 1
    world[6] = 0x42;    // boss room
    world[274] = 1;     // one actor
    world[275] = 0;     // no opaque drop yet
    world[276] = 0x3d;  // Aquamentus template
    world[277] = 3;     // partially damaged
    world[278] = 0x09;  // known HP + opaque drop, not defeated
    world[280] = 0xb0;
    world[281] = 0x80;
    return world;
}

std::vector<std::uint8_t> overworld_session_v5() {
    std::vector<std::uint8_t> session(fw::kZelda1OverworldSessionBlobBytes, 0);
    session[0] = 'Z'; session[1] = '1'; session[2] = 'O'; session[3] = 'S';
    session[4] = 5; session[5] = 1;
    session[6] = 0x77; session[7] = 56; session[8] = 21;
    session[9] = 1; session[10] = 16; session[11] = 1;
    session[14] = 1;  // dialogue acknowledged
    session[16] = 0x78; session[17] = 0x98;  // collected-sword source position
    return session;
}

std::vector<std::uint8_t> overworld_session_ow66_v5() {
    std::vector<std::uint8_t> session(fw::kZelda1OverworldSessionBlobBytes, 0);
    session[0] = 'Z'; session[1] = '1'; session[2] = 'O'; session[3] = 'S';
    session[4] = 5; session[5] = 1;
    session[6] = 0x66; session[7] = 120; session[8] = 80; session[12] = 1;
    auto actor = session.begin() + 32;
    actor[0] = 'Z'; actor[1] = '1'; actor[2] = 'O'; actor[3] = 'R'; actor[4] = 1;
    actor[5] = 9; // source-actor cadence survives as an opaque host record.
    const std::array<std::array<std::uint8_t, 5>, 4> actors{{
        {{7, 1, 0x50, 0x50, 0}}, {{8, 3, 0x60, 0x60, 0}},
        {{8, 0, 0x70, 0x70, 3}}, {{7, 1, 0x80, 0x80, 0}},
    }};
    for (std::size_t i = 0; i < actors.size(); ++i)
        std::copy(actors[i].begin(), actors[i].end(), actor + 9 + i * 5);
    return session;
}

std::vector<std::uint8_t> stopped_overworld_music_state() {
    // The renderer's stopped state is an intentionally valid, bounded
    // transport record.  It is sufficient to prove FWNS treats the music
    // state as opaque-but-structurally-validated native state.
    const fw::zelda1::Zelda1OverworldMusicRenderer renderer;
    const auto serialized = renderer.serialize();
    return {serialized.begin(), serialized.end()};
}

bool populate(fw::ForeignWorldNativeState* state, std::string* error) {
    const fw::CrossWorldItemId native{fw::WorldId::Native, 7};
    const fw::CrossWorldItemId zelda{fw::WorldId::Zelda1, 7};
    auto& inventory = state->inventory();
    if (!inventory.set_item(native, fw::OwnershipFlags::Owned,
                            fw::Capability::Sword, traits(fw::WorldId::Native,
                                                          fw::ResourcePoolProvenance::Native), error) ||
        !inventory.set_item(zelda, fw::OwnershipFlags::Owned | fw::OwnershipFlags::Quest,
                            fw::Capability::Sword | fw::Capability::Bomb,
                            traits(fw::WorldId::Zelda1,
                                   fw::ResourcePoolProvenance::Zelda1), error) ||
        !inventory.set_loadout_slot(fw::LoadoutId::A, 0, native, error) ||
        !inventory.set_loadout_slot(fw::LoadoutId::B, 2, zelda, error))
        return false;
    inventory.native_resources() = {5, 6, 20, 1, 0, 0};
    inventory.zelda1_resources() = {12, 16, 255, 8, 4, 2};
    const auto world = partial_aquamentus_v2();
    const auto session = overworld_session_v5();
    const auto music = stopped_overworld_music_state();
    if (!state->set_zelda1_world_blob(world, error) ||
        !state->set_zelda1_overworld_session_blob(session, error) ||
        !state->set_zelda1_overworld_music_blob(music, error))
        return false;
    state->set_zelda1_presentation_active(true);
    return true;
}

int test_native_state_and_provider_roundtrip() {
    // Zelda's feature activation owns registration. With no activation the
    // normal Minish MODS catalog stays empty and legacy snapshots remain
    // loadable without a foreign-world provider/schema entry.
    if (gbarecomp::debug::global_mod_state_registry().size() != 0)
        return fail("disabled foreign feature unexpectedly registered a provider");
    std::string error;
    if (!fw::register_foreign_world_native_state_provider(&error) ||
        !fw::register_foreign_world_native_state_provider(&error) ||
        gbarecomp::debug::global_mod_state_registry().size() != 1)
        return fail("explicit activation-time provider registration was not idempotent: " + error);
    fw::ForeignWorldNativeState empty;
    fw::ForeignWorldNativeState empty_restored;
    if (!fw::ForeignWorldNativeState::deserialize(empty.serialize(), &empty_restored, &error) ||
        !empty_restored.zelda1_world_blob().empty() ||
        !empty_restored.zelda1_overworld_session_blob().empty() ||
        !empty_restored.zelda1_overworld_music_blob().empty() ||
        empty_restored.zelda1_presentation_active())
        return fail("empty-before-Zelda state did not remain empty: " + error);
    auto legacy_v3 = empty.serialize();
    // v4 adds a u32 music length immediately before the old lifecycle byte.
    legacy_v3.erase(legacy_v3.end() - 5, legacy_v3.end() - 1);
    legacy_v3[4] = 3; legacy_v3[5] = 0;
    auto legacy_v2 = legacy_v3;
    legacy_v2.pop_back();  // v2 had no presentation-active byte.
    legacy_v2[4] = 2; legacy_v2[5] = 0;
    auto legacy_v1 = legacy_v2;
    legacy_v1.erase(legacy_v1.end() - 4, legacy_v1.end());  // no Z1OS size.
    legacy_v1[4] = 1; legacy_v1[5] = 0;
    for (const auto& legacy : {legacy_v1, legacy_v2, legacy_v3}) {
        if (!fw::ForeignWorldNativeState::deserialize(legacy, &empty_restored, &error) ||
            !empty_restored.zelda1_overworld_music_blob().empty() ||
            empty_restored.zelda1_presentation_active())
            return fail("FWNS v1-v3 did not migrate to empty music state: " + error);
    }
    fw::ForeignWorldNativeState source;
    if (!populate(&source, &error)) return fail("could not build source: " + error);
    const fw::CrossWorldItemId native{fw::WorldId::Native, 7};
    const fw::CrossWorldItemId zelda{fw::WorldId::Zelda1, 7};
    if (source.inventory().loadout_slot(fw::LoadoutId::A, 0) != native ||
        source.inventory().loadout_slot(fw::LoadoutId::B, 2) != zelda ||
        source.inventory().ownership(native) == source.inventory().ownership(zelda) ||
        source.inventory().native_resources() == source.inventory().zelda1_resources())
        return fail("same-number native/Zelda IDs or cross-world resources merged");

    gbarecomp::debug::ModStateRegistry source_registry;
    if (!source_registry.register_provider(
            fw::make_foreign_world_native_state_provider(source), &error))
        return fail("source provider registration failed: " + error);
    gbarecomp::debug::SnapshotWriter saved;
    if (!source_registry.serialize(saved, &error)) return fail("provider serialize failed: " + error);

    fw::ForeignWorldNativeState restored;
    auto prior_world = partial_aquamentus_v2();
    prior_world[277] = 6;  // intact boss; distinct valid preflight-preservation state
    if (!restored.set_zelda1_world_blob(prior_world, &error)) return fail(error);
    const auto prior = restored.serialize();
    gbarecomp::debug::ModStateRegistry restored_registry;
    if (!restored_registry.register_provider(
            fw::make_foreign_world_native_state_provider(restored), &error))
        return fail("restore provider registration failed: " + error);
    gbarecomp::debug::SnapshotReader preflight(saved.buffer().data(), saved.size());
    if (!restored_registry.preflight(preflight, &error)) return fail("provider preflight failed: " + error);
    gbarecomp::debug::SnapshotReader apply(saved.buffer().data(), saved.size());
    if (!restored_registry.restore(apply, &error) || restored.serialize() != source.serialize() ||
        restored.zelda1_world_blob() != source.zelda1_world_blob() ||
        restored.zelda1_world_blob()[277] != 3 ||
        restored.zelda1_overworld_session_blob() !=
            source.zelda1_overworld_session_blob() ||
        restored.zelda1_overworld_session_blob()[9] != 1 ||
        restored.zelda1_overworld_session_blob()[11] != 1 ||
        restored.zelda1_overworld_session_blob()[16] != 0x78 ||
        !same_bytes(restored.zelda1_overworld_music_blob(),
                    source.zelda1_overworld_music_blob()) ||
        !restored.zelda1_presentation_active() ||
        restored.restore_generation() != 1)
        return fail("provider roundtrip lost inventory or world blob");

    // A separate provider roundtrip carries the exact fixed OW66 actor
    // fragment, rather than collapsing it into the cave record above.
    fw::ForeignWorldNativeState actor_source;
    if (!populate(&actor_source, &error) ||
        !actor_source.set_zelda1_overworld_session_blob(overworld_session_ow66_v5(), &error))
        return fail("could not construct OW66 provider fixture: " + error);
    gbarecomp::debug::ModStateRegistry actor_source_registry;
    if (!actor_source_registry.register_provider(
            fw::make_foreign_world_native_state_provider(actor_source), &error))
        return fail("OW66 source provider registration failed: " + error);
    gbarecomp::debug::SnapshotWriter actor_saved;
    if (!actor_source_registry.serialize(actor_saved, &error)) return fail(error);
    fw::ForeignWorldNativeState actor_restored;
    gbarecomp::debug::ModStateRegistry actor_restored_registry;
    if (!actor_restored_registry.register_provider(
            fw::make_foreign_world_native_state_provider(actor_restored), &error))
        return fail("OW66 restore provider registration failed: " + error);
    gbarecomp::debug::SnapshotReader actor_preflight(actor_saved.buffer().data(), actor_saved.size());
    gbarecomp::debug::SnapshotReader actor_apply(actor_saved.buffer().data(), actor_saved.size());
    if (!actor_restored_registry.preflight(actor_preflight, &error) ||
        !actor_restored_registry.restore(actor_apply, &error) ||
        actor_restored.zelda1_overworld_session_blob() !=
            actor_source.zelda1_overworld_session_blob() ||
        actor_restored.zelda1_overworld_session_blob()[32 + 9 + 2 * 5 + 4] != 3 ||
        actor_restored.restore_generation() != 1)
        return fail("provider roundtrip lost OW66 actor state");

    const std::vector<std::uint8_t> session_marker = {'Z', '1', 'O', 'S'};
    const auto session_it = std::search(saved.buffer().begin(), saved.buffer().end(),
                                        session_marker.begin(), session_marker.end());
    if (session_it == saved.buffer().end()) return fail("serialized Z1OS marker absent");
    auto corrupt_session = saved.buffer();
    corrupt_session[static_cast<std::size_t>(session_it - saved.buffer().begin()) + 4] = 4;
    fw::ForeignWorldNativeState session_untouched;
    if (!populate(&session_untouched, &error)) return fail(error);
    const auto session_prior = session_untouched.serialize();
    gbarecomp::debug::ModStateRegistry session_registry;
    if (!session_registry.register_provider(
            fw::make_foreign_world_native_state_provider(session_untouched), &error))
        return fail(error);
    gbarecomp::debug::SnapshotReader session_reader(corrupt_session.data(),
                                                     corrupt_session.size());
    if (session_registry.preflight(session_reader, &error) ||
        session_untouched.serialize() != session_prior ||
        session_untouched.restore_generation() != 0)
        return fail("corrupt Z1OS passed preflight or mutated provider state");

    auto corrupt_lifecycle = source.serialize();
    corrupt_lifecycle.back() = 2;
    fw::ForeignWorldNativeState lifecycle_untouched;
    if (!populate(&lifecycle_untouched, &error)) return fail(error);
    const auto lifecycle_prior = lifecycle_untouched.serialize();
    if (fw::ForeignWorldNativeState::deserialize(corrupt_lifecycle,
                                                  &lifecycle_untouched, &error) ||
        lifecycle_untouched.serialize() != lifecycle_prior)
        return fail("invalid FWNS v4 presentation lifecycle mutated state");

    // The provider preflight must use the renderer's strict structural gate,
    // and must not alter the installed native state or its restore generation.
    const auto music = source.zelda1_overworld_music_blob();
    const auto source_before_bad_checkpoint = source.serialize();
    auto direct_corrupt_music = std::vector<std::uint8_t>(music.begin(), music.end());
    direct_corrupt_music[37] = 7;
    if (source.checkpoint_zelda1_overworld_music_blob(direct_corrupt_music, &error) ||
        source.serialize() != source_before_bad_checkpoint)
        return fail("corrupt Z1OM checkpoint mutated native state");
    const auto music_it = std::search(saved.buffer().begin(), saved.buffer().end(),
                                      music.begin(), music.end());
    if (music_it == saved.buffer().end()) return fail("serialized Z1OM record absent");
    auto corrupt_music = saved.buffer();
    // Byte 37 is phrase_index in the fixed renderer record. A stopped record
    // must use 8; 7 is structurally invalid and should fail before restore.
    corrupt_music[static_cast<std::size_t>(music_it - saved.buffer().begin()) + 37] = 7;
    fw::ForeignWorldNativeState music_untouched;
    if (!populate(&music_untouched, &error)) return fail(error);
    const auto music_prior = music_untouched.serialize();
    gbarecomp::debug::ModStateRegistry music_registry;
    if (!music_registry.register_provider(
            fw::make_foreign_world_native_state_provider(music_untouched), &error))
        return fail(error);
    gbarecomp::debug::SnapshotReader music_reader(corrupt_music.data(),
                                                   corrupt_music.size());
    if (music_registry.preflight(music_reader, &error) ||
        music_untouched.serialize() != music_prior ||
        music_untouched.restore_generation() != 0)
        return fail("corrupt Z1OM passed preflight or mutated provider state");

    const std::vector<std::uint8_t> marker = {'Z', '1', 'W', 'M'};
    const auto marker_it = std::search(saved.buffer().begin(), saved.buffer().end(),
                                       marker.begin(), marker.end());
    if (marker_it == saved.buffer().end()) return fail("serialized Z1WM marker absent");
    const auto z1_offset = static_cast<std::size_t>(marker_it - saved.buffer().begin());
    auto preflight_rejects_without_mutation = [&](std::vector<std::uint8_t> corrupt,
                                                    const char* label) -> bool {
        fw::ForeignWorldNativeState untouched;
        std::string local_error;
        if (!untouched.set_zelda1_world_blob(prior_world, &local_error)) return false;
        gbarecomp::debug::ModStateRegistry corrupt_registry;
        if (!corrupt_registry.register_provider(
                fw::make_foreign_world_native_state_provider(untouched), &local_error))
            return false;
        gbarecomp::debug::SnapshotReader corrupt_reader(corrupt.data(), corrupt.size());
        if (corrupt_registry.preflight(corrupt_reader, &local_error) ||
            untouched.serialize() != prior) {
            std::cerr << "corrupt Z1WM " << label << " passed or mutated state\n";
            return false;
        }
        return true;
    };
    // Exercise every data-independent validator field that could otherwise
    // let a semantically corrupt world survive snapshot preflight.
    std::vector<std::uint8_t> corrupt = saved.buffer();
    corrupt[z1_offset] = 0;
    if (!preflight_rejects_without_mutation(corrupt, "magic")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 5] = 3;
    if (!preflight_rejects_without_mutation(corrupt, "area")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 274] = 9;
    if (!preflight_rejects_without_mutation(corrupt, "actor count")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 278] = 0x10;
    if (!preflight_rejects_without_mutation(corrupt, "actor flags")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 277] = 7;
    if (!preflight_rejects_without_mutation(corrupt, "actor HP")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 9] = 7;
    if (!preflight_rejects_without_mutation(corrupt, "world health")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 14] = 1;
    if (!preflight_rejects_without_mutation(corrupt, "overworld flags")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 142] = 4;
    if (!preflight_rejects_without_mutation(corrupt, "level flags")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 276 + 6] = 1;
    if (!preflight_rejects_without_mutation(corrupt, "actor reserved byte")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 278] = 0x08;  // HP unknown but nonzero HP persists.
    if (!preflight_rejects_without_mutation(corrupt, "unknown actor HP")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 278] = 0x0d;  // defeated while HP remains nonzero.
    if (!preflight_rejects_without_mutation(corrupt, "defeated actor HP")) return 1;
    corrupt = saved.buffer();
    corrupt[z1_offset + 275] = 1;
    corrupt[z1_offset + 276 + 8 * 7] = 1;
    if (!preflight_rejects_without_mutation(corrupt, "opaque drop")) return 1;
    return 0;
}
}  // namespace

int main() {
    if (const int result = test_native_state_and_provider_roundtrip()) return result;
    std::cout << "Foreign-world native provider roundtrip and corruption gate passed\n";
    return 0;
}
