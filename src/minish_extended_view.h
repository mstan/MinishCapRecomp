#pragma once

#include <cstdint>

namespace gbarecomp { struct ExtendedViewFrameInfo; struct RunOptions; }

namespace minish {

// Keep non-room screens in their original native presentation.
void configure_scene_presentation(gbarecomp::RunOptions& options);

// Installs the Minish Cap room-map source used only by expanded PPU margins.
void install_extended_view(std::uint32_t extra_left,
                           std::uint32_t extra_right);

void update_extended_view(const gbarecomp::ExtendedViewFrameInfo* frame);

}  // namespace minish
