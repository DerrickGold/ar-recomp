cmake_minimum_required(VERSION 3.16)
project(ActRaiserRecomp C)

# A bare configure (no preset, no -DCMAKE_BUILD_TYPE) would otherwise build
# with NO optimization and no -g. Default to the optimized play config; the
# CMakePresets.json presets and any explicit -D still override this.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    message(STATUS "No build type given — defaulting to RelWithDebInfo")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# MSVC's C runtime provides the standard math functions without a separate
# libm. Unix-like toolchains still require -lm. An interface target keeps that
# distinction in one place and remains a valid link item on every platform.
add_library(actraiser_math INTERFACE)
if(NOT MSVC)
    target_link_libraries(actraiser_math INTERFACE m)
endif()
include(CTest)

if(BUILD_TESTING)
    add_test(NAME actraiser_runner_private_boundary
    COMMAND ${CMAKE_COMMAND}
            -DGAME_SOURCE_ROOT=${CMAKE_SOURCE_DIR}/src
            -DSNESRECOMP_RUNNER_PRIVATE_ROOT=${CMAKE_SOURCE_DIR}/snesrecomp-go/runtime/src
            -P ${CMAKE_SOURCE_DIR}/tools/check_runner_boundary.cmake)
endif()

# Configure the ROM-free test tier without requiring generated ROM banks. This
# skips the game target; tests link only the hand-written sources they exercise.
option(AR_TESTS_ONLY "Configure only the ROM-free test tier (no ROM required)" OFF)

# Audited height field for the full-town SIM ground and every grounded
# presentation consumer. Shipped builds enable it; pass
# -DAR_SIM3D_TERRAIN_ELEVATION=OFF (or use the control preset) for a strict
# flat-terrain A/B control. The numeric value is shared with ROM-free settings
# tests, so define it outside the game-target-only branch below.
option(AR_SIM3D_TERRAIN_ELEVATION
    "Render the audited SIM town elevation mesh" ON)
if(AR_SIM3D_TERRAIN_ELEVATION)
    set(AR_SIM3D_TERRAIN_ELEVATION_VALUE 1)
else()
    set(AR_SIM3D_TERRAIN_ELEVATION_VALUE 0)
endif()

set(SNESRECOMP_GO_ROOT ${CMAKE_SOURCE_DIR}/snesrecomp-go)
include(${SNESRECOMP_GO_ROOT}/runtime/runner.cmake)
message(STATUS "snesrecomp runner: independent portable runtime")
add_library(snesrecomp_sdk_headers INTERFACE)
target_include_directories(snesrecomp_sdk_headers INTERFACE
    ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS})
add_library(snesrecomp::sdk ALIAS snesrecomp_sdk_headers)

# Architecture-specific paths are optional accelerators. The compiler's
# target macros decide which implementation is available, which is correct
# for native and cross builds alike. This switch can force the portable C11
# implementation without changing source files or removing their test oracle.
if(SNESRECOMP_ENABLE_SIMD)
    message(STATUS "snesrecomp SIMD: enabled when supported by build target")
else()
    message(STATUS "snesrecomp SIMD: portable implementation forced")
endif()

find_package(SDL3 3.4.0 REQUIRED)

set(ACTRAISER_GEN_DIR "${CMAKE_SOURCE_DIR}/src/gen" CACHE PATH
    "Directory containing generated recomp C translation units")
file(GLOB ACTRAISER_GEN_SOURCES CONFIGURE_DEPENDS "${ACTRAISER_GEN_DIR}/*.c")
if(NOT ACTRAISER_GEN_SOURCES AND NOT AR_TESTS_ONLY)
    message(FATAL_ERROR
        "${ACTRAISER_GEN_DIR} is empty — run `snesbuild regen --root . "
        "--rom ar.sfc` (or the compatibility tools/regen.sh wrapper) before building. "
        "(Configure with -DAR_TESTS_ONLY=ON to build only the ROM-free test tier.)")
endif()

# CMake updates the target when regeneration replaces bankXX_v2.c with
# bankXX_partNN_v2.c, but Make/Ninja leave the removed source's old .o/.d files
# on disk. They are not linked, yet the large debug objects can waste hundreds
# of MiB and make build inspection misleading. On every configure, remove only
# generated-bank artifacts whose matching source basename no longer exists;
# active incremental objects are deliberately preserved.
set(_AR_GEN_SOURCE_NAMES)
foreach(_src IN LISTS ACTRAISER_GEN_SOURCES)
    get_filename_component(_name "${_src}" NAME)
    list(APPEND _AR_GEN_SOURCE_NAMES "${_name}")
endforeach()

set(_AR_GEN_OBJ_DIR
    "${CMAKE_BINARY_DIR}/CMakeFiles/ActRaiserRecomp.dir/src/gen")
if(IS_DIRECTORY "${_AR_GEN_OBJ_DIR}")
    file(GLOB _AR_GEN_OBJECT_ARTIFACTS
        "${_AR_GEN_OBJ_DIR}/bank*_v2.c.o"
        "${_AR_GEN_OBJ_DIR}/bank*_v2.c.o.d"
        "${_AR_GEN_OBJ_DIR}/bank*_v2.c.obj")
    set(_AR_STALE_COUNT 0)
    set(_AR_STALE_BYTES 0)
    foreach(_artifact IN LISTS _AR_GEN_OBJECT_ARTIFACTS)
        get_filename_component(_artifact_name "${_artifact}" NAME)
        string(REGEX REPLACE "\\.(o|obj)(\\.d)?$" ""
            _source_name "${_artifact_name}")
        if(NOT _source_name IN_LIST _AR_GEN_SOURCE_NAMES)
            file(SIZE "${_artifact}" _artifact_size)
            math(EXPR _AR_STALE_BYTES
                "${_AR_STALE_BYTES} + ${_artifact_size}")
            math(EXPR _AR_STALE_COUNT "${_AR_STALE_COUNT} + 1")
            file(REMOVE "${_artifact}")
        endif()
    endforeach()
    if(_AR_STALE_COUNT GREATER 0)
        math(EXPR _AR_STALE_MIB
            "(${_AR_STALE_BYTES} + 1048575) / 1048576")
        message(STATUS
            "ActRaiserRecomp: removed ${_AR_STALE_COUNT} stale generated "
            "build artifact(s), ${_AR_STALE_MIB} MiB")
    endif()
endif()

# The ActRaiserRecomp game target links the generated ROM banks + funcs.h, so it
# is skipped entirely under AR_TESTS_ONLY (a ROM-free checkout). The BUILD_TESTING
# test tier below is independent of this target and always available.
if(NOT AR_TESTS_ONLY)

# snesbuild.ini is the sole game-source list for both local builds and shipped
# hermetic rebuilds. A second CMake list once let a local build pass while the
# bundle failed to link. Add sources to the manifest; its paths are relative to
# this directory, and CMAKE_CONFIGURE_DEPENDS picks up edits automatically.
set(ACTRAISER_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/snesbuild.ini)
if(NOT EXISTS ${ACTRAISER_MANIFEST})
    message(FATAL_ERROR "snesbuild.ini is missing; it holds the game source list")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ACTRAISER_MANIFEST})

file(STRINGS ${ACTRAISER_MANIFEST} ACTRAISER_MANIFEST_SOURCE_LINES
    REGEX "^[ \t]*source[ \t]*=")
set(ACTRAISER_GAME_SOURCES "")
foreach(_ar_line IN LISTS ACTRAISER_MANIFEST_SOURCE_LINES)
    string(REGEX REPLACE "^[ \t]*source[ \t]*=[ \t]*" "" _ar_source "${_ar_line}")
    string(STRIP "${_ar_source}" _ar_source)
    if(NOT _ar_source STREQUAL "")
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_ar_source})
            message(FATAL_ERROR
                "snesbuild.ini lists ${_ar_source}, which does not exist")
        endif()
        list(APPEND ACTRAISER_GAME_SOURCES ${_ar_source})
    endif()
endforeach()
# An empty list would otherwise link a game with no game in it.
if(ACTRAISER_GAME_SOURCES STREQUAL "")
    message(FATAL_ERROR "snesbuild.ini declares no `source =` entries")
endif()

add_library(snesrecomp_runtime STATIC
    ${SNESRECOMP_RUNNER_SOURCES})
snesrecomp_configure_runtime_target(snesrecomp_runtime)
add_library(snesrecomp::runtime ALIAS snesrecomp_runtime)

add_executable(ActRaiserRecomp
    # game-specific runtime, from snesbuild.ini (see above)
    ${ACTRAISER_GAME_SOURCES}
    # recompiled banks (generated)
    ${ACTRAISER_GEN_SOURCES}
)

target_include_directories(ActRaiserRecomp PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/recomp
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
)

if(SNESRECOMP_ENABLE_TRACE)
    set(_SNESRECOMP_TRACE 1)
else()
    set(_SNESRECOMP_TRACE 0)
endif()

# SNESRECOMP_WATCHDOG: the 5s hang watchdog. Bring-up instrumentation — it exists to
# catch an infinite loop in recompiled code during porting, and once a build is
# verified there is nothing left for it to find. It is not free: the generated
# code calls WatchdogCheck at EVERY loop header (thousands of sites per bank),
# and escaping a stuck frame necessarily abandons it. Default ON for the
# development configurations and OFF for Release, so shipping builds carry
# neither the per-loop-header clock check nor the abandon path.
if(NOT DEFINED SNESRECOMP_WATCHDOG)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        set(_SNESRECOMP_WATCHDOG_DEFAULT OFF)
    else()
        set(_SNESRECOMP_WATCHDOG_DEFAULT ON)
    endif()
else()
    set(_SNESRECOMP_WATCHDOG_DEFAULT ${SNESRECOMP_WATCHDOG})
endif()
option(SNESRECOMP_WATCHDOG "Build the 5s hang watchdog (dev instrumentation)"
       ${_SNESRECOMP_WATCHDOG_DEFAULT})
if(SNESRECOMP_WATCHDOG)
    set(_SNESRECOMP_WATCHDOG 1)
    message(STATUS "ActRaiserRecomp: hang watchdog ENABLED (dev build)")
else()
    set(_SNESRECOMP_WATCHDOG 0)
    message(STATUS "ActRaiserRecomp: hang watchdog omitted (release build)")
endif()

target_compile_definitions(ActRaiserRecomp PRIVATE
    SYSTEM_VOLUME_MIXER_AVAILABLE=0
    SNESRECOMP_TRACE=${_SNESRECOMP_TRACE}
    SNESRECOMP_REVERSE_DEBUG=0
    SNESRECOMP_WATCHDOG=${_SNESRECOMP_WATCHDOG}
)
target_compile_definitions(snesrecomp_runtime PRIVATE
    SYSTEM_VOLUME_MIXER_AVAILABLE=0
    SNESRECOMP_TRACE=${_SNESRECOMP_TRACE}
    SNESRECOMP_REVERSE_DEBUG=0
    SNESRECOMP_WATCHDOG=${_SNESRECOMP_WATCHDOG}
)

if(NOT MSVC)
    # Warnings are worth having on the code we write and worthless on the code
    # we generate: src/gen is roughly 8,000 warnings of machine-emitted 65816
    # translation, which would bury the ~10 the hand-written sources produce.
    # The blanket `-w` that used to sit on the whole target hid both -- and hid
    # two calls with no prototype in scope, which C silently gives `int f()`
    # and which therefore had the wrong return type.
    #
    # -w wins over -Wall in clang regardless of order, so the polarity has to
    # be this way round: real warnings on the target, `-w` re-applied to the
    # generated sources per file. runtime is maintained here and keeps
    # its warnings visible.
    target_compile_options(ActRaiserRecomp PRIVATE
        -Wall -Wextra
        # Designated initializers are the house style for the settings
        # descriptor registry; 219 of the 224 hits are that one table.
        -Wno-missing-field-initializers
        # Third-party: the snesrecomp runtime's types.h carries a pragma clang
        # does not know.
        -Wno-unknown-pragmas
        -Wno-unused-parameter
        # A missing prototype is an error, not a warning. Every hand-written
        # source passes; see docs/code-style.md "Verifying a change".
        -Werror=implicit-function-declaration
        -Werror=implicit-int)
    set_source_files_properties(
        ${ACTRAISER_GEN_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# AR_SIM3D_PICKER_TOPDOWN: restore the authentic flat view while a SIM position
# picker is active (Direct the People / Building Direction and every targeted
# miracle). Off while the projected ground is evaluated for targeting -- the
# path is retained, not deleted. Gameplay, $7F:9215, D-pad targeting, and the
# selected cell are identical either way; only those frames' presentation
# changes. The sim3d_demo checkpoints read the compiled value back out of the
# D1 trace and assert the matching picker contract, so neither setting leaves
# the picker frames unverified.
option(AR_SIM3D_PICKER_TOPDOWN
    "Use the authentic top-down view while a SIM map picker is active" OFF)
if(AR_SIM3D_PICKER_TOPDOWN)
    target_compile_definitions(ActRaiserRecomp PRIVATE AR_SIM3D_PICKER_TOPDOWN=1)
else()
    target_compile_definitions(ActRaiserRecomp PRIVATE AR_SIM3D_PICKER_TOPDOWN=0)
endif()

target_compile_definitions(ActRaiserRecomp PRIVATE
    AR_SIM3D_TERRAIN_ELEVATION=${AR_SIM3D_TERRAIN_ELEVATION_VALUE})

# AddressSanitizer + UndefinedBehaviorSanitizer for memory-corruption hunts.
# Off by default because the instrumentation has a substantial runtime cost.
option(AR_SANITIZE "Build with ASan + UBSan (debug corruption bugs)" OFF)
if(AR_SANITIZE AND NOT MSVC)
    target_compile_options(ActRaiserRecomp PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(ActRaiserRecomp PRIVATE
        -fsanitize=address,undefined)
    message(STATUS "ActRaiserRecomp: AR_SANITIZE on — ASan+UBSan enabled")
endif()

# ThreadSanitizer for concurrency diagnostics. It is mutually exclusive with
# AR_SANITIZE and off by default because of its runtime cost.
option(AR_TSAN "Build with ThreadSanitizer" OFF)
if(AR_TSAN AND NOT MSVC)
    if(AR_SANITIZE)
        message(FATAL_ERROR "AR_TSAN and AR_SANITIZE cannot both be ON")
    endif()
    target_compile_options(ActRaiserRecomp PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(ActRaiserRecomp PRIVATE
        -fsanitize=thread)
    message(STATUS "ActRaiserRecomp: AR_TSAN on — ThreadSanitizer enabled")
endif()

target_link_libraries(ActRaiserRecomp PRIVATE
    snesrecomp::runtime
    SDL3::SDL3
    actraiser_math
)

endif() # NOT AR_TESTS_ONLY

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)

    # The production HLE fatal path escapes through the game coroutine. Unit
    # targets link only the formatter/trampoline core; invalid direct calls
    # retain abort semantics unless a focused test registers its own escape.
    add_library(actraiser_hle_fatal_test_support STATIC
        src/actraiser/actraiser_hle_fatal.c)
    target_include_directories(actraiser_hle_fatal_test_support PUBLIC
        ${CMAKE_SOURCE_DIR}/src)

    add_executable(actraiser_consolidated_utils_test
        tests/consolidated_utils_test.c
        src/manifest_utils.c
    )
    target_include_directories(actraiser_consolidated_utils_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME actraiser_consolidated_utils
        COMMAND actraiser_consolidated_utils_test)

    add_executable(actraiser_action_load_pacing_test
        tests/action_load_pacing_test.c
    )
    target_include_directories(actraiser_action_load_pacing_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
    )
    add_test(NAME actraiser_action_load_pacing
        COMMAND actraiser_action_load_pacing_test)

    add_executable(actraiser_host_audio_pause_test
        tests/host_audio_pause_test.c
        src/host/host_audio.c
    )
    target_include_directories(actraiser_host_audio_pause_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/host
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_host_audio_pause_test PRIVATE
        SDL3::SDL3
        actraiser_math
    )
    add_test(NAME actraiser_host_audio_pause
        COMMAND actraiser_host_audio_pause_test)
    set_tests_properties(actraiser_host_audio_pause PROPERTIES
        ENVIRONMENT "SDL_AUDIODRIVER=dummy")

    add_executable(actraiser_host_display_pacing_test
        tests/host_display_pacing_test.c
        src/host/host_display_pacing.c
    )
    target_include_directories(actraiser_host_display_pacing_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/host
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_host_display_pacing
        COMMAND actraiser_host_display_pacing_test)

    add_executable(actraiser_host_display_refresh_cache_test
        tests/host_display_refresh_cache_test.c
        src/host/host_display_refresh_cache.c
    )
    target_include_directories(actraiser_host_display_refresh_cache_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/host
    )
    add_test(NAME actraiser_host_display_refresh_cache
        COMMAND actraiser_host_display_refresh_cache_test)

    add_executable(actraiser_input_replay_test
        tests/input_replay_test.c
        src/input_replay.c
    )
    target_include_directories(actraiser_input_replay_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_input_replay COMMAND actraiser_input_replay_test)

    add_executable(actraiser_scene3d_math_test
        tests/scene3d_math_test.c
        src/scene3d_math.c
    )
    target_include_directories(actraiser_scene3d_math_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(actraiser_scene3d_math_test PRIVATE actraiser_math)
    add_test(NAME actraiser_scene3d_math COMMAND actraiser_scene3d_math_test)

    add_executable(actraiser_presentation_upload_mirror_test
        tests/presentation_upload_mirror_test.c
        src/presentation_upload_mirror.c
    )
    target_include_directories(actraiser_presentation_upload_mirror_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(actraiser_presentation_upload_mirror_test PRIVATE
        SDL3::SDL3
    )
    add_test(NAME actraiser_presentation_upload_mirror
        COMMAND actraiser_presentation_upload_mirror_test)

    add_executable(actraiser_camera_orbit_test
        tests/camera_orbit_test.c
        src/camera_orbit.c
    )
    target_include_directories(actraiser_camera_orbit_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(actraiser_camera_orbit_test PRIVATE actraiser_math)
    add_test(NAME actraiser_camera_orbit COMMAND actraiser_camera_orbit_test)

    add_executable(actraiser_sim_town_canvas_test
        tests/sim_town_canvas_test.c
        src/sim/sim_town_canvas.c
    )
    target_include_directories(actraiser_sim_town_canvas_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_town_canvas
        COMMAND actraiser_sim_town_canvas_test)

    add_executable(actraiser_sim_town_terrain_test
        tests/sim_town_terrain_test.c
        src/sim/sim_town_terrain.c
    )
    target_include_directories(actraiser_sim_town_terrain_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
    )
    target_link_libraries(actraiser_sim_town_terrain_test PRIVATE
        actraiser_math)
    add_test(NAME actraiser_sim_town_terrain
        COMMAND actraiser_sim_town_terrain_test)

    add_executable(actraiser_sim_background_voxels_test
        tests/sim_background_voxels_test.c
        src/sim/sim_town_layout.c
        src/sim/sim_background_voxels.c
        src/sim/sim_structure_visuals.c
        src/sim/sim_background_voxel_models.c
        src/sim/sim_background_voxel_landmarks.c
        src/sim/sim_background_mountains.c
        src/sim/sim_background_mountain_silhouette.c
        src/sim/sim_background_mountain_relief.c
        src/sim/sim_background_voxel_region.c
        src/sim/sim_town_canvas.c
        src/sim/sim_town_terrain.c
    )
    target_include_directories(actraiser_sim_background_voxels_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_sim_background_voxels_test PRIVATE
        actraiser_math)
    add_test(NAME actraiser_sim_background_voxels
        COMMAND actraiser_sim_background_voxels_test)

    add_executable(actraiser_sim_background_voxel_landmarks_test
        tests/sim_background_voxel_landmarks_test.c
        src/sim/sim_town_layout.c
        src/sim/sim_background_voxel_landmarks.c
    )
    target_include_directories(
        actraiser_sim_background_voxel_landmarks_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
    )
    add_test(NAME actraiser_sim_background_voxel_landmarks
        COMMAND actraiser_sim_background_voxel_landmarks_test)

    add_executable(actraiser_sim_background_mountains_test
        tests/sim_background_mountains_test.c
        src/sim/sim_town_layout.c
        src/sim/sim_background_mountains.c
        src/sim/sim_background_mountain_objects.c
    )
    target_include_directories(actraiser_sim_background_mountains_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
    )
    add_test(NAME actraiser_sim_background_mountains
        COMMAND actraiser_sim_background_mountains_test)

    add_executable(actraiser_sim_background_mountain_relief_test
        tests/sim_background_mountain_relief_test.c
        src/sim/sim_background_mountain_relief.c
    )
    target_include_directories(
        actraiser_sim_background_mountain_relief_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
    )
    add_test(NAME actraiser_sim_background_mountain_relief
        COMMAND actraiser_sim_background_mountain_relief_test)

    add_executable(actraiser_sim_background_voxel_models_test
        tests/sim_background_voxel_models_test.c
        src/sim/sim_background_voxel_models.c
        src/sim/sim_background_voxel_region.c
        src/sim/sim_structure_visuals.c
    )
    target_include_directories(actraiser_sim_background_voxel_models_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_background_voxel_models
        COMMAND actraiser_sim_background_voxel_models_test)

    add_executable(actraiser_sim_background_voxel_biome_test
        tests/sim_background_voxel_biome_test.c
        src/sim/sim_background_voxel_biome.c
    )
    target_include_directories(actraiser_sim_background_voxel_biome_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_sim_background_voxel_biome_test PRIVATE
        actraiser_math)
    add_test(NAME actraiser_sim_background_voxel_biome
        COMMAND actraiser_sim_background_voxel_biome_test)

    add_executable(actraiser_sim_background_voxel_model_cache_test
        tests/sim_background_voxel_model_cache_test.c
        src/sim/sim_background_voxel_model_cache.c
        src/sim/sim_background_voxel_models.c
        src/sim/sim_background_voxel_region.c
        src/sim/sim_background_voxel_lighting.c
        src/sim/sim_background_voxel_surface.c
        src/sim/sim_background_voxel_biome.c
    )
    target_include_directories(actraiser_sim_background_voxel_model_cache_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_background_voxel_model_cache
        COMMAND actraiser_sim_background_voxel_model_cache_test)

    add_executable(actraiser_sim_background_voxel_lod_test
        tests/sim_background_voxel_lod_test.c
        src/sim/sim_background_voxel_lod.c
    )
    target_include_directories(actraiser_sim_background_voxel_lod_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
    )
    add_test(NAME actraiser_sim_background_voxel_lod
        COMMAND actraiser_sim_background_voxel_lod_test)

    add_executable(actraiser_sim_background_voxel_preset_test
        tests/sim_background_voxel_preset_test.c
        src/sim/sim_background_voxel_preset.c
    )
    target_include_directories(actraiser_sim_background_voxel_preset_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
    )
    add_test(NAME actraiser_sim_background_voxel_preset
        COMMAND actraiser_sim_background_voxel_preset_test)

    add_executable(actraiser_sim_background_voxel_palette_test
        tests/sim_background_voxel_palette_test.c
        src/sim/sim_background_voxel_palette.c
        src/sim/sim_background_voxel_region.c
    )
    target_include_directories(actraiser_sim_background_voxel_palette_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_background_voxel_palette
        COMMAND actraiser_sim_background_voxel_palette_test)

    add_executable(actraiser_sim_background_voxel_lighting_test
        tests/sim_background_voxel_lighting_test.c
        src/sim/sim_background_voxel_lighting.c
        src/sim/sim_background_voxel_surface.c
    )
    target_include_directories(actraiser_sim_background_voxel_lighting_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_sim_background_voxel_lighting_test PRIVATE
        actraiser_math)
    add_test(NAME actraiser_sim_background_voxel_lighting
        COMMAND actraiser_sim_background_voxel_lighting_test)

    add_executable(actraiser_sim_background_voxel_proportions_test
        tests/sim_background_voxel_proportions_test.c
        src/sim/sim_background_voxel_models.c
        src/sim/sim_background_voxel_proportions.c
        src/sim/sim_background_voxel_region.c
    )
    target_include_directories(actraiser_sim_background_voxel_proportions_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_background_voxel_proportions
        COMMAND actraiser_sim_background_voxel_proportions_test)

    add_executable(actraiser_sim_background_voxel_region_test
        tests/sim_background_voxel_region_test.c
        src/sim/sim_background_voxel_region.c
    )
    target_include_directories(actraiser_sim_background_voxel_region_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
    )
    add_test(NAME actraiser_sim_background_voxel_region
        COMMAND actraiser_sim_background_voxel_region_test)

    add_executable(actraiser_sim_world_map_test
        tests/sim_world_map_test.c
        src/sim/sim_world_map.c
        src/sim/sim_world_map_compose.c
    )
    target_include_directories(actraiser_sim_world_map_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_world_map COMMAND actraiser_sim_world_map_test)

    add_executable(actraiser_game_test
        tests/actraiser_game_test.c
    )
    target_include_directories(actraiser_game_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_game COMMAND actraiser_game_test)

    add_executable(actraiser_action_effects_test
        tests/action_effects_test.c
        src/action/action_effect_clock.c
        src/action/action_effects.c
        src/action/action_bg_world.c
    )
    target_include_directories(actraiser_action_effects_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_effects COMMAND actraiser_action_effects_test)

    add_executable(actraiser_action_camera_bounds_test
        tests/action_camera_bounds_test.c
        src/action/action_camera_bounds.c
    )
    target_include_directories(actraiser_action_camera_bounds_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
    )
    add_test(NAME actraiser_action_camera_bounds
        COMMAND actraiser_action_camera_bounds_test)

    # SPEC-bg-hle BH2: the action-stage WRAM world decoder is a pure source
    # provider. This target pins page addressing, metatile expansion, finite
    # bounds, atomic publication, and exact-byte cache invalidation without a
    # ROM, generated banks, or the PPU.
    add_executable(actraiser_action_bg_world_test
        tests/action_bg_world_test.c
        src/action/action_bg_world.c
    )
    target_include_directories(actraiser_action_bg_world_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
    )
    add_test(NAME actraiser_action_bg_world
        COMMAND actraiser_action_bg_world_test)

    # CPU contracts for the shared $02:B90D/$02:B95A action metatile strip
    # HLE, including their distinct geometry and stack/register/flag effects.
    add_executable(actraiser_action_metatile_hle_test
        tests/actraiser_action_metatile_hle_test.c
        src/actraiser/actraiser_cpu_hle_internal.c
        src/actraiser/actraiser_action_metatile.c
        src/cpu_65816_math.c
    )
    target_include_directories(actraiser_action_metatile_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_metatile_hle
        COMMAND actraiser_action_metatile_hle_test)

    # CPU and guarded-native-fallback contracts for action-only command-5/4
    # background staging. Synthetic compressed assets pin script traversal,
    # LoROM conversion, WRAM outputs, byte order, scratch, registers and RTS.
    add_executable(actraiser_action_room_loader_hle_test
        tests/actraiser_action_room_loader_hle_test.c
        src/quintet_lzss.c
        src/actraiser/actraiser_lzss.c
        src/actraiser/actraiser_cpu_hle_internal.c
        src/actraiser/actraiser_action_room_loader.c
    )
    target_include_directories(actraiser_action_room_loader_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/actraiser
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_room_loader_hle
        COMMAND actraiser_action_room_loader_hle_test)

    # CPU, PPU-port, workspace and guarded-native-fallback contracts for the
    # action-only command-7/6 character and palette staging handlers.
    add_executable(actraiser_action_room_graphics_hle_test
        tests/actraiser_action_room_graphics_hle_test.c
        src/quintet_lzss.c
        src/actraiser/actraiser_lzss.c
        src/actraiser/actraiser_cpu_hle_internal.c
        src/actraiser/actraiser_action_room_graphics.c
    )
    target_include_directories(actraiser_action_room_graphics_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/actraiser
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_room_graphics_hle
        COMMAND actraiser_action_room_graphics_hle_test)

    # CPU, PPU-port, direct-page and guarded-native-fallback contracts for
    # the action-only command-3 28-byte video-profile applicator.
    add_executable(actraiser_action_video_config_hle_test
        tests/actraiser_action_video_config_hle_test.c
        src/actraiser/actraiser_cpu_hle_internal.c
        src/actraiser/actraiser_action_video_config.c
    )
    target_include_directories(actraiser_action_video_config_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/actraiser
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_video_config_hle
        COMMAND actraiser_action_video_config_hle_test)

    # CPU contracts for the shared per-town/global lair-bit resolver family,
    # its six operations, stack writes, registers, and final flags.
    add_executable(actraiser_town_lair_bits_hle_test
        tests/actraiser_town_lair_bits_hle_test.c
        src/actraiser/actraiser_cpu_hle_internal.c
        src/actraiser/actraiser_town_lair_bits.c
        src/cpu_65816_math.c
    )
    target_include_directories(actraiser_town_lair_bits_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_town_lair_bits_hle
        COMMAND actraiser_town_lair_bits_hle_test)

    add_executable(actraiser_action_bg_plan_test
        tests/action_bg_plan_test.c
        src/action/action_bg_plan.c
    )
    target_include_directories(actraiser_action_bg_plan_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_bg_plan
        COMMAND actraiser_action_bg_plan_test)

    add_executable(actraiser_action_bg_tuner_test
        tests/action_bg_tuner_test.c
        src/action/action_bg_plan.c
        src/action/action_bg_tuner.c
    )
    target_include_directories(actraiser_action_bg_tuner_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_bg_tuner
        COMMAND actraiser_action_bg_tuner_test)

    add_executable(actraiser_action_bg_test
        tests/actraiser_action_bg_test.c
        src/quintet_lzss.c
        src/action/action_bg_plan.c
        src/action/action_bg_world.c
        src/action/action_room_scene.c
        src/actraiser/actraiser_action_bg.c
        src/diorama/diorama_layer_order.c
    )
    target_include_directories(actraiser_action_bg_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
        ${CMAKE_SOURCE_DIR}/src/actraiser
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_bg COMMAND actraiser_action_bg_test)

    if(Python3_Interpreter_FOUND)
        add_test(NAME actraiser_bg_hle_census
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/bg_hle_census_test.py)
        set_tests_properties(actraiser_bg_hle_census PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    endif()

    add_executable(actraiser_action_effect_render_test
        tests/action_effect_render_test.c
        src/action/action_effect_render.c
        src/action/action_effect_projection.c
        src/diorama/diorama_projection.c
        src/diorama/diorama_depth_shapes.c
        src/scene3d_math.c
    )
    target_include_directories(actraiser_action_effect_render_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
        ${CMAKE_SOURCE_DIR}/src/diorama
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_action_effect_render_test PRIVATE
        SDL3::SDL3
        actraiser_math
    )
    add_test(NAME actraiser_action_effect_render
        COMMAND actraiser_action_effect_render_test)

    add_executable(actraiser_diorama_projection_test
        tests/diorama_projection_test.c
        src/diorama/diorama_projection.c
        src/diorama/diorama_depth_shapes.c
        src/scene3d_math.c
    )
    target_include_directories(actraiser_diorama_projection_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_diorama_projection_test PRIVATE
        SDL3::SDL3
        actraiser_math
    )
    add_test(NAME actraiser_diorama_projection
        COMMAND actraiser_diorama_projection_test)

    add_executable(actraiser_sim_phase0_trace_test
        tests/sim_phase0_trace_test.c
        src/sim/sim_phase0_trace.c
    )
    target_include_directories(actraiser_sim_phase0_trace_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_phase0_trace
        COMMAND actraiser_sim_phase0_trace_test)

    add_executable(actraiser_sim_render_metadata_test
        tests/sim_render_metadata_test.c
        src/sim/sim_render_metadata.c
        src/sim/sim_world_map.c
        src/sim/sim_world_navigation_scene.c
    )
    target_include_directories(actraiser_sim_render_metadata_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_sim_render_metadata_test PRIVATE
        actraiser_math)
    # The picker-view test asserts whichever policy the app is built with.
    if(AR_SIM3D_PICKER_TOPDOWN)
        target_compile_definitions(actraiser_sim_render_metadata_test
            PRIVATE AR_SIM3D_PICKER_TOPDOWN=1)
    else()
        target_compile_definitions(actraiser_sim_render_metadata_test
            PRIVATE AR_SIM3D_PICKER_TOPDOWN=0)
    endif()
    add_test(NAME actraiser_sim_render_metadata
        COMMAND actraiser_sim_render_metadata_test)

    add_executable(actraiser_sim_visual_patches_test
        tests/sim_visual_patches_test.c
        src/sim/sim_visual_patches.c
    )
    target_include_directories(actraiser_sim_visual_patches_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
    )
    add_test(NAME actraiser_sim_visual_patches
        COMMAND actraiser_sim_visual_patches_test)

    add_executable(actraiser_settings_test
        tests/settings_test.c
        src/config.c
        src/settings.c
        src/atomic_replace.c
        # settings.c's randomizer rows call into the transform for their
        # availability gate and their on-change refresh. It is pure ROM-buffer
        # work with no SDL/PPU closure, so linking it here costs nothing.
        src/randomizer.c
        src/input_map.c
        src/user_data_dir.c
    )
    # settings.c owns the input-binding descriptors, so the registry test now
    # links the SDL-backed key/button naming they serialize through.
    target_link_libraries(actraiser_settings_test PRIVATE
        SDL3::SDL3 actraiser_math)
    target_include_directories(actraiser_settings_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_compile_definitions(actraiser_settings_test PRIVATE
        AR_SIM3D_TERRAIN_ELEVATION=${AR_SIM3D_TERRAIN_ELEVATION_VALUE})
    add_test(NAME actraiser_settings COMMAND actraiser_settings_test)

    add_executable(actraiser_settings_overlay_test
        tests/settings_overlay_test.c
        src/config.c
        src/quintet_lzss.c
        src/settings.c
        src/atomic_replace.c
        src/randomizer.c
        src/settings_overlay.c
        src/settings_overlay_debug_panel.c
        src/host/host_display_status.c
        src/presentation_geometry.c
        src/input_map.c
        src/user_data_dir.c
        # The layer editor's row model. Pure, so it adds no SDL/PPU closure to
        # this target -- which is the whole reason the overlay reaches the
        # override table through injected hooks rather than calling diorama.c.
        src/diorama/diorama_layer_editor.c
        src/diorama/diorama_layer_order.c
        # The live BG Extents tab is another pure custom-row model. Runtime
        # injection happens through its session singleton; no PPU/SDL closure.
        src/action/action_bg_plan.c
        src/action/action_bg_tuner.c
    )
    target_include_directories(actraiser_settings_overlay_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_settings_overlay_test PRIVATE
        SDL3::SDL3
        actraiser_math
    )
    target_compile_definitions(actraiser_settings_overlay_test PRIVATE
        AR_SIM3D_TERRAIN_ELEVATION=${AR_SIM3D_TERRAIN_ELEVATION_VALUE})
    add_test(NAME actraiser_settings_overlay COMMAND actraiser_settings_overlay_test)
    set_tests_properties(actraiser_settings_overlay PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    add_test(NAME actraiser_settings_overlay_minimum
        COMMAND actraiser_settings_overlay_test)
    set_tests_properties(actraiser_settings_overlay_minimum PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;AR_OVERLAY_TEST_SIZE=464x208")
    add_test(NAME actraiser_settings_overlay_pillarbox
        COMMAND actraiser_settings_overlay_test)
    set_tests_properties(actraiser_settings_overlay_pillarbox PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;AR_OVERLAY_TEST_SIZE=1280x720")

    add_executable(actraiser_scheduled_settings_test
        tests/scheduled_settings_test.c
        src/scheduled_settings.c
    )
    target_include_directories(actraiser_scheduled_settings_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS})
    add_test(NAME actraiser_scheduled_settings
        COMMAND actraiser_scheduled_settings_test)

    # SDL3 render-pipeline regression guard: presents a known framebuffer
    # through the same texture/FRect/logical-presentation path main.c uses and
    # asserts the read-back frame is not black (the prior SDL3 attempt's bug).
    add_executable(actraiser_render_pipeline_test
        tests/render_pipeline_test.c
        src/presentation_geometry.c
    )
    target_include_directories(actraiser_render_pipeline_test PRIVATE src)
    target_link_libraries(actraiser_render_pipeline_test PRIVATE
        SDL3::SDL3
        actraiser_math
    )
    add_test(NAME actraiser_render_pipeline
        COMMAND actraiser_render_pipeline_test)
    set_tests_properties(actraiser_render_pipeline PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

    # Scene -> CRT resolve -> host UI sequencing. The tiny present_frame.c
    # orchestrator is linked against test stubs so this remains deterministic
    # and GPU-free while guarding the order that keeps UI above pillar bars.
    add_executable(actraiser_present_frame_order_test
        tests/present_frame_order_test.c
        src/present_frame.c
        src/render_comparison.c
        src/session_fatal.c
    )
    target_include_directories(actraiser_present_frame_order_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_present_frame_order_test PRIVATE
        SDL3::SDL3
        actraiser_math
    )
    add_test(NAME actraiser_present_frame_order
        COMMAND actraiser_present_frame_order_test)

    add_executable(actraiser_render_comparison_test
        tests/render_comparison_test.c
        src/render_comparison.c
    )
    target_include_directories(actraiser_render_comparison_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME actraiser_render_comparison
        COMMAND actraiser_render_comparison_test)

    add_executable(actraiser_session_fatal_test
        tests/session_fatal_test.c
        src/session_fatal.c
    )
    target_include_directories(actraiser_session_fatal_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME actraiser_session_fatal
        COMMAND actraiser_session_fatal_test)

    add_executable(actraiser_presentation_outcome_test
        tests/presentation_outcome_test.c
    )
    target_include_directories(actraiser_presentation_outcome_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME actraiser_presentation_outcome
        COMMAND actraiser_presentation_outcome_test)

    add_executable(actraiser_hle_fatal_test
        tests/actraiser_hle_fatal_test.c)
    target_link_libraries(actraiser_hle_fatal_test PRIVATE
        actraiser_hle_fatal_test_support)
    add_test(NAME actraiser_hle_fatal
        COMMAND actraiser_hle_fatal_test)

    add_executable(actraiser_audio_presentation_policy_test
        tests/audio_presentation_policy_test.c
        src/audio_presentation_policy.c
    )
    target_include_directories(actraiser_audio_presentation_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS})
    add_test(NAME actraiser_audio_presentation_policy
        COMMAND actraiser_audio_presentation_policy_test)

    add_executable(actraiser_native_audio_trace_test
        tests/native_audio_trace_test.c
        src/dev/native_audio_trace.c
    )
    target_include_directories(actraiser_native_audio_trace_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME actraiser_native_audio_trace
        COMMAND actraiser_native_audio_trace_test)

    add_executable(actraiser_native_audio_mixer_test
        tests/native_audio_mixer_test.c
        src/native_audio_mixer.c
    )
    target_include_directories(actraiser_native_audio_mixer_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS})
    add_test(NAME actraiser_native_audio_mixer
        COMMAND actraiser_native_audio_mixer_test)

    add_executable(actraiser_native_audio_extension_test
        tests/native_audio_extension_test.c
        src/native_audio_extension.c
        ${SNESRECOMP_RUNNER_SAVELOAD_SOURCE}
    )
    target_include_directories(actraiser_native_audio_extension_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS})
    add_test(NAME actraiser_native_audio_extension
        COMMAND actraiser_native_audio_extension_test)

    add_executable(actraiser_dsp_bus_mix_test
        tests/dsp_bus_mix_test.c
        ${SNESRECOMP_RUNNER_DSP_SOURCE}
        ${SNESRECOMP_RUNNER_SAVELOAD_SOURCE}
    )
    target_include_directories(actraiser_dsp_bus_mix_test PRIVATE
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS})
    add_test(NAME actraiser_dsp_bus_mix
        COMMAND actraiser_dsp_bus_mix_test)

    add_executable(actraiser_dsp_voice_benchmark
        tools/benchmark_dsp_voices.c
        ${SNESRECOMP_RUNNER_DSP_SOURCE}
        ${SNESRECOMP_RUNNER_SAVELOAD_SOURCE}
    )
    target_include_directories(actraiser_dsp_voice_benchmark PRIVATE
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS})

    # Generated-shader distribution guard: asserts the COMMITTED shader blobs
    # (src/shaders/*_frag.h, produced by tools/build_shaders.py) are accepted by
    # the live GPU backend, and that the per-format entrypoint name is real.
    # Skips itself where no GPU device exists. Deliberately NOT run under the
    # dummy video driver: it needs a genuine Metal/Vulkan device.
    add_executable(actraiser_shader_blob_test
        tests/shader_blob_test.c
    )
    target_include_directories(actraiser_shader_blob_test PRIVATE src)
    target_link_libraries(actraiser_shader_blob_test PRIVATE
        SDL3::SDL3
        actraiser_math
    )
    add_test(NAME actraiser_shader_blob
        COMMAND actraiser_shader_blob_test)
    set_tests_properties(actraiser_shader_blob PROPERTIES
        ENVIRONMENT "SDL_AUDIODRIVER=dummy")

    # Verify that the SDL render sequence stays on the SDL_Init thread. The
    # dummy driver and software renderer keep the test headless and ROM-free.
    add_executable(actraiser_main_thread_boot_test
        tests/main_thread_boot_test.c
    )
    target_link_libraries(actraiser_main_thread_boot_test PRIVATE
        SDL3::SDL3
        actraiser_math
    )
    add_test(NAME actraiser_main_thread_boot
        COMMAND actraiser_main_thread_boot_test)
    set_tests_properties(actraiser_main_thread_boot PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

    # End-to-end guard using the REAL PPU: renders a backdrop through the actual
    # scanline renderer (which writes RGB with alpha=0), presents it through the
    # SDL3 path, and asserts the frame is visible — catching the SDL3 default
    # BLENDMODE_BLEND black-screen regression with genuine emulator output.
    add_executable(actraiser_ppu_render_pipeline_test
        tests/ppu_render_pipeline_test.c
        src/sim/sim3d.c
        src/sim/sim_background_voxel_preset.c
        src/sim/sim_render_metadata.c
        src/sim/sim_render_metadata_trace.c
        src/sim/sim_world_map.c
        src/sim/sim_world_navigation_scene.c
        src/sim/sim_world_navigation_capture.c
        src/sim/sim_town_canvas.c
        src/sim/sim_town_terrain.c
        src/sim/sim_town_layout.c
        src/sim/sim_background_voxels.c
        src/sim/sim_structure_visuals.c
        src/sim/sim_background_voxel_landmarks.c
        src/sim/sim_background_mountains.c
        src/sim/sim_background_mountain_silhouette.c
        src/sim/sim_background_mountain_relief.c
        src/sim/sim_background_voxel_region.c
        src/sim/sim_render_atlas.c
        ${SNESRECOMP_RUNNER_ROOT}/src/runner/runner.c
        ${SNESRECOMP_RUNNER_ROOT}/src/runner/runner_audio_trace.c
        ${SNESRECOMP_RUNNER_ROOT}/src/runner/runner_audio_control.c
        ${SNESRECOMP_RUNNER_ROOT}/src/runner/runner_ppu_services.c
        ${SNESRECOMP_RUNNER_ROOT}/tests/apu_sync_stub.c
        tests/audio_mix_control_stub.c
        ${SNESRECOMP_RUNNER_DEVICE_ROOT}/src/snes/ppu.c
        ${SNESRECOMP_RUNNER_SAVELOAD_SOURCE}
    )
    target_include_directories(actraiser_ppu_render_pipeline_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_ppu_render_pipeline_test PRIVATE
        SDL3::SDL3
        actraiser_math
    )
    add_test(NAME actraiser_ppu_render_pipeline
        COMMAND actraiser_ppu_render_pipeline_test)
    set_tests_properties(actraiser_ppu_render_pipeline PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

    # Pins the exact-vs-decoded OAM position equivalence over the whole in-cap
    # window (Phase 1's byte-identity claim), the divergence at the wrap
    # threshold, and range-vs-explicit-part rasterization identity.
    add_executable(actraiser_ppu_exact_pos_test
        tests/ppu_exact_pos_test.c
        ${SNESRECOMP_RUNNER_DEVICE_ROOT}/src/snes/ppu.c
        ${SNESRECOMP_RUNNER_SAVELOAD_SOURCE}
    )
    target_include_directories(actraiser_ppu_exact_pos_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_ppu_exact_pos_test PRIVATE actraiser_math)
    add_test(NAME actraiser_ppu_exact_pos
        COMMAND actraiser_ppu_exact_pos_test)

    add_executable(actraiser_hd_manifest_test
        tests/hd_manifest_test.c
        src/manifest_utils.c
        src/hd_replacements.c
    )
    target_include_directories(actraiser_hd_manifest_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_hd_manifest COMMAND actraiser_hd_manifest_test)

    add_executable(actraiser_scene_inspector_test
        tests/scene_inspector_test.c
        src/dev/scene_inspector.c
        src/sim/sim_render_metadata.c
        src/sim/sim_world_map.c
        src/sim/sim_world_navigation_scene.c
    )
    target_include_directories(actraiser_scene_inspector_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${CMAKE_SOURCE_DIR}/src/dev
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_scene_inspector COMMAND actraiser_scene_inspector_test)

    add_executable(actraiser_scene_asset_dump_test
        tests/scene_asset_dump_test.c
        ${SNESRECOMP_RUNNER_CRC32_SOURCE}
        src/dev/scene_asset_dump.c
    )
    target_include_directories(actraiser_scene_asset_dump_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/dev
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_scene_asset_dump COMMAND actraiser_scene_asset_dump_test)

    add_executable(actraiser_music_manifest_test
        tests/music_manifest_test.c
        src/manifest_utils.c
        src/music_replacements.c
        src/hd_replacements.c
        third_party/stb/stb_vorbis.c
    )
    target_include_directories(actraiser_music_manifest_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_music_manifest_test PRIVATE actraiser_math)
    add_test(NAME actraiser_music_manifest COMMAND actraiser_music_manifest_test)

    add_executable(actraiser_save_system_test
        tests/save_system_test.c
        src/save_system.c
        src/atomic_replace.c
    )
    target_include_directories(actraiser_save_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_compile_definitions(actraiser_save_system_test PRIVATE
        ACTRAISER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME actraiser_save_system COMMAND actraiser_save_system_test)

    # Shared instruction-exact arithmetic used by CPU-facing HLE wrappers.
    # Pin carry-in, carry-out, overflow, and packed-BCD adjustment directly.
    add_executable(actraiser_cpu_65816_math_test
        tests/cpu_65816_math_test.c
        src/cpu_65816_math.c
    )
    target_include_directories(actraiser_cpu_65816_math_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME actraiser_cpu_65816_math
        COMMAND actraiser_cpu_65816_math_test)

    # CPU-facing contract for the shared $00:84F3 save-checksum HLE. The
    # existing save-system suite pins the binary algorithm; this additionally
    # pins DP/register/P/RTL effects and the native decimal-mode edge.
    add_executable(actraiser_save_checksum_hle_test
        tests/actraiser_save_checksum_hle_test.c
        src/actraiser/actraiser_save_checksum.c
        src/cpu_65816_math.c
        src/save_system.c
        src/atomic_replace.c
    )
    target_include_directories(actraiser_save_checksum_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_save_checksum_hle
        COMMAND actraiser_save_checksum_hle_test)

    # Exhaustive semantic grids plus the complete CPU-facing contracts for the
    # shared $03:9710 cell-to-mark-map index and $03:96EF traversal-predicate
    # HLEs. Raw-width and decimal-mode cases pin instruction-level edges too.
    add_executable(actraiser_cell_map_hle_test
        tests/actraiser_cell_map_hle_test.c
        src/actraiser/actraiser_cpu_hle_internal.c
        src/actraiser/actraiser_cell_map.c
        src/cpu_65816_math.c
    )
    target_include_directories(actraiser_cell_map_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_cell_map_hle
        COMMAND actraiser_cell_map_hle_test)

    # Shared semantic town-metatile copy/draw-list execution plus the complete
    # CPU contracts for $03:9B5A, $03:9C43, and $03:A591.
    add_executable(actraiser_town_metatile_hle_test
        tests/actraiser_town_metatile_hle_test.c
        src/actraiser/actraiser_cpu_hle_internal.c
        src/actraiser/actraiser_town_metatile.c
        src/cpu_65816_math.c
    )
    target_include_directories(actraiser_town_metatile_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_town_metatile_hle
        COMMAND actraiser_town_metatile_hle_test)

    # Shared semantic town-structure step-program resolution/arming plus the
    # complete CPU contracts for rebuild $03:A4A8 and construction $03:A4B8.
    add_executable(actraiser_town_structure_steps_hle_test
        tests/actraiser_town_structure_steps_hle_test.c
        src/actraiser/actraiser_cpu_hle_internal.c
        src/actraiser/actraiser_town_structure_steps.c
        src/cpu_65816_math.c
    )
    target_include_directories(actraiser_town_structure_steps_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_town_structure_steps_hle
        COMMAND actraiser_town_structure_steps_hle_test)

    add_executable(actraiser_presentation_frame_generation_test
        tests/presentation_frame_generation_test.c
        src/presentation_frame_generation.c
    )
    target_include_directories(actraiser_presentation_frame_generation_test
        PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(actraiser_presentation_frame_generation_test PRIVATE
        actraiser_math)
    add_test(NAME actraiser_presentation_frame_generation
        COMMAND actraiser_presentation_frame_generation_test)

    add_executable(actraiser_diorama_depth_shapes_test
        tests/diorama_depth_shapes_test.c
        src/diorama/diorama_depth_shapes.c
    )
    target_include_directories(actraiser_diorama_depth_shapes_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src/diorama)
    target_link_libraries(actraiser_diorama_depth_shapes_test PRIVATE
        actraiser_math)
    add_test(NAME actraiser_diorama_depth_shapes
        COMMAND actraiser_diorama_depth_shapes_test)

    add_executable(actraiser_diorama_frame_generation_test
        tests/diorama_frame_generation_test.c
        src/diorama/diorama_frame_generation.c
        src/presentation_frame_generation.c
        src/presentation_geometry.c
    )
    target_include_directories(actraiser_diorama_frame_generation_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_diorama_frame_generation_test PRIVATE
        SDL3::SDL3 actraiser_math)
    add_test(NAME actraiser_diorama_frame_generation
        COMMAND actraiser_diorama_frame_generation_test)
    set_tests_properties(actraiser_diorama_frame_generation PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    add_test(NAME actraiser_diorama_frame_generation_gpu
        COMMAND actraiser_diorama_frame_generation_test)
    set_tests_properties(actraiser_diorama_frame_generation_gpu PROPERTIES
        ENVIRONMENT "SDL_AUDIODRIVER=dummy;AR_REQUIRE_GPU_RENDERER=1"
        SKIP_RETURN_CODE 77)

    # Production renderer/backend contract: compile the shipped vertex and
    # fragment shaders, build the real D32 pipelines, submit occluded geometry,
    # and read the cycled SDL_GPU target back through its SDL_Texture wrapper.
    # A host with no GPU renderer skips; Metal/Vulkan/D3D12 runners exercise
    # their native shader blob and synchronization path.
    add_executable(actraiser_sim3d_depth_pass_gpu_test
        tests/sim3d_depth_pass_gpu_test.c
        src/sim/sim3d_depth_pass.c
    )
    target_include_directories(actraiser_sim3d_depth_pass_gpu_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(actraiser_sim3d_depth_pass_gpu_test PRIVATE
        SDL3::SDL3 actraiser_math)
    add_test(NAME actraiser_sim3d_depth_pass_gpu
        COMMAND actraiser_sim3d_depth_pass_gpu_test)
    set_tests_properties(actraiser_sim3d_depth_pass_gpu PROPERTIES
        ENVIRONMENT "SDL_AUDIODRIVER=dummy"
        SKIP_RETURN_CODE 77)

    # Fix C (SPEC-backdrop-clip.md): geometry of the framebuffer margin-gap
    # fill. Pure, so it needs neither SDL nor the PPU.
    add_executable(actraiser_ws_gap_test
        tests/actraiser_ws_gap_test.c
        src/actraiser/actraiser_ws_gap.c
    )
    target_include_directories(actraiser_ws_gap_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/actraiser
    )
    add_test(NAME actraiser_ws_gap COMMAND actraiser_ws_gap_test)

    # Fix B (SPEC-backdrop-clip.md): BG2 valid-span classification and the
    # skybox quad's UV range, including the no-op guarantee that a padded
    # margin reproduces the pre-fix UV values exactly.
    add_executable(actraiser_diorama_skybox_uv_test
        tests/diorama_skybox_uv_test.c
        src/diorama/diorama_skybox_uv.c
        src/action/action_bg_plan.c
    )
    target_include_directories(actraiser_diorama_skybox_uv_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_diorama_skybox_uv_test PRIVATE
        actraiser_math)
    add_test(NAME actraiser_diorama_skybox_uv
        COMMAND actraiser_diorama_skybox_uv_test)

    # The action OBJ apron channel: where the two resolve bands sit in surface
    # columns, what is admitted into them, and the capacity's overflow
    # accounting. The negative property (an apron column is NEVER a display
    # column) is what the phase's byte-identity gate rests on.
    add_executable(actraiser_action_obj_apron_test
        tests/action_obj_apron_test.c
        src/action/action_obj_apron.c
    )
    target_include_directories(actraiser_action_obj_apron_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_obj_apron
        COMMAND actraiser_action_obj_apron_test)

    # F4 (handback 2026-07-26): which captured layers are half-added with the
    # subscreen and so must carry 50% alpha. Pure -- no SDL, no PPU struct.
    add_executable(actraiser_diorama_capture_blend_test
        tests/diorama_capture_blend_test.c
        src/diorama/diorama_capture_blend.c
    )
    target_include_directories(actraiser_diorama_capture_blend_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
    )
    add_test(NAME actraiser_diorama_capture_blend
        COMMAND actraiser_diorama_capture_blend_test)

    # F3/F4 (handback 2026-07-26): per-room ($18,$19) layer z/alpha overrides.
    # Pure -- no SDL, no PPU, no file I/O.
    add_executable(actraiser_diorama_layer_order_test
        tests/diorama_layer_order_test.c
        src/diorama/diorama_layer_order.c
    )
    target_include_directories(actraiser_diorama_layer_order_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_diorama_layer_order
        COMMAND actraiser_diorama_layer_order_test)

    # ROM backdrop HLE uses the stock asset script and compressed-asset format.
    # This exercises command inheritance plus literal/back-reference decoding
    # with synthetic data, so no copyrighted ROM is required by the suite. The
    # binary also accepts an optional local ROM path for a 98-source census.
    add_executable(actraiser_diorama_rom_backdrop_test
        tests/diorama_rom_backdrop_test.c
        src/quintet_lzss.c
        src/action/action_room_scene.c
        src/diorama/diorama_rom_backdrop.c
    )
    target_include_directories(actraiser_diorama_rom_backdrop_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_diorama_rom_backdrop
        COMMAND actraiser_diorama_rom_backdrop_test)

    # Shared byte-exact Quintet LZSS core used by both immutable host assets
    # and the formal $02:C5C9 CPU HLE. Pins bit phases, dictionary overlap,
    # truncation, and the native scratch metadata without requiring the ROM.
    add_executable(actraiser_quintet_lzss_test
        tests/quintet_lzss_test.c
        src/quintet_lzss.c
    )
    target_include_directories(actraiser_quintet_lzss_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME actraiser_quintet_lzss
        COMMAND actraiser_quintet_lzss_test)

    # CPU-facing contract for the $02:C5C9 whole-body HLE: output and ring
    # bytes plus all caller-visible DP scratch, registers, and RTL stack pop.
    add_executable(actraiser_lzss_hle_test
        tests/actraiser_lzss_hle_test.c
        src/quintet_lzss.c
        src/actraiser/actraiser_lzss.c
    )
    target_include_directories(actraiser_lzss_hle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_lzss_hle
        COMMAND actraiser_lzss_hle_test)

    # The in-game layer editor's row model and strategy authoring. Pure for the
    # same reason the depth shapes are: the overlay that consumes it is SDL, and
    # its test target deliberately does not link diorama.c.
    add_executable(actraiser_diorama_layer_editor_test
        tests/diorama_layer_editor_test.c
        src/diorama/diorama_layer_editor.c
        src/diorama/diorama_layer_order.c
    )
    target_include_directories(actraiser_diorama_layer_editor_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_diorama_layer_editor
        COMMAND actraiser_diorama_layer_editor_test)

    # Upgrade-safe INI merging. Pure string transform -- no SDL, no I/O -- and it
    # decides whether a user's settings survive extracting a new bundle over an
    # existing install, so it is gated like any other correctness module.
    add_executable(actraiser_ini_upgrade_test
        tests/ini_upgrade_test.c
        src/ini_upgrade.c
        src/ini_upgrade_apply.c
        src/user_data_dir.c
        src/atomic_replace.c
    )
    target_include_directories(actraiser_ini_upgrade_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME actraiser_ini_upgrade COMMAND actraiser_ini_upgrade_test)

    # In-game manual: page indexing and reader kinematics. Pure -- no SDL, no
    # stdio -- because the reader's SDL shim is untestable and everything that
    # decides behaviour should not be. Its load-bearing case asserts the leaf
    # never dips behind a settled page, which is what makes the reader's single
    # fixed draw order correct on a renderer with no depth test.
    add_executable(actraiser_manual_pages_test
        tests/manual_pages_test.c
        src/manual/manual_pages.c
        # ManualTurn_SheetExtents solves the sheet's world extents by probing the
        # projection, so the module depends on scene3d_math -- the one place the
        # settled page and the turning leaf are guaranteed to agree.
        src/scene3d_math.c
    )
    target_include_directories(actraiser_manual_pages_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/manual
    )
    target_link_libraries(actraiser_manual_pages_test PRIVATE actraiser_math)
    add_test(NAME actraiser_manual_pages COMMAND actraiser_manual_pages_test)

    # The reader's control scheme and its per-frame decode budget. Its own
    # translation unit precisely so this target exists: manual_reader.c owns
    # textures, a JPEG decoder and the window, and none of that has to be linked
    # to assert that PageDown turns a page.
    add_executable(actraiser_manual_input_test
        tests/manual_input_test.c
        src/manual/manual_input.c
    )
    target_include_directories(actraiser_manual_input_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/manual
    )
    target_link_libraries(actraiser_manual_input_test PRIVATE SDL3::SDL3)
    add_test(NAME actraiser_manual_input COMMAND actraiser_manual_input_test)

    # Cartridge-specific source pointers, sample pools, bootstrap signature,
    # and resident-uploader timing must stay in the game adapter rather than
    # leaking back into the portable runner.
    add_executable(actraiser_spc_upload_adapter_test
        tests/actraiser_spc_upload_test.c
        src/actraiser/actraiser_spc_upload.c
        snesrecomp-go/runtime/src/support/spc_upload.c)
    target_include_directories(actraiser_spc_upload_adapter_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS})
    add_test(NAME actraiser_spc_upload_adapter
        COMMAND actraiser_spc_upload_adapter_test)

    foreach(_ar_hle_target IN ITEMS
            actraiser_action_metatile_hle_test
            actraiser_action_room_loader_hle_test
            actraiser_action_room_graphics_hle_test
            actraiser_action_video_config_hle_test
            actraiser_town_lair_bits_hle_test
            actraiser_save_checksum_hle_test
            actraiser_cell_map_hle_test
            actraiser_town_metatile_hle_test
            actraiser_town_structure_steps_hle_test
            actraiser_lzss_hle_test)
        target_link_libraries(${_ar_hle_target} PRIVATE
            actraiser_hle_fatal_test_support)
    endforeach()
endif()

# ROM-free targets are authored consumers of the SDK unless they explicitly
# compile runner implementation sources. Give every local target the public
# headers through a target relationship instead of a directory-wide include
# path; focused hardware tests add the private roots themselves above.
if(BUILD_TESTING)
    get_property(_ar_directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_ar_target IN LISTS _ar_directory_targets)
        get_target_property(_ar_target_type ${_ar_target} TYPE)
        if(_ar_target_type STREQUAL "EXECUTABLE" OR
           _ar_target_type STREQUAL "STATIC_LIBRARY" OR
           _ar_target_type STREQUAL "SHARED_LIBRARY" OR
           _ar_target_type STREQUAL "MODULE_LIBRARY" OR
           _ar_target_type STREQUAL "OBJECT_LIBRARY")
            target_link_libraries(${_ar_target} PRIVATE snesrecomp::sdk)
        endif()
    endforeach()
endif()
