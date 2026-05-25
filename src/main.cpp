// main.cpp — MinishCapRecomp entry point.
//
// Every gbarecomp game binary takes BOTH a BIOS and a ROM at launch
// (see ../gbarecomp/PRINCIPLES.md "BIOS is sacred"). The CLI accepts:
//
//   MinishCapRecomp [--bios <path>] [--rom <path>] [game.toml]
//
// All three are optional on the command line; missing values are
// pulled from game.toml. Hashes are verified before any code runs.

#include <cstdio>
#include <cstring>
#include <string>

#include "runtime.h"

namespace {

void print_usage() {
    std::printf(
        "MinishCapRecomp [--bios <path>] [--rom <path>] [game.toml]\n"
        "\n"
        "Both BIOS and ROM are required (either via flags or via the\n"
        "[bios] / [rom] sections of game.toml). The runtime refuses\n"
        "to start unless both hash-verify.\n"
        "\n"
        "Default BIOS path: ../gbarecomp/bios/gba_bios.bin\n"
        "Default game config: game.toml (in CWD)\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("MinishCapRecomp (Phase 2 scaffold: BIOS bring-up)\n");

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
    opts.builtin_game_name  = "Minish Cap (USA)";
    opts.builtin_rom_sha1   = "b4bd50e4131b027c334547b4524e2dbbd4227130";
    opts.builtin_rom_crc32  = 0x32D19810u;
    return gbarecomp::run_game(argc, argv, opts);
}
