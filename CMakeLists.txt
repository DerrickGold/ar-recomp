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
include(CTest)

set(SNESRECOMP_GO_ROOT ${CMAKE_SOURCE_DIR}/snesrecomp-go)
include(${SNESRECOMP_GO_ROOT}/runtime/runner.cmake)

find_package(SDL3 REQUIRED)

set(ACTRAISER_GEN_DIR "${CMAKE_SOURCE_DIR}/src/gen" CACHE PATH
    "Directory containing generated recomp C translation units")
file(GLOB ACTRAISER_GEN_SOURCES CONFIGURE_DEPENDS "${ACTRAISER_GEN_DIR}/*.c")
if(NOT ACTRAISER_GEN_SOURCES)
    message(FATAL_ERROR
        "${ACTRAISER_GEN_DIR} is empty — run `snesbuild regen --root . "
        "--rom ar.sfc` (or the compatibility tools/regen.sh wrapper) before building.")
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

add_executable(ActRaiserRecomp
    ${SNESRECOMP_RUNNER_SOURCES}
    # game-specific runtime
    src/config.c
    src/settings.c
    src/settings_overlay.c
    src/input_map.c
    src/scene_inspector.c
    src/scene_asset_dump.c
    src/sim_phase0_trace.c
    src/sim_render_metadata.c
    src/sim_render_atlas.c
    src/sim_world_map.c
    src/sim_town_canvas.c
    src/sim3d.c
    src/save_system.c
    src/run_dir.c
    src/scene3d_math.c
    src/diorama.c
    src/present.c
    src/main.c
    src/actraiser_00.c
    src/gen_stubs.c
    src/actraiser_cpu_infra.c
    src/actraiser_rtl.c
    src/hd_replacements.c
    src/music_replacements.c
    src/sfx_census.c
    third_party/stb/stb_vorbis.c
    src/hd_tile_census.c
    src/actraiser_widescreen_bg.c
    src/actraiser_widescreen_sprites.c
    src/actraiser_bugfixes.c
    src/actraiser_spc_player.c
    # recompiled banks (generated)
    ${ACTRAISER_GEN_SOURCES}
)

target_include_directories(ActRaiserRecomp PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/recomp
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
)

if(SNESRECOMP_ENABLE_TRACE)
    set(_AR_TRACE 1)
else()
    set(_AR_TRACE 0)
endif()
target_compile_definitions(ActRaiserRecomp PRIVATE
    SYSTEM_VOLUME_MIXER_AVAILABLE=0
    SNESRECOMP_TRACE=${_AR_TRACE}
    SNESRECOMP_REVERSE_DEBUG=0
)

if(NOT MSVC)
    target_compile_options(ActRaiserRecomp PRIVATE
        -w -Wno-implicit-function-declaration)
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

# AR_SANITIZE: AddressSanitizer + UndefinedBehaviorSanitizer opt-in build.
# 2026-06-30: added chasing a CpuState->x_flag corruption where runtime
# instrumentation proved the flag reads differently one C statement apart
# with ZERO emitted code in between (a switch-case selecting a variant by
# x_flag==1, then that variant's own first statement reading x_flag==0) --
# a signature of a stray/out-of-bounds write elsewhere landing on memory
# that happens to alias the CpuState struct, not a decode-time bug. ASan
# catches the actual corrupting write with a full stack trace; printf-based
# instrumentation can only observe the symptom. Off by default (real perf
# cost — ASan is typically 2-3x slower, tractable for one repro run but not
# for normal play). Enable with -DAR_SANITIZE=ON.
option(AR_SANITIZE "Build with ASan + UBSan (debug corruption bugs)" OFF)
if(AR_SANITIZE AND NOT MSVC)
    target_compile_options(ActRaiserRecomp PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(ActRaiserRecomp PRIVATE
        -fsanitize=address,undefined)
    message(STATUS "ActRaiserRecomp: AR_SANITIZE on — ASan+UBSan enabled")
endif()

# AR_TSAN: ThreadSanitizer opt-in build (D16, ar-recomp-threading-impl.md).
# Mutually exclusive with AR_SANITIZE (ASan+TSan can't be combined in one
# binary). Use this to bring up the M5 present thread: run a session
# exercising widescreen/diorama/HD-replacements, F5/F7 save-load, F9 display
# cycle, F2 snapshot, pause/settings-overlay, and window resize, and watch
# for TSan reports. Off by default (real perf cost, like AR_SANITIZE).
option(AR_TSAN "Build with ThreadSanitizer (debug the present-thread handshake)" OFF)
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
    SDL3::SDL3
    m
)

if(BUILD_TESTING)
    add_executable(actraiser_scene3d_math_test
        tests/scene3d_math_test.c
        src/scene3d_math.c
    )
    target_include_directories(actraiser_scene3d_math_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(actraiser_scene3d_math_test PRIVATE m)
    add_test(NAME actraiser_scene3d_math COMMAND actraiser_scene3d_math_test)

    add_executable(actraiser_sim_town_canvas_test
        tests/sim_town_canvas_test.c
        src/sim_town_canvas.c
    )
    target_include_directories(actraiser_sim_town_canvas_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_town_canvas
        COMMAND actraiser_sim_town_canvas_test)

    add_executable(actraiser_sim_world_map_test
        tests/sim_world_map_test.c
        src/sim_world_map.c
    )
    target_include_directories(actraiser_sim_world_map_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_world_map COMMAND actraiser_sim_world_map_test)

    add_executable(actraiser_game_test
        tests/actraiser_game_test.c
    )
    target_include_directories(actraiser_game_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_game COMMAND actraiser_game_test)

    add_executable(actraiser_sim_phase0_trace_test
        tests/sim_phase0_trace_test.c
        src/sim_phase0_trace.c
    )
    target_include_directories(actraiser_sim_phase0_trace_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_phase0_trace
        COMMAND actraiser_sim_phase0_trace_test)

    add_executable(actraiser_sim_render_metadata_test
        tests/sim_render_metadata_test.c
        src/sim_render_metadata.c
        src/sim_world_map.c
    )
    target_include_directories(actraiser_sim_render_metadata_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
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

    add_executable(actraiser_settings_test
        tests/settings_test.c
        src/config.c
        src/settings.c
        src/input_map.c
    )
    # settings.c owns the input-binding descriptors, so the registry test now
    # links the SDL-backed key/button naming they serialize through.
    target_link_libraries(actraiser_settings_test PRIVATE SDL3::SDL3 m)
    target_include_directories(actraiser_settings_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_settings COMMAND actraiser_settings_test)

    add_executable(actraiser_settings_overlay_test
        tests/settings_overlay_test.c
        src/config.c
        src/settings.c
        src/settings_overlay.c
        src/input_map.c
    )
    target_include_directories(actraiser_settings_overlay_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_settings_overlay_test PRIVATE
        SDL3::SDL3
        m
    )
    add_test(NAME actraiser_settings_overlay COMMAND actraiser_settings_overlay_test)
    set_tests_properties(actraiser_settings_overlay PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    add_test(NAME actraiser_settings_overlay_minimum
        COMMAND actraiser_settings_overlay_test)
    set_tests_properties(actraiser_settings_overlay_minimum PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;AR_OVERLAY_TEST_SIZE=464x208")

    # SDL3 render-pipeline regression guard: presents a known framebuffer
    # through the same texture/FRect/logical-presentation path main.c uses and
    # asserts the read-back frame is not black (the prior SDL3 attempt's bug).
    add_executable(actraiser_render_pipeline_test
        tests/render_pipeline_test.c
    )
    target_link_libraries(actraiser_render_pipeline_test PRIVATE
        SDL3::SDL3
        m
    )
    add_test(NAME actraiser_render_pipeline
        COMMAND actraiser_render_pipeline_test)
    set_tests_properties(actraiser_render_pipeline PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

    # End-to-end guard using the REAL PPU: renders a backdrop through the actual
    # scanline renderer (which writes RGB with alpha=0), presents it through the
    # SDL3 path, and asserts the frame is visible — catching the SDL3 default
    # BLENDMODE_BLEND black-screen regression with genuine emulator output.
    add_executable(actraiser_ppu_render_pipeline_test
        tests/ppu_render_pipeline_test.c
        src/sim3d.c
        src/sim_render_metadata.c
        src/sim_world_map.c
        src/sim_town_canvas.c
        src/sim_render_atlas.c
        ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu.c
        ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu_old.c
    )
    target_include_directories(actraiser_ppu_render_pipeline_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_ppu_render_pipeline_test PRIVATE
        SDL3::SDL3
        m
    )
    add_test(NAME actraiser_ppu_render_pipeline
        COMMAND actraiser_ppu_render_pipeline_test)
    set_tests_properties(actraiser_ppu_render_pipeline PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

    add_executable(actraiser_hd_manifest_test
        tests/hd_manifest_test.c
        src/hd_replacements.c
    )
    target_include_directories(actraiser_hd_manifest_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_hd_manifest COMMAND actraiser_hd_manifest_test)

    add_executable(actraiser_scene_inspector_test
        tests/scene_inspector_test.c
        src/scene_inspector.c
        src/sim_render_metadata.c
        src/sim_world_map.c
    )
    target_include_directories(actraiser_scene_inspector_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_scene_inspector COMMAND actraiser_scene_inspector_test)

    add_executable(actraiser_scene_asset_dump_test
        tests/scene_asset_dump_test.c
        src/scene_asset_dump.c
    )
    target_include_directories(actraiser_scene_asset_dump_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_scene_asset_dump COMMAND actraiser_scene_asset_dump_test)

    add_executable(actraiser_music_manifest_test
        tests/music_manifest_test.c
        src/music_replacements.c
        src/hd_replacements.c
        third_party/stb/stb_vorbis.c
    )
    target_include_directories(actraiser_music_manifest_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_music_manifest_test PRIVATE m)
    add_test(NAME actraiser_music_manifest COMMAND actraiser_music_manifest_test)

    add_executable(actraiser_save_system_test
        tests/save_system_test.c
        src/save_system.c
    )
    target_include_directories(actraiser_save_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_compile_definitions(actraiser_save_system_test PRIVATE
        ACTRAISER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME actraiser_save_system COMMAND actraiser_save_system_test)
endif()
