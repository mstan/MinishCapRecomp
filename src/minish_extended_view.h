#pragma once

#include <cstdint>

namespace minish {

// Installs the Minish Cap room-map source used only by expanded PPU margins.
void install_extended_view(std::uint32_t extra_left,
                           std::uint32_t extra_right);

}  // namespace minish
