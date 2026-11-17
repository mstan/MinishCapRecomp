#include "minish_definitive14.h"
#include "sha1.h"
#include "de14_rom_data.h"
#include <cstring>

bool minish::prepare_definitive14_image(std::uint8_t* image,
    std::size_t image_size, const char** error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!image || image_size != 0x1000000 ||
        gba::sha1(image, image_size).hex() != "b4bd50e4131b027c334547b4524e2dbbd4227130")
        return fail("Definitive Edition requires the supported original USA cartridge");

    // Validate the generated edit table before touching the player's image.
    std::size_t total = 0;
    for (const auto& edit : definitive14_data::edits) {
        if (edit.offset > image_size || edit.size > image_size - edit.offset ||
            total > sizeof(definitive14_data::bytes) ||
            edit.size > sizeof(definitive14_data::bytes) - total)
            return fail("The built-in Definitive Edition data is invalid");
        total += edit.size;
    }
    if (total != sizeof(definitive14_data::bytes))
        return fail("The built-in Definitive Edition data is incomplete");
    total = 0;
    for (const auto& edit : definitive14_data::edits) {
        std::memcpy(image + edit.offset, definitive14_data::bytes + total, edit.size);
        total += edit.size;
    }
    if (gba::sha1(image, image_size).hex() != "5de00d88ecc367c9d374c6b2601caa51a17ca743")
        return fail("Definitive Edition data does not match its compiled program");
    return true;
}
