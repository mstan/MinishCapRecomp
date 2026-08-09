#include "foreign_worlds/foreign_world_native_state.h"
#include "foreign_worlds/zelda1/zelda1_overworld_session.h"
#include "foreign_worlds/zelda1/zelda1_world_model.h"

#include <array>
#include <cstdlib>

#include "mod_runtime.h"

namespace minish::foreign_world {
namespace {

constexpr std::array<std::uint8_t, 4> kStateMagic{'F', 'W', 'N', 'S'};
constexpr std::uint16_t kStateVersion = 3;
constexpr char kProviderId[] = "minish.foreign-world";
constexpr std::uint32_t kProviderSchema = 3;
static_assert(kZelda1WorldBlobBytes == zelda1::Zelda1WorldModel::kSerializedSize,
              "native persistence must track the live Z1WM record size");
static_assert(kZelda1OverworldSessionBlobBytes ==
                  zelda1::Zelda1OverworldSession::kSerializedSize,
              "native persistence must track the live Z1OS record size");

void set_error(std::string* error, const char* message) {
    if (error) *error = message;
}

void append_u16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value));
    out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        out->push_back(static_cast<std::uint8_t>(value >> shift));
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool u8(std::uint8_t* value) {
        if (offset_ == bytes_.size()) return false;
        *value = bytes_[offset_++];
        return true;
    }
    bool u16(std::uint16_t* value) {
        std::uint8_t low, high;
        if (!u8(&low) || !u8(&high)) return false;
        *value = static_cast<std::uint16_t>(low) |
                 (static_cast<std::uint16_t>(high) << 8);
        return true;
    }
    bool u32(std::uint32_t* value) {
        *value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8) {
            std::uint8_t byte;
            if (!u8(&byte)) return false;
            *value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool bytes(std::size_t count, std::vector<std::uint8_t>* value) {
        if (count > bytes_.size() - offset_) return false;
        value->assign(bytes_.begin() + offset_, bytes_.begin() + offset_ + count);
        offset_ += count;
        return true;
    }
    bool at_end() const { return offset_ == bytes_.size(); }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

bool valid_zelda_world_blob(std::span<const std::uint8_t> blob,
                            std::string* error) {
    if (blob.size() != kZelda1WorldBlobBytes ||
        !zelda1::Zelda1WorldModel::validate_serialized(blob)) {
        set_error(error, "foreign world blob is not a valid Z1WM v2 record");
        return false;
    }
    return true;
}

bool valid_zelda_overworld_session_blob(std::span<const std::uint8_t> blob,
                                        std::string* error) {
    if (blob.size() != kZelda1OverworldSessionBlobBytes ||
        !zelda1::Zelda1OverworldSession::validate_serialized(blob)) {
        set_error(error, "foreign overworld session blob is not a valid migratable Z1OS v5/v6/v7 record");
        return false;
    }
    return true;
}

bool provider_save(void* user, gbarecomp::debug::SnapshotWriter& out,
                   std::string*) {
    const auto* state = static_cast<ForeignWorldNativeState*>(user);
    const auto bytes = state->serialize();
    out.bytes(bytes.data(), bytes.size());
    return true;
}

bool provider_preflight(void*, gbarecomp::debug::SnapshotReader& in,
                        std::string* error) {
    std::vector<std::uint8_t> bytes(in.remaining());
    in.bytes(bytes.data(), bytes.size());
    ForeignWorldNativeState parsed;
    return in.ok() && ForeignWorldNativeState::deserialize(bytes, &parsed, error);
}

void provider_restore(void* user, gbarecomp::debug::SnapshotReader& in) {
    std::vector<std::uint8_t> bytes(in.remaining());
    in.bytes(bytes.data(), bytes.size());
    ForeignWorldNativeState parsed;
    // The engine invoked matching preflight before it restored guest state.
    // A violation here is a programming error; continuing would be unsafe.
    if (!in.ok() || !ForeignWorldNativeState::deserialize(bytes, &parsed, nullptr))
        std::abort();
    static_cast<ForeignWorldNativeState*>(user)->replace_after_provider_restore(
        std::move(parsed));
}

}  // namespace

bool ForeignWorldNativeState::set_zelda1_world_blob(
    std::span<const std::uint8_t> blob, std::string* error) {
    if (!valid_zelda_world_blob(blob, error)) return false;
    zelda1_world_blob_.assign(blob.begin(), blob.end());
    return true;
}

bool ForeignWorldNativeState::set_zelda1_overworld_session_blob(
    std::span<const std::uint8_t> blob, std::string* error) {
    if (!valid_zelda_overworld_session_blob(blob, error)) return false;
    zelda1_overworld_session_blob_.assign(blob.begin(), blob.end());
    return true;
}

void ForeignWorldNativeState::replace_after_provider_restore(
    ForeignWorldNativeState&& restored) {
    const auto next_generation = restore_generation_ + 1;
    inventory_ = std::move(restored.inventory_);
    zelda1_world_blob_ = std::move(restored.zelda1_world_blob_);
    zelda1_overworld_session_blob_ =
        std::move(restored.zelda1_overworld_session_blob_);
    zelda1_presentation_active_ = restored.zelda1_presentation_active_;
    restore_generation_ = next_generation;
}

std::vector<std::uint8_t> ForeignWorldNativeState::serialize() const {
    const auto inventory_bytes = inventory_.serialize();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(4 + 2 + 4 + inventory_bytes.size() + 4 +
                  zelda1_world_blob_.size() + 4 +
                  zelda1_overworld_session_blob_.size() + 1);
    bytes.insert(bytes.end(), kStateMagic.begin(), kStateMagic.end());
    append_u16(&bytes, kStateVersion);
    append_u32(&bytes, static_cast<std::uint32_t>(inventory_bytes.size()));
    bytes.insert(bytes.end(), inventory_bytes.begin(), inventory_bytes.end());
    append_u32(&bytes, static_cast<std::uint32_t>(zelda1_world_blob_.size()));
    bytes.insert(bytes.end(), zelda1_world_blob_.begin(), zelda1_world_blob_.end());
    append_u32(&bytes,
               static_cast<std::uint32_t>(zelda1_overworld_session_blob_.size()));
    bytes.insert(bytes.end(), zelda1_overworld_session_blob_.begin(),
                 zelda1_overworld_session_blob_.end());
    bytes.push_back(zelda1_presentation_active_ ? 1 : 0);
    return bytes;
}

bool ForeignWorldNativeState::deserialize(std::span<const std::uint8_t> bytes,
                                          ForeignWorldNativeState* output,
                                          std::string* error) {
    if (!output) { set_error(error, "foreign native state output is null"); return false; }
    Reader reader(bytes);
    for (const auto expected : kStateMagic) {
        std::uint8_t actual;
        if (!reader.u8(&actual) || actual != expected) {
            set_error(error, "foreign native state magic is invalid"); return false;
        }
    }
    std::uint16_t version;
    std::uint32_t inventory_size, world_size, overworld_size = 0;
    std::uint8_t presentation_active = 0;
    std::vector<std::uint8_t> inventory_bytes, world_bytes, overworld_bytes;
    if (!reader.u16(&version) || (version != 1 && version != 2 && version != kStateVersion) ||
        !reader.u32(&inventory_size) || inventory_size > bytes.size() ||
        !reader.bytes(inventory_size, &inventory_bytes) ||
        !reader.u32(&world_size) || world_size > kZelda1WorldBlobBytes ||
        !reader.bytes(world_size, &world_bytes) ||
        (version >= 2 &&
         (!reader.u32(&overworld_size) ||
          overworld_size > kZelda1OverworldSessionBlobBytes ||
          !reader.bytes(overworld_size, &overworld_bytes))) ||
        (version == kStateVersion &&
         (!reader.u8(&presentation_active) || presentation_active > 1)) ||
        !reader.at_end()) {
        set_error(error, "foreign native state is truncated or malformed"); return false;
    }
    ForeignWorldNativeState parsed;
    if (!InventoryCore::deserialize(inventory_bytes, &parsed.inventory_, error)) return false;
    if (!world_bytes.empty() && !parsed.set_zelda1_world_blob(world_bytes, error)) return false;
    if (!overworld_bytes.empty() &&
        !parsed.set_zelda1_overworld_session_blob(overworld_bytes, error))
        return false;
    parsed.zelda1_presentation_active_ = presentation_active != 0;
    *output = std::move(parsed);
    return true;
}

ForeignWorldNativeState& foreign_world_native_state() {
    static ForeignWorldNativeState state;
    return state;
}

gbarecomp::debug::ModStateProvider make_foreign_world_native_state_provider(
    ForeignWorldNativeState& state) {
    return {kProviderId, kProviderSchema, &state,
            provider_save, provider_preflight, provider_restore};
}

bool register_foreign_world_native_state_provider(std::string* error) {
    // Registry membership freezes at the first snapshot. A feature enabled
    // after that boundary needs a restart; changing the catalog mid-save
    // would invalidate existing snapshots, so fail closed instead.
    static bool registered = false;
    if (registered) return true;
    if (!gbarecomp::debug::register_mod_state_provider(
            make_foreign_world_native_state_provider(foreign_world_native_state()), error))
        return false;
    registered = true;
    return true;
}

}  // namespace minish::foreign_world
