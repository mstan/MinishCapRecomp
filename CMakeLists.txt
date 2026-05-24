cmake_minimum_required(VERSION 3.20)
project(MinishCapRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to the platform core. Override with -DGBARECOMP_ROOT=... if you
# keep the layout differently.
set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../gbarecomp"
    CACHE PATH "Path to gbarecomp platform core")

if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GBARECOMP_ROOT does not point to a gbarecomp checkout: ${GBARECOMP_ROOT}")
endif()

# Pull in the platform core targets. This makes gbarecomp_runtime,
# gbarecomp_debug, gbarecomp_armv4t available as link targets.
add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)

# ─────────────────────────────────────────────────────────────────────
# verify_rom_hash — refuses to launch the game with an unknown ROM.
# ─────────────────────────────────────────────────────────────────────
add_executable(verify_rom_hash tools/verify_rom_hash/main.cpp)

# ─────────────────────────────────────────────────────────────────────
# import_tmc_symbols — pulls symbol + boundary data from a local
# checkout of zeldaret/tmc into symbols/. The decomp source itself
# never enters this repo.
#
# Implemented as a Python script (text scraping is awkward in C++);
# run directly:
#   python tools/import_tmc_symbols/import_tmc_symbols.py
# ─────────────────────────────────────────────────────────────────────

# ─────────────────────────────────────────────────────────────────────
# MinishCapRecomp — the game binary. Links generated/*.c against the
# runtime libs. The Phase 0 scaffold has no generated/*.c yet, so we
# build with just main + game_config to verify the wiring compiles.
# ─────────────────────────────────────────────────────────────────────
file(GLOB MINISH_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")

add_executable(MinishCapRecomp
    src/main.cpp
    src/game_config.cpp
    ${MINISH_GENERATED_SOURCES}
)

target_include_directories(MinishCapRecomp PRIVATE
    "${GBARECOMP_ROOT}/src/armv4t"
    "${GBARECOMP_ROOT}/src/gba"
    "${GBARECOMP_ROOT}/src/runtime"
    "${GBARECOMP_ROOT}/src/debug"
    src
)

target_link_libraries(MinishCapRecomp PRIVATE
    # --start-group/--end-group lets ld re-iterate the gbarecomp static
    # libs to resolve their mutual references (runtime_arm.cpp in
    # armv4t references kBiosDispatchTable defined in runtime, etc.).
    # Without it, the single left-to-right linker scan misses
    # cross-library refs that surface only after both libs have been
    # partially processed.
    "-Wl,--start-group"
    gbarecomp_runtime
    gbarecomp_debug
    gbarecomp_gba
    gbarecomp_armv4t
    "-Wl,--end-group"
)

target_compile_definitions(MinishCapRecomp PRIVATE
    GBARECOMP_DEFAULT_DEBUG_PORT=19842
    GBARECOMP_WINDOW_TITLE="MinishCapRecomp"
    GBARECOMP_DEFAULT_GAME_CONFIG="game.toml"
)

if(MSVC)
    target_link_options(MinishCapRecomp PRIVATE /STACK:16777216)
elseif(MINGW OR WIN32)
    target_link_options(MinishCapRecomp PRIVATE -Wl,--stack,16777216)
endif()
