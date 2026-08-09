cmake_minimum_required(VERSION 3.20)
project(MinishCapRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)

# Path to the platform core, consumed as a git submodule (junctioned locally
# to the shared checkout). Override with -DGBARECOMP_ROOT=... for a different
# layout (e.g. a sibling ../gbarecomp checkout).
set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp"
    CACHE PATH "Path to gbarecomp platform core (git submodule)")

if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GBARECOMP_ROOT does not point to a gbarecomp checkout: ${GBARECOMP_ROOT}")
endif()

# Expose recomp-ui's shared in-game model to the platform runtime. The game
# still owns which capabilities it advertises through RunOptions.
set(GBARECOMP_RUNTIME_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui"
    CACHE PATH "recomp-ui root used by the live GBA runtime adapter")
set(RECOMP_UI_ROOT "${GBARECOMP_RUNTIME_UI_ROOT}"
    CACHE PATH "Path to the shared recomp-ui checkout")

# Minish Cap is the first GBA title to expose game-owned enhancements through
# the package catalog. This also opens recomp-ui's otherwise inert Mods gate.
set(GBARECOMP_ENABLE_MODS ON CACHE BOOL
    "Enable Minish Cap's built-in mod catalog and GBA mod packages" FORCE)

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
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled.cpp")
    message(FATAL_ERROR
        "Stale monolithic generated/recompiled.cpp found. Regenerate with "
        "symbols/minishcap.toml and the current gbarecomp before building.")
endif()
file(GLOB MINISH_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")

add_executable(MinishCapRecomp
    src/main.cpp
    src/game_config.cpp
    src/minish_extended_view.cpp
    src/mods/minish_adaptive_view_plugin.cpp
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
    include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
    add_library(game_launcher_ui STATIC src/game_launcher_boot.cpp)
    target_include_directories(game_launcher_ui PRIVATE
        "${GBARECOMP_ROOT}/src/runtime"   # launcher_seam.h + runtime.h
        "${GBARECOMP_ROOT}/src/gba"
        src
    )
    target_compile_definitions(game_launcher_ui PRIVATE
        GBARECOMP_ENABLE_MODS=1)
    # sources, includes, RECOMP_LAUNCHER, asset staging
    recomp_target_launcher_ui(game_launcher_ui
        CONSOLE gba
        BOXART ${CMAKE_CURRENT_SOURCE_DIR}/recomp/launcher/boxart.tga)
    find_package(SDL2 REQUIRED)
    target_link_libraries(game_launcher_ui PUBLIC SDL2::SDL2)
    if(WIN32)
        target_link_libraries(game_launcher_ui PUBLIC opengl32)
    endif()
    target_link_libraries(MinishCapRecomp PRIVATE game_launcher_ui)
    set_property(SOURCE src/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS GBAGAME_RECOMP_UI)
endif()

# Ship the game-owned, disabled-by-default adaptive-view package beside the
# executable. User-installed .gbamod archives join this same catalog.
set(MINISH_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")
if(EXISTS "${MINISH_PRELOADED_MODS}/packages")
    add_custom_command(TARGET MinishCapRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MINISH_PRELOADED_MODS}"
            "$<TARGET_FILE_DIR:MinishCapRecomp>/mods"
        COMMENT "Preloading the Minish Cap mod catalog")
endif()

if(BUILD_TESTING)
    add_library(minish_foreign_world_core STATIC
        src/foreign_worlds/foreign_world.cpp)
    target_include_directories(minish_foreign_world_core PUBLIC src)

    add_executable(minish_foreign_world_test
        tests/test_foreign_world.cpp)
    target_link_libraries(minish_foreign_world_test PRIVATE
        minish_foreign_world_core)
    add_test(NAME minish_foreign_world COMMAND minish_foreign_world_test)

    add_library(minish_zelda1_first_quest_data STATIC
        src/foreign_worlds/zelda1/zelda1_first_quest_data.cpp)
    target_include_directories(minish_zelda1_first_quest_data PUBLIC src)

    add_executable(minish_zelda1_first_quest_data_test
        tests/test_zelda1_first_quest_data.cpp)
    target_link_libraries(minish_zelda1_first_quest_data_test PRIVATE
        minish_zelda1_first_quest_data)
    add_test(NAME minish_zelda1_first_quest_data
        COMMAND minish_zelda1_first_quest_data_test)

    add_executable(minish_preloaded_mods_test
        tests/test_preloaded_mods.cpp
        "${GBARECOMP_ROOT}/src/runtime/mod_runtime.cpp"
        "${GBARECOMP_ROOT}/src/gba/crc32.cpp"
        "${GBARECOMP_ROOT}/src/gba/sha1.cpp")
    target_compile_definitions(minish_preloaded_mods_test PRIVATE
        GBARECOMP_MOD_UI=1)
    target_include_directories(minish_preloaded_mods_test PRIVATE
        "${GBARECOMP_ROOT}/src/runtime"
        "${GBARECOMP_ROOT}/src/gba"
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui/src")
    add_test(NAME minish_preloaded_mods
        COMMAND minish_preloaded_mods_test "${MINISH_PRELOADED_MODS}")
endif()

if(MSVC)
    target_link_options(MinishCapRecomp PRIVATE /STACK:16777216)
elseif(MINGW OR WIN32)
    target_link_options(MinishCapRecomp PRIVATE -Wl,--stack,16777216)
endif()
