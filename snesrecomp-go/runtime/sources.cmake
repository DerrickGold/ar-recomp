# Canonical source list for the independently authored portable runner.
# Keep target-specific adapters (for example trace stubs) in the consuming
# build description rather than duplicating the core list.

set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/runner/runner.c
    ${SNESRECOMP_RUNNER_ROOT}/src/runner/runner_game_module.c
    ${SNESRECOMP_RUNNER_ROOT}/src/runner/runner_audio_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/runner/runner_ppu_services.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/crc32.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/sha256.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/widescreen.c
    ${SNESRECOMP_RUNNER_ROOT}/src/core/recomp_hw.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/framedump.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/audio_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/core/runtime_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/util.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/packed_data.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/bps.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/launcher.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/keybinds.c
    ${SNESRECOMP_RUNNER_ROOT}/src/core/cpu_state.c
    ${SNESRECOMP_RUNNER_ROOT}/src/core/common_cpu_infra.c
    ${SNESRECOMP_RUNNER_ROOT}/src/core/diagnostic.c
    ${SNESRECOMP_RUNNER_ROOT}/src/core/cpu_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/core/common_rtl.c
    ${SNESRECOMP_RUNNER_ROOT}/src/support/spc_upload.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cart_map.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cart.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/rom.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/saveload.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes_other.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dma.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/apu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/spc.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp_accuracy_bridge.cpp
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/accuracy/dsp.cpp
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/audio_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/msu1.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/color_lut.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cpu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu.c
    # Cold, synchronous adapter control stays last so adding it does not
    # perturb the established hot runner object order.
    ${SNESRECOMP_RUNNER_ROOT}/src/runner/runner_audio_control.c
)
