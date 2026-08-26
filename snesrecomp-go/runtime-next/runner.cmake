# Manifest for the independently authored portable runner.

set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(SNESRECOMP_RUNNER_VARIANT next)
set(SNESRECOMP_RUNNER_LEGACY_FALLBACK OFF)
set(SNESRECOMP_RUNNER_DEVICE_ROOT ${SNESRECOMP_RUNNER_ROOT})
set(SNESRECOMP_RUNNER_CRC32_SOURCE ${SNESRECOMP_RUNNER_ROOT}/src/crc32.c)
set(SNESRECOMP_RUNNER_DSP_SOURCE ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp.c)

set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/runner_next.c
    ${SNESRECOMP_RUNNER_ROOT}/src/crc32.c
    ${SNESRECOMP_RUNNER_ROOT}/src/sha256.c
    ${SNESRECOMP_RUNNER_ROOT}/src/widescreen.c
    ${SNESRECOMP_RUNNER_ROOT}/src/recomp_hw.c
    ${SNESRECOMP_RUNNER_ROOT}/src/framedump.c
    ${SNESRECOMP_RUNNER_ROOT}/src/audio_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/ar_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/util.c
    ${SNESRECOMP_RUNNER_ROOT}/src/launcher.c
    ${SNESRECOMP_RUNNER_ROOT}/src/keybinds.c
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_state.c
    ${SNESRECOMP_RUNNER_ROOT}/src/common_cpu_infra.c
    ${SNESRECOMP_RUNNER_ROOT}/src/diagnostic.c
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/common_rtl.c
    ${SNESRECOMP_RUNNER_ROOT}/src/spc_upload.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cart_map.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cart.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/rom.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes_other.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dma.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/apu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/spc.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/audio_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/msu1.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/color_lut.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cpu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu_old.c
)

option(SNESRECOMP_ENABLE_TRACE "Build local observability rings and tripwires" OFF)
if(SNESRECOMP_ENABLE_TRACE)
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        ${SNESRECOMP_RUNNER_ROOT}/src/debug_server_stub.c)
endif()

set(SNESRECOMP_RUNNER_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
)
