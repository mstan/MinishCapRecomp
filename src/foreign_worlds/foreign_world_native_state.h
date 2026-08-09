// Game-owned native persistence for the Zelda 1 foreign-world seam.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "foreign_worlds/foreign_world.h"
#include "mod_state.h"

namespace minish::foreign_world {

// The current Zelda world-model save is a fixed Z1WM v2 record.  Keep this
// exact gate here until the live adapter can expose a shared strict validator.
// An empty blob is still allowed for a session that has not entered Zelda.
constexpr std::size_t kZelda1WorldBlobBytes = 340;
constexpr std::size_t kZelda1OverworldSessionBlobBytes = 64;

class ForeignWorldNativeState {
public:
    InventoryCore& inventory() { return inventory_; }
    const InventoryCore& inventory() const { return inventory_; }

    bool set_zelda1_world_blob(std::span<const std::uint8_t> blob,
                               std::string* error = nullptr);
    const std::vector<std::uint8_t>& zelda1_world_blob() const {
        return zelda1_world_blob_;
    }

    bool set_zelda1_overworld_session_blob(
        std::span<const std::uint8_t> blob, std::string* error = nullptr);
    void clear_zelda1_overworld_session_blob() {
        zelda1_overworld_session_blob_.clear();
    }
    const std::vector<std::uint8_t>& zelda1_overworld_session_blob() const {
        return zelda1_overworld_session_blob_;
    }

    // Host presentation lifecycle is separate from Z1OS terrain progress,
    // letting an active foreign PPU view resume without making a normal exit
    // discard its world state.
    void set_zelda1_presentation_active(bool active) {
        zelda1_presentation_active_ = active;
    }
    [[nodiscard]] bool zelda1_presentation_active() const {
        return zelda1_presentation_active_;
    }

    // Host session owners use this monotonically increasing value to notice a
    // provider restore on the next trusted emulation-thread hook. It is host
    // coordination state and is intentionally not serialized.
    [[nodiscard]] std::uint64_t restore_generation() const {
        return restore_generation_;
    }
    void replace_after_provider_restore(ForeignWorldNativeState&& restored);

    // FWNS v3 framing used inside the engine's trusted provider payload.
    // v1/v2 input remains readable and has no active presentation lifecycle.
    // Deserialize validates into a temporary state and swaps only on success.
    std::vector<std::uint8_t> serialize() const;
    static bool deserialize(std::span<const std::uint8_t> bytes,
                            ForeignWorldNativeState* output,
                            std::string* error = nullptr);

private:
    InventoryCore inventory_;
    std::vector<std::uint8_t> zelda1_world_blob_;
    std::vector<std::uint8_t> zelda1_overworld_session_blob_;
    bool zelda1_presentation_active_ = false;
    std::uint64_t restore_generation_ = 0;
};

ForeignWorldNativeState& foreign_world_native_state();

// Build a trusted provider for an explicit state instance (useful in tests).
// The Zelda feature activation (not a normal game constructor) registers the
// singleton through this API. The engine freezes provider membership after the
// first snapshot, so enabling this feature after that boundary requires a
// process restart before it can safely participate in saves.
gbarecomp::debug::ModStateProvider make_foreign_world_native_state_provider(
    ForeignWorldNativeState& state);
bool register_foreign_world_native_state_provider(std::string* error = nullptr);

}  // namespace minish::foreign_world
