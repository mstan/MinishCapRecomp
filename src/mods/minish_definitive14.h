#pragma once
#include <cstddef>
#include <cstdint>
namespace gbarecomp { struct RunOptions; }
namespace minish {
void configure_definitive14(gbarecomp::RunOptions& options);
bool definitive14_active();
// Accepts only the exact USA 1.4 author patch and original verified cartridge.
bool apply_definitive14_patch(std::uint8_t* image, std::size_t image_size,
    const std::uint8_t* patch, std::size_t patch_size, const char** error);
}
