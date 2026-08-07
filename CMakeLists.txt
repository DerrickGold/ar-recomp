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

# AR_TESTS_ONLY: configure ONLY the ROM-free test tier (the tests/ suite links
# individual src/*.c + SDL + PPU, never the generated ROM banks). When ON, the
# generated-bank glob check and the ActRaiserRecomp game target — both of which
# require a ROM to have been recompiled (src/gen/*.c + funcs.h) — are skipped,
# so `cmake -DAR_TESTS_ONLY=ON` configures cleanly on a checkout with no ROM.
# Off by default: a normal build is byte-identical to before this option existed.
option(AR_TESTS_ONLY "Configure only the ROM-free test tier (no ROM required)" OFF)

set(SNESRECOMP_GO_ROOT ${CMAKE_SOURCE_DIR}/snesrecomp-go)
include(${SNESRECOMP_GO_ROOT}/runtime/runner.cmake)

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

# THE GAME SOURCE LIST LIVES IN snesbuild.ini, AND THIS READS IT.
#
# There is exactly one list, and it is the one that SHIPS. snesbuild.ini is
# installed into a bundle's utils/ (packaging/CMakeLists.txt) and is what a user's
# `snesbuild build --hermetic` rebuild compiles from; this file is not shipped at
# all. Keeping a second literal copy here meant the two could disagree, and when
# they did the failure surfaced as a link error in a USER's bundle rebuild rather
# than anywhere near the commit that caused it:
#
#     error: undefined symbol: _AtomicReplaceFile
#
# src/atomic_replace.c had been added here and not to the manifest. The drift was
# detectable -- project.ManifestDriftWarnings names it exactly -- but its only
# hard-failing caller reads CMakeLists.txt from the hermetic build's own --root,
# which in a bundle is utils/ and carries no CMakeLists.txt: the read fails, the
# check returns nothing, and it is a silent no-op precisely where the rebuild runs.
#
# So the duplication is removed rather than policed. TO ADD A SOURCE FILE, add a
# `source = <path>` line to snesbuild.ini; both builds pick it up. Paths stay
# relative to this directory, exactly as the literal list was.
#
# CMAKE_CONFIGURE_DEPENDS re-runs configure when the manifest changes, so editing
# it does not require a manual cmake invocation to take effect.
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

add_executable(ActRaiserRecomp
    ${SNESRECOMP_RUNNER_SOURCES}
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
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
)

if(SNESRECOMP_ENABLE_TRACE)
    set(_AR_TRACE 1)
else()
    set(_AR_TRACE 0)
endif()

# AR_WATCHDOG: the 5s hang watchdog. Bring-up instrumentation — it exists to
# catch an infinite loop in recompiled code during porting, and once a build is
# verified there is nothing left for it to find. It is not free: the generated
# code calls WatchdogCheck at EVERY loop header (thousands of sites per bank),
# and escaping a stuck frame necessarily abandons it. Default ON for the
# development configurations and OFF for Release, so shipping builds carry
# neither the per-loop-header clock check nor the abandon path.
if(NOT DEFINED AR_WATCHDOG)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        set(_AR_WATCHDOG_DEFAULT OFF)
    else()
        set(_AR_WATCHDOG_DEFAULT ON)
    endif()
else()
    set(_AR_WATCHDOG_DEFAULT ${AR_WATCHDOG})
endif()
option(AR_WATCHDOG "Build the 5s hang watchdog (dev instrumentation)"
       ${_AR_WATCHDOG_DEFAULT})
if(AR_WATCHDOG)
    set(_AR_WATCHDOG 1)
    message(STATUS "ActRaiserRecomp: hang watchdog ENABLED (dev build)")
else()
    set(_AR_WATCHDOG 0)
    message(STATUS "ActRaiserRecomp: hang watchdog omitted (release build)")
endif()

target_compile_definitions(ActRaiserRecomp PRIVATE
    SYSTEM_VOLUME_MIXER_AVAILABLE=0
    SNESRECOMP_TRACE=${_AR_TRACE}
    SNESRECOMP_REVERSE_DEBUG=0
    AR_WATCHDOG=${_AR_WATCHDOG}
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

endif() # NOT AR_TESTS_ONLY

if(BUILD_TESTING)
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
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_host_audio_pause_test PRIVATE
        SDL3::SDL3
        m
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
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_host_display_pacing
        COMMAND actraiser_host_display_pacing_test)

    add_executable(actraiser_scene3d_math_test
        tests/scene3d_math_test.c
        src/scene3d_math.c
    )
    target_include_directories(actraiser_scene3d_math_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(actraiser_scene3d_math_test PRIVATE m)
    add_test(NAME actraiser_scene3d_math COMMAND actraiser_scene3d_math_test)

    add_executable(actraiser_camera_orbit_test
        tests/camera_orbit_test.c
        src/camera_orbit.c
    )
    target_include_directories(actraiser_camera_orbit_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(actraiser_camera_orbit_test PRIVATE m)
    add_test(NAME actraiser_camera_orbit COMMAND actraiser_camera_orbit_test)

    add_executable(actraiser_sim_town_canvas_test
        tests/sim_town_canvas_test.c
        src/sim/sim_town_canvas.c
    )
    target_include_directories(actraiser_sim_town_canvas_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_sim_town_canvas
        COMMAND actraiser_sim_town_canvas_test)

    add_executable(actraiser_sim_world_map_test
        tests/sim_world_map_test.c
        src/sim/sim_world_map.c
        src/sim/sim_world_map_compose.c
    )
    target_include_directories(actraiser_sim_world_map_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
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

    add_executable(actraiser_action_effects_test
        tests/action_effects_test.c
        src/action/action_effects.c
    )
    target_include_directories(actraiser_action_effects_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_action_effects COMMAND actraiser_action_effects_test)

    add_executable(actraiser_action_effect_render_test
        tests/action_effect_render_test.c
        src/action/action_effect_render.c
    )
    target_include_directories(actraiser_action_effect_render_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/action
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_action_effect_render_test PRIVATE
        SDL3::SDL3
        m
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
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_diorama_projection_test PRIVATE
        SDL3::SDL3
        m
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
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
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
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_sim_render_metadata_test PRIVATE m)
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
        src/atomic_replace.c
        src/randomizer.c
        src/settings_overlay.c
        src/input_map.c
        src/user_data_dir.c
        # The layer editor's row model. Pure, so it adds no SDL/PPU closure to
        # this target -- which is the whole reason the overlay reaches the
        # override table through injected hooks rather than calling diorama.c.
        src/diorama/diorama_layer_editor.c
        src/diorama/diorama_layer_order.c
    )
    target_include_directories(actraiser_settings_overlay_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
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
        m
    )
    add_test(NAME actraiser_shader_blob
        COMMAND actraiser_shader_blob_test)
    set_tests_properties(actraiser_shader_blob PROPERTIES
        ENVIRONMENT "SDL_AUDIODRIVER=dummy")

    # Phase 0 insurance: the whole SDL present pattern (window -> renderer ->
    # clear -> present -> readpixels) executed on the SDL_Init thread, proving
    # nothing in that sequence structurally needs a second thread once the
    # present thread is removed. Software renderer under the dummy driver.
    add_executable(actraiser_main_thread_boot_test
        tests/main_thread_boot_test.c
    )
    target_link_libraries(actraiser_main_thread_boot_test PRIVATE
        SDL3::SDL3
        m
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
        src/sim/sim_render_metadata.c
        src/sim/sim_render_metadata_trace.c
        src/sim/sim_world_map.c
        src/sim/sim_world_navigation_scene.c
        src/sim/sim_world_navigation_capture.c
        src/sim/sim_town_canvas.c
        src/sim/sim_render_atlas.c
        ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu.c
        ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu_old.c
    )
    target_include_directories(actraiser_ppu_render_pipeline_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
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

    # Pins the exact-vs-decoded OAM position equivalence over the whole in-cap
    # window (Phase 1's byte-identity claim), the divergence at the wrap
    # threshold, and range-vs-explicit-part rasterization identity.
    add_executable(actraiser_ppu_exact_pos_test
        tests/ppu_exact_pos_test.c
        ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu.c
        ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu_old.c
    )
    target_include_directories(actraiser_ppu_exact_pos_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_ppu_exact_pos_test PRIVATE m)
    add_test(NAME actraiser_ppu_exact_pos
        COMMAND actraiser_ppu_exact_pos_test)

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
        src/dev/scene_inspector.c
        src/sim/sim_render_metadata.c
        src/sim/sim_world_map.c
        src/sim/sim_world_navigation_scene.c
    )
    target_include_directories(actraiser_scene_inspector_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/sim
        ${CMAKE_SOURCE_DIR}/src/dev
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_scene_inspector COMMAND actraiser_scene_inspector_test)

    add_executable(actraiser_scene_asset_dump_test
        tests/scene_asset_dump_test.c
        src/dev/scene_asset_dump.c
    )
    target_include_directories(actraiser_scene_asset_dump_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/dev
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
        src/atomic_replace.c
    )
    target_include_directories(actraiser_save_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_compile_definitions(actraiser_save_system_test PRIVATE
        ACTRAISER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME actraiser_save_system COMMAND actraiser_save_system_test)

    # Golden test for the diorama M7 scroll-interpolation math, extracted from
    # present.c with the wall-clock injected so t is controllable (Test gap 2).
    add_executable(actraiser_diorama_scroll_math_test
        tests/diorama_scroll_math_test.c
        src/diorama/diorama_scroll_math.c
        src/diorama/diorama_depth_shapes.c
    )
    target_include_directories(actraiser_diorama_scroll_math_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_diorama_scroll_math_test PRIVATE SDL3::SDL3 m)
    add_test(NAME actraiser_diorama_scroll_math
        COMMAND actraiser_diorama_scroll_math_test)

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
    )
    target_include_directories(actraiser_diorama_skybox_uv_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/diorama
    )
    target_link_libraries(actraiser_diorama_skybox_uv_test PRIVATE m)
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
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
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
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    )
    add_test(NAME actraiser_diorama_layer_order
        COMMAND actraiser_diorama_layer_order_test)

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
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
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
    target_link_libraries(actraiser_manual_pages_test PRIVATE m)
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
endif()
