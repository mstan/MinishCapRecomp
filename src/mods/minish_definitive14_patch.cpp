#include "minish_definitive14.h"
#include "sha1.h"
#include <algorithm>
#include <cstring>

bool minish::apply_definitive14_patch(std::uint8_t* image, std::size_t image_size,
    const std::uint8_t* patch, std::size_t patch_size, const char** error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!image || !patch || image_size != 0x1000000 || patch_size != 26644 ||
        gba::sha1(image, image_size).hex() != "b4bd50e4131b027c334547b4524e2dbbd4227130" ||
        gba::sha1(patch, patch_size).hex() != "1778b00b1595f46cd7cb146e952352e3b6bbd818")
        return fail("Definitive Edition requires the exact USA cartridge and 1.4 IPS patch");
    if (std::memcmp(patch, "PATCH", 5)) return fail("Invalid Definitive Edition patch");
    std::size_t pos = 5;
    auto take16 = [&]() { auto value = (std::size_t(patch[pos]) << 8) | patch[pos+1]; pos += 2; return value; };
    for (;;) {
        if (pos + 3 > patch_size) return fail("Truncated Definitive Edition patch");
        if (!std::memcmp(patch + pos, "EOF", 3)) {
            if (pos + 3 != patch_size) return fail("Unexpected Definitive Edition patch trailer");
            break;
        }
        const std::size_t offset = (std::size_t(patch[pos]) << 16) |
            (std::size_t(patch[pos+1]) << 8) | patch[pos+2];
        pos += 3;
        if (pos + 2 > patch_size) return fail("Truncated Definitive Edition record");
        auto length = take16();
        if (!length) {
            if (pos + 3 > patch_size) return fail("Truncated Definitive Edition run");
            length = take16();
            if (offset > image_size || length > image_size - offset)
                return fail("Definitive Edition run exceeds the cartridge");
            std::fill_n(image + offset, length, patch[pos++]);
        } else {
            if (offset > image_size || length > image_size - offset ||
                pos > patch_size || length > patch_size - pos)
                return fail("Definitive Edition record exceeds its input");
            std::memcpy(image + offset, patch + pos, length);
            pos += length;
        }
    }
    if (gba::sha1(image, image_size).hex() != "5de00d88ecc367c9d374c6b2601caa51a17ca743")
        return fail("Definitive Edition output does not match its compiled program");
    return true;
}
