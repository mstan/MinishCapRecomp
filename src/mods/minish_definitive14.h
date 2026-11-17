#pragma once
#include <cstddef>
#include <cstdint>
namespace gbarecomp { struct RunOptions; }
namespace minish {
void configure_definitive14(gbarecomp::RunOptions& options);
bool definitive14_active();
// Prepare the built-in native mod from the exact original USA cartridge.
bool prepare_definitive14_image(std::uint8_t* image, std::size_t image_size,
    const char** error);
}
