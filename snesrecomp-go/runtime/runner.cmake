# runner.cmake — shared source list for snesrecomp game projects.
#
# Usage from a game project's CMakeLists.txt:
#   set(SNESRECOMP_GO_ROOT ${CMAKE_SOURCE_DIR}/snesrecomp-go)
#   include(${SNESRECOMP_GO_ROOT}/runtime/runner.cmake)
#   add_executable(MyGame ${SNESRECOMP_RUNNER_SOURCES} <game sources> <generated sources>)
#   target_include_directories(MyGame PRIVATE ${SNESRECOMP_RUNNER_INCLUDE_DIRS} ...)
#
# Mirrors the file list in the MSVC .vcxproj so the same sources build on
# Windows (MSVC) and on macOS/Linux (clang/gcc + CMake). The snes9x emulator
# oracle (snes9x_bridge.cpp / ENABLE_ORACLE_BACKEND) is intentionally NOT part
# of this list — it is a developer-only verify backend, off for normal builds.

set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})

set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/common_cpu_infra.c
    ${SNESRECOMP_RUNNER_ROOT}/src/common_rtl.c
    ${SNESRECOMP_RUNNER_ROOT}/src/widescreen.c
    ${SNESRECOMP_RUNNER_ROOT}/src/recomp_hw.c
    ${SNESRECOMP_RUNNER_ROOT}/src/framedump.c
    ${SNESRECOMP_RUNNER_ROOT}/src/launcher.c
    ${SNESRECOMP_RUNNER_ROOT}/src/crc32.c
    ${SNESRECOMP_RUNNER_ROOT}/src/sha256.c
    ${SNESRECOMP_RUNNER_ROOT}/src/keybinds.c
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_state.c
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/ar_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/audio_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/util.c
    # SNES hardware model
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/apu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cart.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cpu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dma.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/audio_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/msu1.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/color_lut.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu_old.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes_other.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/spc.c
)

# Trace rings and tripwire instrumentation are developer-only. The historical
# TCP debug_server.c currently drifts from cpu_trace.c and is intentionally not
# linked; debug_server_stub.c satisfies its integration hooks in trace builds.
# Off by default; opt in with -DSNESRECOMP_ENABLE_TRACE=ON.
option(SNESRECOMP_ENABLE_TRACE "Build local observability rings and tripwires" OFF)
if(SNESRECOMP_ENABLE_TRACE)
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        ${SNESRECOMP_RUNNER_ROOT}/src/debug_server_stub.c
    )
endif()

set(SNESRECOMP_RUNNER_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
    ${SNESRECOMP_RUNNER_ROOT}/src/snes
)

# ── Build-speed freebies (do NOT affect generated C / runtime behavior) ──────
# Ported from perplexes/snesrecomp 71467b9 (2026-06-30). These only change
# HOW the C is compiled, never WHAT is compiled, so they are convergence-risk-
# free. Included from the game's top-level CMakeLists (before its
# add_executable), so the compiler launcher and -O override apply to all
# targets — including the large machine-generated src/gen/*.c that dominate
# C build time.

# ccache: transparently caches object files. A clean rebuild of unchanged
# generated banks becomes a cache-hit copy. Auto-enabled if ccache is on PATH;
# turn off with -DSNESRECOMP_USE_CCACHE=OFF.
option(SNESRECOMP_USE_CCACHE "Use ccache as the compiler launcher if available" ON)
if(SNESRECOMP_USE_CCACHE AND NOT CMAKE_C_COMPILER_LAUNCHER)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
        message(STATUS "snesrecomp: ccache enabled (${CCACHE_PROGRAM})")
    endif()
endif()

# Dev fast-build: the generated bank C is enormous straight-line code where the
# optimizer cost is superlinear (bank07 measured 17.9s@-O3 vs 4.1s@-O0). For the
# inner edit/regen/build loop -O3 is wasted; -O0 slashes compile time. Opt in
# with -DSNESRECOMP_DEV_FAST_BUILD=ON. A trailing -O0 wins over any build-type
# -O flag since the last -O on the command line takes effect. OFF by default so
# release/perf builds are unaffected.
option(SNESRECOMP_DEV_FAST_BUILD "Compile at -O0 for fast iteration (dev only)" OFF)
if(SNESRECOMP_DEV_FAST_BUILD AND NOT MSVC)
    add_compile_options(-O0)
    message(STATUS "snesrecomp: SNESRECOMP_DEV_FAST_BUILD on — compiling at -O0")
endif()
