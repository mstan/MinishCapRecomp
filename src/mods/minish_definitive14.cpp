#include "minish_definitive14.h"
#include "mod_runtime.h"
#include "runtime.h"
#include <fstream>
#include <filesystem>
#include <iterator>
#include <vector>

extern "C" const RuntimeNativeProgram de14_kNativeProgram;
namespace {
bool enabled = false;
bool prepared = false;
void reset_definitive() { enabled = false; prepared = false; }
void activate_definitive() { enabled = true; }
const gbarecomp::ModNativeProgramProfile profiles[] = {
    {"5de00d88ecc367c9d374c6b2601caa51a17ca743", &de14_kNativeProgram, "definitive-1-4",
     "minish-cap.gameplay.definitive-edition", "definitive-edition"},
};
gbarecomp::ModRomDataResult prepare(std::uint8_t* data, std::size_t size, const char** error) {
    using gbarecomp::ModRomDataResult;
    prepared = false;
    if (!enabled) return ModRomDataResult::Unchanged;
    const auto* path = gba_mod_required_asset_path(
        "minish-cap.gameplay.definitive-edition", "usa-1-4-patch");
    if (!path) {
        if (error) *error = "Select the exact Definitive Edition USA 1.4 IPS in Mods";
        return ModRomDataResult::Failed;
    }
    const auto file = std::filesystem::u8path(path);
    std::error_code ec;
    if (std::filesystem::file_size(file, ec) != 26644 || ec) {
        if (error) *error = "Definitive Edition USA 1.4 IPS must be exactly 26644 bytes";
        return ModRomDataResult::Failed;
    }
    std::ifstream stream(file, std::ios::binary);
    std::vector<std::uint8_t> patch(26644);
    stream.read(reinterpret_cast<char*>(patch.data()), patch.size());
    if (!stream || stream.peek() != std::char_traits<char>::eof()) {
        if (error) *error = "Cannot read the selected Definitive Edition patch";
        return ModRomDataResult::Failed;
    }
    prepared = minish::apply_definitive14_patch(data, size, patch.data(), patch.size(), error);
    return prepared ? ModRomDataResult::PreparedNativeProgram : ModRomDataResult::Failed;
}
GBA_MOD_CONSTRUCTOR(register_definitive14) {
    gba_mod_register_reset_callback(reset_definitive);
    gba_mod_register_activation_plugin("minish-cap.definitive-edition-1-4", activate_definitive);
}
}
bool minish::definitive14_active() { return prepared; }
void minish::configure_definitive14(gbarecomp::RunOptions& options) {
    prepared = false;
    options.prepare_mod_rom_data = prepare;
    options.mod_native_programs = profiles;
    options.mod_native_program_count = 1;
}
