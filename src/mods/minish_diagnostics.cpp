#include "mod_runtime.h"

namespace {
void reset_diagnostics() { gba_mod_set_diagnostic_replay_enabled(0); }
void enable_diagnostics() { gba_mod_set_diagnostic_replay_enabled(1); }
GBA_MOD_CONSTRUCTOR(register_minish_diagnostics) {
    gba_mod_register_reset_callback(reset_diagnostics);
    gba_mod_register_activation_plugin("minish-cap.diagnostics", enable_diagnostics);
}
}
