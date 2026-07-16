cmake_minimum_required(VERSION 3.16)
project(ActRaiserRecomp C)

set(CMAKE_C_STANDARD 11)
include(CTest)

set(SNESRECOMP_ROOT ${CMAKE_SOURCE_DIR}/snesrecomp)
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

find_package(SDL2 REQUIRED)

file(GLOB ACTRAISER_GEN_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/gen/*.c)
if(NOT ACTRAISER_GEN_SOURCES)
    message(FATAL_ERROR
        "src/gen/ is empty — run `bash tools/regen.sh` with ar.sfc "
        "at the repo root before building.")
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
    src/run_dir.c
    src/main.c
    src/actraiser_00.c
    src/gen_stubs.c
    src/actraiser_cpu_infra.c
    src/actraiser_rtl.c
    src/hd_replacements.c
    src/music_replacements.c
    third_party/stb/stb_vorbis.c
    src/hd_tile_census.c
    src/actraiser_widescreen_bg.c
    src/actraiser_widescreen_sprites.c
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
    ${SDL2_INCLUDE_DIRS}
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

target_link_libraries(ActRaiserRecomp PRIVATE
    ${SDL2_LIBRARIES}
    m
)

if(BUILD_TESTING)
    add_executable(actraiser_settings_test
        tests/settings_test.c
        src/config.c
        src/settings.c
    )
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
    )
    target_include_directories(actraiser_settings_overlay_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_libraries(actraiser_settings_overlay_test PRIVATE
        ${SDL2_LIBRARIES}
        m
    )
    add_test(NAME actraiser_settings_overlay COMMAND actraiser_settings_overlay_test)
    set_tests_properties(actraiser_settings_overlay PROPERTIES
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
endif()
