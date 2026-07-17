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

# ─────────────────────────────────────────────────────────────────────
# recomp-ui pre-boot launcher (Dear ImGui), consumed as a git submodule
# (junctioned locally to the shared checkout). Isolated in its own
# static lib so recomp-ui's sources + RECOMP_LAUNCHER define never
# touch the giant generated/*.c TU flags — only main.cpp (gated on
# GBAGAME_RECOMP_UI) and launcher_boot.cpp know the launcher exists.
# ─────────────────────────────────────────────────────────────────────
option(GBAGAME_RECOMP_UI "Build the recomp-ui pre-boot launcher" ON)
if(GBAGAME_RECOMP_UI)
    include(${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui/recomp_ui.cmake)
    add_library(game_launcher_ui STATIC src/game_launcher_boot.cpp)
    target_include_directories(game_launcher_ui PRIVATE
        "${GBARECOMP_ROOT}/src/runtime"   # launcher_seam.h + runtime.h
        "${GBARECOMP_ROOT}/src/gba"
        src
    )
    # sources, includes, RECOMP_LAUNCHER, asset staging
    recomp_target_launcher_ui(game_launcher_ui
        BOXART ${CMAKE_CURRENT_SOURCE_DIR}/recomp/launcher/boxart.tga)
    find_package(SDL2 REQUIRED)
    target_link_libraries(game_launcher_ui PUBLIC SDL2::SDL2)
    if(WIN32)
        target_link_libraries(game_launcher_ui PUBLIC opengl32)
    endif()
    target_link_libraries(MinishCapRecomp PRIVATE game_launcher_ui)
    set_property(SOURCE src/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS GBAGAME_RECOMP_UI)
endif()

if(MSVC)
    target_link_options(MinishCapRecomp PRIVATE /STACK:16777216)
elseif(MINGW OR WIN32)
    target_link_options(MinishCapRecomp PRIVATE -Wl,--stack,16777216)
endif()
