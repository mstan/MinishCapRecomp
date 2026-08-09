#pragma once

#include "foreign_worlds/zelda1/zelda1_ow_renderer.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace minish::foreign_world::zelda1 {

// Owns the exact PRG payload from a caller's already hash-validated Zelda 1
// PRG0 iNES file.  This deliberately does not search or read an importer
// cache.  A failed load or render leaves the previously usable state intact.
class Zelda1LiveFrameLoader {
public:
    // The caller owns whole-ROM identity validation.  This layer additionally
    // requires the canonical PRG0 iNES shape: 16-byte header, 8 PRG banks,
    // CHR RAM, mapper 1, battery, horizontal mirroring, and no trainer.
    bool load_hash_validated_ines(const std::filesystem::path& path,
                                  std::string* error = nullptr);

    // Preferred activation path: load and render are committed as one unit,
    // so a malformed ROM or an unsupported requested room cannot displace a
    // previously usable data/frame pair.
    bool load_and_render_hash_validated_ines(const std::filesystem::path& path,
                                             std::uint8_t room_id = 0x77,
                                             std::string* error = nullptr);

    // Renders the selected static overworld room to the stable BGR555 frame.
    // For initial activation, pass 0x77.  On failure framebuffer() is
    // unchanged.
    bool render_overworld_room(std::uint8_t room_id, std::string* error = nullptr);

    [[nodiscard]] bool loaded() const { return data_.has_value(); }
    [[nodiscard]] const FirstQuestData* first_quest_data() const {
        return data_ ? &*data_ : nullptr;
    }
    [[nodiscard]] const OwFramebuffer& framebuffer() const { return framebuffer_; }

private:
    std::optional<FirstQuestData> data_;
    OwFramebuffer framebuffer_{};
};

}  // namespace minish::foreign_world::zelda1
