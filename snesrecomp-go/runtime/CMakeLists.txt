cmake_minimum_required(VERSION 3.16)
project(snesrecomp_runtime VERSION 2.0.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(SNESRECOMP_BUILD_DOCS
    "Generate the browsable public API reference with Doxygen" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-fno-common)
endif()

set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})
include(${SNESRECOMP_RUNNER_ROOT}/runner.cmake)
set(SNESRECOMP_RUNTIME_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS})

add_library(snesrecomp_runtime STATIC
    ${SNESRECOMP_RUNNER_SOURCES})
add_library(snesrecomp::runtime ALIAS snesrecomp_runtime)
set_target_properties(snesrecomp_runtime PROPERTIES
    EXPORT_NAME runtime)
snesrecomp_configure_runtime_target(snesrecomp_runtime)
if(SNESRECOMP_ENABLE_SIMD)
    message(STATUS "runtime SIMD: enabled when supported by build target")
else()
    message(STATUS "runtime SIMD: portable implementation forced")
endif()

configure_package_config_file(
    cmake/snesrecomp-runtime-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/snesrecomp-runtime-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/snesrecomp-runtime)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/snesrecomp-runtime-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(TARGETS snesrecomp_runtime
    EXPORT snesrecompRuntimeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT snesrecompRuntimeTargets
    FILE snesrecomp-runtime-targets.cmake
    NAMESPACE snesrecomp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/snesrecomp-runtime)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/snesrecomp-runtime-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/snesrecomp-runtime-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/snesrecomp-runtime)
install(FILES LICENSE README.md PROVENANCE.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/snesrecomp-runtime)
install(FILES
    docs/README.md
    docs/API_REFERENCE.md
    docs/GAME_ENHANCEMENT_INTEGRATION.md
    docs/RUNTIME.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/snesrecomp-runtime/docs)
install(DIRECTORY examples/
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/snesrecomp-runtime/examples
    FILES_MATCHING PATTERN "*.c" PATTERN "*.h" PATTERN "*.md"
                   PATTERN "CMakeLists.txt")

if(SNESRECOMP_BUILD_DOCS)
    find_package(Doxygen 1.8.13 REQUIRED)
    configure_file(Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    file(GLOB_RECURSE SNESRECOMP_RUNTIME_DOCUMENTATION_INPUTS
        CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/snesrecomp/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/*.md
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/*.dox)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/api-docs/html/index.html
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
            ${SNESRECOMP_RUNTIME_DOCUMENTATION_INPUTS}
        COMMENT "Generating snesrecomp Runtime SDK API reference"
        VERBATIM)
    add_custom_target(snesrecomp_runtime_docs ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/api-docs/html/index.html)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/api-docs/html
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/snesrecomp-runtime/api
        OPTIONAL)
endif()

if(MSVC)
    target_compile_options(snesrecomp_runtime PRIVATE /W4)
else()
    target_compile_options(snesrecomp_runtime PRIVATE
        -Wall -Wextra -Wpedantic -Wmissing-prototypes -Wstrict-prototypes)
endif()

include(CTest)
if(BUILD_TESTING)
    enable_language(CXX)
    add_library(snesrecomp_runtime_minimal_game_example OBJECT
        examples/minimal_game/game_module.c)
    target_include_directories(snesrecomp_runtime_minimal_game_example PRIVATE
        examples/minimal_game)
    target_link_libraries(snesrecomp_runtime_minimal_game_example PRIVATE
        snesrecomp::runtime)
    if(MSVC)
        target_compile_options(snesrecomp_runtime_minimal_game_example PRIVATE /W4)
    else()
        target_compile_options(snesrecomp_runtime_minimal_game_example PRIVATE
            -Wall -Wextra -Wpedantic -Wmissing-prototypes -Wstrict-prototypes)
    endif()

    add_executable(snesrecomp_runtime_public_headers_test
        tests/public_headers_test.c)
    target_link_libraries(snesrecomp_runtime_public_headers_test PRIVATE
        snesrecomp::runtime)
    add_test(NAME snesrecomp_runtime_public_headers
        COMMAND snesrecomp_runtime_public_headers_test)

    # Each domain header must compile from a clean translation unit rather
    # than relying on the umbrella header's include order.
    add_library(snesrecomp_runtime_narrow_headers_test OBJECT
        tests/public_header_base_test.c
        tests/public_header_ppu_test.c
        tests/public_header_events_test.c
        tests/public_header_audio_test.c
        tests/public_header_mutation_test.c
        tests/public_header_api_test.c)
    target_link_libraries(snesrecomp_runtime_narrow_headers_test PRIVATE
        snesrecomp::runtime)

    add_executable(snesrecomp_runtime_cpp_headers_test
        tests/public_headers_cpp_test.cpp)
    target_compile_features(snesrecomp_runtime_cpp_headers_test PRIVATE
        cxx_std_17)
    target_link_libraries(snesrecomp_runtime_cpp_headers_test PRIVATE
        snesrecomp::runtime)
    add_test(NAME snesrecomp_runtime_cpp_headers
        COMMAND snesrecomp_runtime_cpp_headers_test)

    # Runtime contract tests intentionally inspect private hardware state. Keep
    # that access target-local; consumers inherit only `include/`.
    add_executable(snesrecomp_runtime_saveload_test
        tests/saveload_test.c)
    target_link_libraries(snesrecomp_runtime_saveload_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_saveload
        COMMAND snesrecomp_runtime_saveload_test)

    add_executable(snesrecomp_runtime_contract_test
        tests/contract_test.c tests/apu_sync_stub.c)
    target_link_libraries(snesrecomp_runtime_contract_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_contract
        COMMAND snesrecomp_runtime_contract_test)

    add_executable(snesrecomp_runtime_abi_test tests/abi_test.c)
    target_link_libraries(snesrecomp_runtime_abi_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_abi
        COMMAND snesrecomp_runtime_abi_test)

    add_executable(snesrecomp_runtime_cart_test
        tests/cart_test.c tests/runner_event_stub.c)
    target_link_libraries(snesrecomp_runtime_cart_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_cart
        COMMAND snesrecomp_runtime_cart_test)

    add_executable(snesrecomp_runtime_rom_test tests/rom_test.c)
    target_link_libraries(snesrecomp_runtime_rom_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_rom
        COMMAND snesrecomp_runtime_rom_test)

    add_executable(snesrecomp_runtime_dma_test
        tests/dma_test.c tests/runner_event_stub.c)
    target_link_libraries(snesrecomp_runtime_dma_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_dma
        COMMAND snesrecomp_runtime_dma_test)

    add_executable(snesrecomp_runtime_snes_test tests/snes_test.c)
    target_link_libraries(snesrecomp_runtime_snes_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_snes
        COMMAND snesrecomp_runtime_snes_test)

    add_executable(snesrecomp_runtime_apu_test tests/apu_test.c)
    target_link_libraries(snesrecomp_runtime_apu_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_apu
        COMMAND snesrecomp_runtime_apu_test)

    add_executable(snesrecomp_runtime_spc_test tests/spc_test.c)
    target_link_libraries(snesrecomp_runtime_spc_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_spc
        COMMAND snesrecomp_runtime_spc_test)

    add_executable(snesrecomp_runtime_dsp_test tests/dsp_test.c)
    target_link_libraries(snesrecomp_runtime_dsp_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_dsp
        COMMAND snesrecomp_runtime_dsp_test)

    add_executable(snesrecomp_runtime_audio_shadow_test
        tests/audio_shadow_test.c)
    target_link_libraries(snesrecomp_runtime_audio_shadow_test PRIVATE
        snesrecomp_runtime)
    if(NOT MSVC)
        target_link_libraries(snesrecomp_runtime_audio_shadow_test PRIVATE m)
    endif()
    add_test(NAME snesrecomp_runtime_audio_shadow
        COMMAND snesrecomp_runtime_audio_shadow_test)

    add_executable(snesrecomp_runtime_msu1_test tests/msu1_test.c)
    target_compile_definitions(snesrecomp_runtime_msu1_test PRIVATE
        MSU1_TEST_BASE="${CMAKE_CURRENT_BINARY_DIR}/runtime-msu1-fixture")
    target_link_libraries(snesrecomp_runtime_msu1_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_msu1
        COMMAND snesrecomp_runtime_msu1_test)

    add_executable(snesrecomp_runtime_color_lut_test
        tests/color_lut_test.c)
    target_link_libraries(snesrecomp_runtime_color_lut_test PRIVATE
        snesrecomp_runtime)
    if(NOT MSVC)
        target_link_libraries(snesrecomp_runtime_color_lut_test PRIVATE m)
    endif()
    add_test(NAME snesrecomp_runtime_color_lut
        COMMAND snesrecomp_runtime_color_lut_test)

    add_executable(snesrecomp_runtime_audio_trace_test
        tests/audio_trace_test.c)
    target_compile_definitions(snesrecomp_runtime_audio_trace_test PRIVATE
        AUDIO_TRACE_TEST_WAV="${CMAKE_CURRENT_BINARY_DIR}/runtime-audio-trace.wav")
    target_link_libraries(snesrecomp_runtime_audio_trace_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_audio_trace
        COMMAND snesrecomp_runtime_audio_trace_test)

    add_executable(snesrecomp_runtime_runtime_trace_test
        tests/runtime_trace_test.c tests/apu_sync_stub.c)
    target_compile_definitions(snesrecomp_runtime_runtime_trace_test PRIVATE
        RUNTIME_TRACE_TEST_PATH="${CMAKE_CURRENT_BINARY_DIR}/runtime-trace.jsonl")
    target_link_libraries(snesrecomp_runtime_runtime_trace_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_runtime_trace
        COMMAND snesrecomp_runtime_runtime_trace_test)

    add_executable(snesrecomp_runtime_util_test tests/util_test.c)
    target_link_libraries(snesrecomp_runtime_util_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_util
        COMMAND snesrecomp_runtime_util_test)

    add_executable(snesrecomp_runtime_launcher_test tests/launcher_test.c)
    target_compile_definitions(snesrecomp_runtime_launcher_test PRIVATE
        LAUNCHER_TEST_ROM="${CMAKE_CURRENT_BINARY_DIR}/runtime-launcher.sfc")
    target_link_libraries(snesrecomp_runtime_launcher_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_launcher
        COMMAND snesrecomp_runtime_launcher_test)

    add_executable(snesrecomp_runtime_keybinds_test tests/keybinds_test.c)
    target_compile_definitions(snesrecomp_runtime_keybinds_test PRIVATE
        KEYBINDS_TEST_EXE="${CMAKE_CURRENT_BINARY_DIR}/runtime-test-runner"
        KEYBINDS_TEST_INI="${CMAKE_CURRENT_BINARY_DIR}/keybinds.ini")
    target_link_libraries(snesrecomp_runtime_keybinds_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_keybinds
        COMMAND snesrecomp_runtime_keybinds_test)

    add_executable(snesrecomp_runtime_cpu_state_test tests/cpu_state_test.c)
    target_link_libraries(snesrecomp_runtime_cpu_state_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_cpu_state
        COMMAND snesrecomp_runtime_cpu_state_test)

    add_executable(snesrecomp_runtime_cpu_infra_test
        tests/cpu_infra_test.c src/core/common_cpu_infra.c
        src/runner/runner_game_module.c src/snes/saveload.c)
    target_include_directories(snesrecomp_runtime_cpu_infra_test PRIVATE
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNTIME_PRIVATE_INCLUDE_DIRS})
    target_compile_definitions(snesrecomp_runtime_cpu_infra_test PRIVATE
        SNESRECOMP_WATCHDOG=0 SNESRECOMP_TRACE=0 SNESRECOMP_REVERSE_DEBUG=0)
    if(MSVC)
        target_compile_options(snesrecomp_runtime_cpu_infra_test PRIVATE /W4)
    else()
        target_compile_options(snesrecomp_runtime_cpu_infra_test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snesrecomp_runtime_cpu_infra
        COMMAND snesrecomp_runtime_cpu_infra_test)

    add_executable(snesrecomp_runtime_diagnostic_test
        tests/diagnostic_test.c src/core/diagnostic.c)
    target_include_directories(snesrecomp_runtime_diagnostic_test PRIVATE
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNTIME_PRIVATE_INCLUDE_DIRS})
    target_compile_definitions(snesrecomp_runtime_diagnostic_test PRIVATE
        SNESRECOMP_TRACE=0 SNESRECOMP_REVERSE_DEBUG=0)
    if(MSVC)
        target_compile_options(snesrecomp_runtime_diagnostic_test PRIVATE /W4)
    else()
        target_compile_options(snesrecomp_runtime_diagnostic_test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snesrecomp_runtime_diagnostic
        COMMAND snesrecomp_runtime_diagnostic_test)

    add_executable(snesrecomp_runtime_cpu_trace_test
        tests/cpu_trace_test.c src/core/cpu_trace.c)
    target_include_directories(snesrecomp_runtime_cpu_trace_test PRIVATE
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNTIME_PRIVATE_INCLUDE_DIRS})
    target_compile_definitions(snesrecomp_runtime_cpu_trace_test PRIVATE
        SNESRECOMP_TRACE=1 SNESRECOMP_REVERSE_DEBUG=0)
    if(MSVC)
        target_compile_options(snesrecomp_runtime_cpu_trace_test PRIVATE /W4)
    else()
        target_compile_options(snesrecomp_runtime_cpu_trace_test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snesrecomp_runtime_cpu_trace
        COMMAND snesrecomp_runtime_cpu_trace_test)

    add_executable(snesrecomp_runtime_spc_upload_test
        tests/spc_upload_test.c src/support/spc_upload.c)
    target_include_directories(snesrecomp_runtime_spc_upload_test PRIVATE
        ${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}
        ${SNESRECOMP_RUNTIME_PRIVATE_INCLUDE_DIRS})
    if(MSVC)
        target_compile_options(snesrecomp_runtime_spc_upload_test PRIVATE /W4)
    else()
        target_compile_options(snesrecomp_runtime_spc_upload_test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snesrecomp_runtime_spc_upload
        COMMAND snesrecomp_runtime_spc_upload_test)

    add_executable(snesrecomp_runtime_support_test tests/support_test.c)
    target_link_libraries(snesrecomp_runtime_support_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_support
        COMMAND snesrecomp_runtime_support_test)

    add_executable(snesrecomp_runtime_sha256_test tests/sha256_test.c)
    target_link_libraries(snesrecomp_runtime_sha256_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_sha256
        COMMAND snesrecomp_runtime_sha256_test)

    add_executable(snesrecomp_runtime_framedump_test tests/framedump_test.c)
    target_link_libraries(snesrecomp_runtime_framedump_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_framedump
        COMMAND snesrecomp_runtime_framedump_test)

    add_executable(snesrecomp_runtime_ppu_test
        tests/ppu_test.c tests/runner_event_stub.c)
    target_link_libraries(snesrecomp_runtime_ppu_test PRIVATE
        snesrecomp_runtime)
    add_test(NAME snesrecomp_runtime_ppu
        COMMAND snesrecomp_runtime_ppu_test)

    set(_SNESRECOMP_PRIVATE_CONTRACT_TESTS
        snesrecomp_runtime_saveload_test
        snesrecomp_runtime_contract_test
        snesrecomp_runtime_abi_test
        snesrecomp_runtime_cart_test
        snesrecomp_runtime_rom_test
        snesrecomp_runtime_dma_test
        snesrecomp_runtime_snes_test
        snesrecomp_runtime_apu_test
        snesrecomp_runtime_spc_test
        snesrecomp_runtime_dsp_test
        snesrecomp_runtime_audio_shadow_test
        snesrecomp_runtime_msu1_test
        snesrecomp_runtime_color_lut_test
        snesrecomp_runtime_audio_trace_test
        snesrecomp_runtime_runtime_trace_test
        snesrecomp_runtime_util_test
        snesrecomp_runtime_launcher_test
        snesrecomp_runtime_keybinds_test
        snesrecomp_runtime_cpu_state_test
        snesrecomp_runtime_support_test
        snesrecomp_runtime_sha256_test
        snesrecomp_runtime_framedump_test
        snesrecomp_runtime_ppu_test)
    foreach(_target IN LISTS _SNESRECOMP_PRIVATE_CONTRACT_TESTS)
        target_include_directories(${_target} PRIVATE
            ${SNESRECOMP_RUNTIME_PRIVATE_INCLUDE_DIRS})
        if(NOT SNESRECOMP_PPU_BIT_WORD_BITS STREQUAL "auto")
            target_compile_definitions(${_target} PRIVATE
                SNESRECOMP_PPU_BIT_WORD_BITS=${SNESRECOMP_PPU_BIT_WORD_BITS})
        endif()
    endforeach()

    # These focused tests compile selected private implementation units instead
    # of linking the complete archive and must use the same private layout.
    if(NOT SNESRECOMP_PPU_BIT_WORD_BITS STREQUAL "auto")
        foreach(_target
                snesrecomp_runtime_cpu_infra_test
                snesrecomp_runtime_diagnostic_test
                snesrecomp_runtime_cpu_trace_test
                snesrecomp_runtime_spc_upload_test)
            target_compile_definitions(${_target} PRIVATE
                SNESRECOMP_PPU_BIT_WORD_BITS=${SNESRECOMP_PPU_BIT_WORD_BITS})
        endforeach()
    endif()

    add_test(NAME snesrecomp_runtime_installed_consumer
        COMMAND ${CMAKE_COMMAND}
            -DRUNTIME_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DRUNTIME_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DRUNTIME_TEST_CONFIG=$<CONFIG>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_consumer_test.cmake)
endif()
