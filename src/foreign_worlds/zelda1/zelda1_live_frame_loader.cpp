#include "foreign_worlds/zelda1/zelda1_live_frame_loader.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

namespace minish::foreign_world::zelda1 {
namespace {

constexpr std::size_t kInesHeaderSize = 16;
constexpr std::size_t kPrg0PrgSize = 128 * 1024;
constexpr std::array<std::uint8_t, kInesHeaderSize> kPrg0Header{{
    'N', 'E', 'S', 0x1a, 8, 0, 0x12, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
}};

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

bool validate_prg0_ines(std::span<const std::uint8_t> rom, std::string* error) {
    if (rom.size() != kInesHeaderSize + kPrg0PrgSize) {
        set_error(error, "Zelda 1 PRG0 iNES must be exactly 131088 bytes");
        return false;
    }
    if (!std::equal(kPrg0Header.begin(), kPrg0Header.end(), rom.begin())) {
        set_error(error, "iNES header is not the canonical Zelda 1 PRG0 header");
        return false;
    }
    return true;
}

std::optional<FirstQuestData> load_candidate(const std::filesystem::path& path,
                                              std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        set_error(error, "could not open hash-validated Zelda 1 iNES file: " +
                             path.string());
        return std::nullopt;
    }
    std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
    if (!validate_prg0_ines(rom, error)) return std::nullopt;

    std::vector<std::uint8_t> prg(rom.begin() + kInesHeaderSize, rom.end());
    auto verified = VerifiedPrg::from_verified_bytes(std::move(prg), error);
    if (!verified) return std::nullopt;
    return FirstQuestData::create(std::move(*verified), error);
}

}  // namespace

bool Zelda1LiveFrameLoader::load_hash_validated_ines(
    const std::filesystem::path& path, std::string* error) {
    auto candidate = load_candidate(path, error);
    if (!candidate) return false;

    // Commit only after every input and typed-data check passed. A successful
    // ROM replacement invalidates the old frame instead of exposing pixels
    // generated from a different owned PRG.
    data_ = std::move(*candidate);
    framebuffer_.fill(0);
    return true;
}

bool Zelda1LiveFrameLoader::load_and_render_hash_validated_ines(
    const std::filesystem::path& path, std::uint8_t room_id, std::string* error) {
    auto data_candidate = load_candidate(path, error);
    if (!data_candidate) return false;
    std::vector<std::uint8_t> patterns;
    OwFramebuffer frame_candidate{};
    if (!copy_overworld_background_from_verified_prg(*data_candidate, &patterns, error) ||
        !minish::foreign_world::zelda1::render_overworld_room(
            *data_candidate, room_id, patterns, &frame_candidate)) {
        if (error && error->empty())
            *error = "could not render requested Zelda 1 overworld room";
        return false;
    }
    data_ = std::move(*data_candidate);
    framebuffer_ = frame_candidate;
    return true;
}

bool Zelda1LiveFrameLoader::render_overworld_room(std::uint8_t room_id,
                                                   std::string* error) {
    if (!data_) {
        set_error(error, "no Zelda 1 PRG0 iNES data has been loaded");
        return false;
    }
    std::vector<std::uint8_t> patterns;
    OwFramebuffer candidate{};
    if (!copy_overworld_background_from_verified_prg(*data_, &patterns, error) ||
        !minish::foreign_world::zelda1::render_overworld_room(*data_, room_id,
                                                                patterns, &candidate)) {
        if (error && error->empty())
            *error = "could not render requested Zelda 1 overworld room";
        return false;
    }
    framebuffer_ = candidate;
    return true;
}

}  // namespace minish::foreign_world::zelda1
