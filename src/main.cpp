// main.cpp — MinishCapRecomp entry point.
//
// Every gbarecomp game binary takes BOTH a BIOS and a ROM at launch
// (see ../gbarecomp/PRINCIPLES.md "BIOS is sacred"). The CLI accepts:
//
//   MinishCapRecomp [--bios <path>] [--rom <path>] [game.toml]
//
// All three are optional on the command line; missing values are
// pulled from game.toml. Hashes are verified before any code runs.
//
// Windowed play first runs the recomp-ui pre-boot launcher (Dear ImGui;
// see game_launcher_boot.h + gbarecomp's launcher_seam.h) to pick the ROM/BIOS
// and tune settings; the launcher's choices become ordinary CLI args for
// run_game(). Headless/explicit invocations (--tcp/--steps/--frames/
// --no-window/--rom/GBARECOMP_NO_LAUNCHER) bypass it entirely.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"
#include "mobile_platform.h"
#include "minish_extended_view.h"

#if defined(GBAGAME_RECOMP_UI)
#include "game_launcher_boot.h"
#endif

#ifndef GBARECOMP_DEFAULT_GAME_CONFIG
#define GBARECOMP_DEFAULT_GAME_CONFIG "game.toml"
#endif

namespace {

void print_usage() {
    std::printf(
        "MinishCapRecomp [--bios <path>] [--rom <path>] "
        "[game.toml]\n"
        "\n"
        "Both BIOS and ROM are required (either via flags or via the\n"
        "[bios] / [rom] sections of game.toml). The runtime refuses\n"
        "to start unless both hash-verify.\n"
        "\n"
        "Default BIOS path: ../gbarecomp/bios/gba_bios.bin\n"
        "Default game config: game.toml (in CWD)\n"
        "\n"
        "Windowed play opens the pre-boot launcher first; --no-launcher\n"
        "skips it for one run, --launcher forces it past a persisted\n"
        "\"skip launcher\" setting.\n");
}

}  // namespace

int minish_main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    gbarecomp::MobileProcessOptions mobile;
    mobile.game_config = GBARECOMP_DEFAULT_GAME_CONFIG;
    mobile.program_name = "./MinishCapRecomp";
    (void)gbarecomp::mobile_prepare_process(args, mobile);

    // Light arg sniff so we can show a useful usage line. The real
    // parser lives in the runtime; we just want a friendly fail.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    // Built-in defaults so a standalone MinishCapRecomp.exe ships
    // without a sibling game.toml. The picker still validates against
    // these values; CLI / TOML can override.
    gbarecomp::RunOptions opts;
    opts.builtin_game_name  = "The Legend of Zelda: The Minish Cap";
    opts.builtin_rom_sha1   = "b4bd50e4131b027c334547b4524e2dbbd4227130";
    // CRC32 of the pinned USA ROM (same dump the SHA-1 above gates on).
    // The launcher's GAME card uses it for its "ROM verified" check; the
    // asset picker treats it as informational next to the SHA-1 gate.
    opts.builtin_rom_crc32  = 0xABCEBBB1u;
    opts.mod_game_id        = "minish-cap-us";
    opts.mod_owns_adaptive_view = true;
    // The adaptive-view implementation remains game-owned, but its activation
    // is now authoritative in Mods rather than the generic Display settings.
    opts.max_resize_view_width = 480;
    opts.resize_driven_view    = true;
    opts.extended_view_init    = &minish::install_extended_view;
    opts.extended_view_frame   = &minish::update_extended_view;
    opts.launcher_expose_widescreen = false;
    opts.launcher_expose_adaptive_view = false;
    opts.launcher_region    = "USA";
    opts.launcher_game_config = GBARECOMP_DEFAULT_GAME_CONFIG;
#if defined(__ANDROID__)
    opts.ui_touch_friendly = true;
    opts.orientation = gbarecomp::RunOptions::Orientation::Any;
    opts.freely_resizable_window = true;
    opts.resize_view_sizing = gbarecomp::RunOptions::ViewSizing::Density;
    // Keeps even 128px affine clip dimensions representable in their byte
    // reads while exposing authored rows on large or folded surfaces.
    opts.max_resize_view_height = 384;
#endif
    // Save: game.toml [save] has no explicit path — the runtime derives
    // <rom>.sav, and the launcher seam shows the same derivation.

#if defined(GBAGAME_RECOMP_UI)
    if (game_launcher_preboot(args, opts)) return 0;   // user quit the launcher
#endif
    std::vector<char*> av;
    av.reserve(args.size());
    for (auto& s : args) av.push_back(s.data());
    return gbarecomp::run_game(static_cast<int>(av.size()), av.data(), opts);
}

#if defined(__ANDROID__)
extern "C" int SDL_main(int argc, char** argv) {
    return gbarecomp::mobile_run_with_stack(minish_main, argc, argv);
}
#else
int main(int argc, char** argv) {
    return minish_main(argc, argv);
}
#endif
