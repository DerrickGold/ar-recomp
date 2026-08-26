cmake_minimum_required(VERSION 3.16)
project(snesrecomp_runtime_next_contract C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-fno-common)
endif()

add_library(snesrecomp_runtime_next_contract STATIC
    src/runner_next.c
    src/crc32.c
    src/sha256.c
    src/widescreen.c
    src/recomp_hw.c
    src/framedump.c
    src/audio_trace.c
    src/ar_trace.c
    src/util.c
    src/launcher.c
    src/keybinds.c
    src/debug_server_stub.c
    src/cpu_state.c
    src/common_cpu_infra.c
    src/diagnostic.c
    src/cpu_trace.c
    src/common_rtl.c
    src/spc_upload.c
    src/snes/cart_map.c
    src/snes/cart.c
    src/snes/rom.c
    src/snes/dma.c
    src/snes/snes.c
    src/snes/apu.c
    src/snes/spc.c
    src/snes/dsp.c
    src/snes/audio_shadow.c
    src/snes/dsp_shadow.c
    src/snes/msu1.c
    src/snes/color_lut.c
    src/snes/cpu.c
    src/snes/ppu.c
    src/snes/ppu_old.c)
target_include_directories(snesrecomp_runtime_next_contract PUBLIC src)
if(MSVC)
    target_compile_options(snesrecomp_runtime_next_contract PRIVATE /W4)
else()
    target_compile_options(snesrecomp_runtime_next_contract PRIVATE
        -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(snesrecomp_runtime_next_contract_test tests/contract_test.c)
    target_link_libraries(snesrecomp_runtime_next_contract_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_contract
        COMMAND snesrecomp_runtime_next_contract_test)

    add_executable(snesrecomp_runtime_next_cart_test tests/cart_test.c)
    target_link_libraries(snesrecomp_runtime_next_cart_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_cart
        COMMAND snesrecomp_runtime_next_cart_test)

    add_executable(snesrecomp_runtime_next_rom_test tests/rom_test.c)
    target_link_libraries(snesrecomp_runtime_next_rom_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_rom
        COMMAND snesrecomp_runtime_next_rom_test)

    add_executable(snesrecomp_runtime_next_dma_test tests/dma_test.c)
    target_link_libraries(snesrecomp_runtime_next_dma_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_dma
        COMMAND snesrecomp_runtime_next_dma_test)

    add_executable(snesrecomp_runtime_next_snes_test tests/snes_test.c)
    target_link_libraries(snesrecomp_runtime_next_snes_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_snes
        COMMAND snesrecomp_runtime_next_snes_test)

    add_executable(snesrecomp_runtime_next_apu_test tests/apu_test.c)
    target_link_libraries(snesrecomp_runtime_next_apu_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_apu
        COMMAND snesrecomp_runtime_next_apu_test)

    add_executable(snesrecomp_runtime_next_spc_test tests/spc_test.c)
    target_link_libraries(snesrecomp_runtime_next_spc_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_spc
        COMMAND snesrecomp_runtime_next_spc_test)

    add_executable(snesrecomp_runtime_next_dsp_test tests/dsp_test.c)
    target_link_libraries(snesrecomp_runtime_next_dsp_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_dsp
        COMMAND snesrecomp_runtime_next_dsp_test)

    add_executable(snesrecomp_runtime_next_audio_shadow_test
        tests/audio_shadow_test.c)
    target_link_libraries(snesrecomp_runtime_next_audio_shadow_test PRIVATE
        snesrecomp_runtime_next_contract)
    if(NOT MSVC)
        target_link_libraries(snesrecomp_runtime_next_audio_shadow_test PRIVATE m)
    endif()
    add_test(NAME snesrecomp_runtime_next_audio_shadow
        COMMAND snesrecomp_runtime_next_audio_shadow_test)

    add_executable(snesrecomp_runtime_next_msu1_test tests/msu1_test.c)
    target_compile_definitions(snesrecomp_runtime_next_msu1_test PRIVATE
        MSU1_TEST_BASE="${CMAKE_CURRENT_BINARY_DIR}/runtime-next-msu1-fixture")
    target_link_libraries(snesrecomp_runtime_next_msu1_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_msu1
        COMMAND snesrecomp_runtime_next_msu1_test)

    add_executable(snesrecomp_runtime_next_color_lut_test
        tests/color_lut_test.c)
    target_link_libraries(snesrecomp_runtime_next_color_lut_test PRIVATE
        snesrecomp_runtime_next_contract)
    if(NOT MSVC)
        target_link_libraries(snesrecomp_runtime_next_color_lut_test PRIVATE m)
    endif()
    add_test(NAME snesrecomp_runtime_next_color_lut
        COMMAND snesrecomp_runtime_next_color_lut_test)

    add_executable(snesrecomp_runtime_next_audio_trace_test
        tests/audio_trace_test.c)
    target_compile_definitions(snesrecomp_runtime_next_audio_trace_test PRIVATE
        AUDIO_TRACE_TEST_WAV="${CMAKE_CURRENT_BINARY_DIR}/runtime-next-audio-trace.wav")
    target_link_libraries(snesrecomp_runtime_next_audio_trace_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_audio_trace
        COMMAND snesrecomp_runtime_next_audio_trace_test)

    add_executable(snesrecomp_runtime_next_ar_trace_test tests/ar_trace_test.c)
    target_compile_definitions(snesrecomp_runtime_next_ar_trace_test PRIVATE
        AR_TRACE_TEST_PATH="${CMAKE_CURRENT_BINARY_DIR}/runtime-next-ar-trace.jsonl")
    target_link_libraries(snesrecomp_runtime_next_ar_trace_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_ar_trace
        COMMAND snesrecomp_runtime_next_ar_trace_test)

    add_executable(snesrecomp_runtime_next_util_test tests/util_test.c)
    target_link_libraries(snesrecomp_runtime_next_util_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_util
        COMMAND snesrecomp_runtime_next_util_test)

    add_executable(snesrecomp_runtime_next_launcher_test tests/launcher_test.c)
    target_compile_definitions(snesrecomp_runtime_next_launcher_test PRIVATE
        LAUNCHER_TEST_ROM="${CMAKE_CURRENT_BINARY_DIR}/runtime-next-launcher.sfc")
    target_link_libraries(snesrecomp_runtime_next_launcher_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_launcher
        COMMAND snesrecomp_runtime_next_launcher_test)

    add_executable(snesrecomp_runtime_next_keybinds_test tests/keybinds_test.c)
    target_compile_definitions(snesrecomp_runtime_next_keybinds_test PRIVATE
        KEYBINDS_TEST_EXE="${CMAKE_CURRENT_BINARY_DIR}/runtime-next-test-runner"
        KEYBINDS_TEST_INI="${CMAKE_CURRENT_BINARY_DIR}/keybinds.ini")
    target_link_libraries(snesrecomp_runtime_next_keybinds_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_keybinds
        COMMAND snesrecomp_runtime_next_keybinds_test)

    add_executable(snesrecomp_runtime_next_cpu_state_test tests/cpu_state_test.c)
    target_link_libraries(snesrecomp_runtime_next_cpu_state_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_cpu_state
        COMMAND snesrecomp_runtime_next_cpu_state_test)

    add_executable(snesrecomp_runtime_next_cpu_infra_test
        tests/cpu_infra_test.c src/common_cpu_infra.c)
    target_include_directories(snesrecomp_runtime_next_cpu_infra_test PRIVATE src)
    target_compile_definitions(snesrecomp_runtime_next_cpu_infra_test PRIVATE
        AR_WATCHDOG=0 SNESRECOMP_TRACE=0 SNESRECOMP_REVERSE_DEBUG=0)
    if(MSVC)
        target_compile_options(snesrecomp_runtime_next_cpu_infra_test PRIVATE /W4)
    else()
        target_compile_options(snesrecomp_runtime_next_cpu_infra_test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snesrecomp_runtime_next_cpu_infra
        COMMAND snesrecomp_runtime_next_cpu_infra_test)

    add_executable(snesrecomp_runtime_next_diagnostic_test
        tests/diagnostic_test.c src/diagnostic.c)
    target_include_directories(snesrecomp_runtime_next_diagnostic_test PRIVATE src)
    target_compile_definitions(snesrecomp_runtime_next_diagnostic_test PRIVATE
        SNESRECOMP_TRACE=0 SNESRECOMP_REVERSE_DEBUG=0)
    if(MSVC)
        target_compile_options(snesrecomp_runtime_next_diagnostic_test PRIVATE /W4)
    else()
        target_compile_options(snesrecomp_runtime_next_diagnostic_test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snesrecomp_runtime_next_diagnostic
        COMMAND snesrecomp_runtime_next_diagnostic_test)

    add_executable(snesrecomp_runtime_next_cpu_trace_test
        tests/cpu_trace_test.c src/cpu_trace.c)
    target_include_directories(snesrecomp_runtime_next_cpu_trace_test PRIVATE src)
    target_compile_definitions(snesrecomp_runtime_next_cpu_trace_test PRIVATE
        SNESRECOMP_TRACE=1 SNESRECOMP_REVERSE_DEBUG=0)
    if(MSVC)
        target_compile_options(snesrecomp_runtime_next_cpu_trace_test PRIVATE /W4)
    else()
        target_compile_options(snesrecomp_runtime_next_cpu_trace_test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snesrecomp_runtime_next_cpu_trace
        COMMAND snesrecomp_runtime_next_cpu_trace_test)

    add_executable(snesrecomp_runtime_next_spc_upload_test
        tests/spc_upload_test.c src/spc_upload.c)
    target_include_directories(snesrecomp_runtime_next_spc_upload_test PRIVATE src)
    if(MSVC)
        target_compile_options(snesrecomp_runtime_next_spc_upload_test PRIVATE /W4)
    else()
        target_compile_options(snesrecomp_runtime_next_spc_upload_test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snesrecomp_runtime_next_spc_upload
        COMMAND snesrecomp_runtime_next_spc_upload_test)

    add_executable(snesrecomp_runtime_next_support_test tests/support_test.c)
    target_link_libraries(snesrecomp_runtime_next_support_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_support
        COMMAND snesrecomp_runtime_next_support_test)

    add_executable(snesrecomp_runtime_next_sha256_test tests/sha256_test.c)
    target_link_libraries(snesrecomp_runtime_next_sha256_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_sha256
        COMMAND snesrecomp_runtime_next_sha256_test)

    add_executable(snesrecomp_runtime_next_framedump_test tests/framedump_test.c)
    target_link_libraries(snesrecomp_runtime_next_framedump_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_framedump
        COMMAND snesrecomp_runtime_next_framedump_test)

    add_executable(snesrecomp_runtime_next_ppu_test tests/ppu_test.c)
    target_link_libraries(snesrecomp_runtime_next_ppu_test PRIVATE
        snesrecomp_runtime_next_contract)
    add_test(NAME snesrecomp_runtime_next_ppu
        COMMAND snesrecomp_runtime_next_ppu_test)
endif()
