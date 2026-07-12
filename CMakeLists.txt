cmake_minimum_required(VERSION 3.16)
project(ActRaiserRecomp C)

set(CMAKE_C_STANDARD 11)

set(SNESRECOMP_ROOT ${CMAKE_SOURCE_DIR}/snesrecomp)
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

find_package(SDL2 REQUIRED)

file(GLOB ACTRAISER_GEN_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/gen/*.c)
if(NOT ACTRAISER_GEN_SOURCES)
    message(FATAL_ERROR
        "src/gen/ is empty — run `bash tools/regen.sh` with ar.sfc "
        "at the repo root before building.")
endif()

add_executable(ActRaiserRecomp
    ${SNESRECOMP_RUNNER_SOURCES}
    # game-specific runtime
    src/config.c
    src/run_dir.c
    src/main.c
    src/actraiser_00.c
    src/gen_stubs.c
    src/actraiser_cpu_infra.c
    src/actraiser_rtl.c
    src/actraiser_widescreen_bg.c
    src/actraiser_spc_player.c
    # recompiled banks (generated)
    ${ACTRAISER_GEN_SOURCES}
)

target_include_directories(ActRaiserRecomp PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/recomp
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
