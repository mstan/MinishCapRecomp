#include "minish_definitive14.h"
#include "mod_runtime.h"
#include "runtime.h"

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
    prepared = minish::prepare_definitive14_image(data, size, error);
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
