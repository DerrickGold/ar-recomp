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
    src/main.c
    src/actraiser_00.c
    src/gen_stubs.c
    src/actraiser_cpu_infra.c
    src/actraiser_rtl.c
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

target_link_libraries(ActRaiserRecomp PRIVATE
    ${SDL2_LIBRARIES}
    m
)
