# Manifest for the project-owned portable replacement runner. Attributed
# third-party components and their compatible licenses are listed in NOTICE.md.

if(NOT CMAKE_CXX_COMPILER_LOADED)
    message(FATAL_ERROR
        "snesrecomp runtime requires C++20 for its private S-DSP accuracy "
        "unit, but this project has not enabled C++. Declare the project as "
        "project(<name> LANGUAGES C CXX), or call enable_language(CXX), "
        "before including runtime/runner.cmake.")
endif()

set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(SNESRECOMP_RUNNER_DEVICE_ROOT ${SNESRECOMP_RUNNER_ROOT})
set(SNESRECOMP_RUNNER_CRC32_SOURCE ${SNESRECOMP_RUNNER_ROOT}/src/support/crc32.c)
set(SNESRECOMP_RUNNER_DSP_SOURCE ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp.c)
set(SNESRECOMP_RUNNER_DSP_ACCURACY_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp_accuracy_unit.cpp)
set(SNESRECOMP_RUNNER_SAVELOAD_SOURCE ${SNESRECOMP_RUNNER_ROOT}/src/snes/saveload.c)

include(${SNESRECOMP_RUNNER_ROOT}/sources.cmake)

option(SNESRECOMP_ENABLE_TRACE "Build local observability rings and tripwires" OFF)
if(SNESRECOMP_ENABLE_TRACE)
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        ${SNESRECOMP_RUNNER_ROOT}/src/support/generated_trace_stub.c)
endif()

# The JSONL/watch recorder is a host-side diagnostic consumer of the public
# runner observers. Keep it separate from the observer ABI itself so release
# games can retain supported observer services without carrying the recorder,
# its formatting code, or per-entry/call probes. Deep generated tracing needs
# the recorder and therefore implies it even when the explicit option is off.
option(SNESRECOMP_ENABLE_TRACE_RECORDER
    "Build the runtime-selectable JSONL/watch trace recorder" OFF)
if(SNESRECOMP_ENABLE_TRACE OR SNESRECOMP_ENABLE_TRACE_RECORDER)
    set(_SNESRECOMP_TRACE_RECORDER 1)
else()
    set(_SNESRECOMP_TRACE_RECORDER 0)
endif()

option(SNESRECOMP_ENABLE_SIMD
    "Enable runner SIMD implementations supported by the build target" ON)
option(SNESRECOMP_ENABLE_IPO
    "Enable supported interprocedural optimization in release runner targets" ON)
set(SNESRECOMP_PPU_BIT_WORD_BITS "auto" CACHE STRING
    "Runner PPU bitset word width: auto, 32, or 64")
set_property(CACHE SNESRECOMP_PPU_BIT_WORD_BITS PROPERTY STRINGS auto 32 64)
if(NOT SNESRECOMP_PPU_BIT_WORD_BITS STREQUAL "auto" AND
   NOT SNESRECOMP_PPU_BIT_WORD_BITS STREQUAL "32" AND
   NOT SNESRECOMP_PPU_BIT_WORD_BITS STREQUAL "64")
    message(FATAL_ERROR
        "SNESRECOMP_PPU_BIT_WORD_BITS must be auto, 32, or 64")
endif()

set(SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/include
)
set(SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
    ${SNESRECOMP_RUNNER_ROOT}/src/core
    ${SNESRECOMP_RUNNER_ROOT}/src/runner
    ${SNESRECOMP_RUNNER_ROOT}/src/support
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/accuracy/include
)

# Configure a target that compiles runner implementation sources. Consumers
# inherit only the supported SDK headers; implementation roots never propagate.
function(snesrecomp_configure_runtime_target target)
    if(DEFINED CMAKE_INSTALL_INCLUDEDIR AND
       NOT CMAKE_INSTALL_INCLUDEDIR STREQUAL "")
        set(_snesrecomp_install_include ${CMAKE_INSTALL_INCLUDEDIR})
    else()
        set(_snesrecomp_install_include include)
    endif()
    target_compile_features(${target} PUBLIC c_std_11)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(SNESRECOMP_ENABLE_IPO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _snesrecomp_ipo_supported
                            OUTPUT _snesrecomp_ipo_error)
        if(_snesrecomp_ipo_supported)
            set_property(TARGET ${target} PROPERTY
                INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            set_property(TARGET ${target} PROPERTY
                INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
            set_property(TARGET ${target} PROPERTY
                INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
        else()
            message(STATUS
                "runner IPO unavailable for ${target}: ${_snesrecomp_ipo_error}")
        endif()
    endif()
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
            $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
    endif()
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS}>
            $<INSTALL_INTERFACE:${_snesrecomp_install_include}>
        PRIVATE ${SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS})
    if(SNESRECOMP_ENABLE_SIMD)
        target_compile_definitions(${target} PRIVATE SNESRECOMP_ENABLE_SIMD=1)
    else()
        target_compile_definitions(${target} PRIVATE SNESRECOMP_ENABLE_SIMD=0)
    endif()
    if(SNESRECOMP_ENABLE_TRACE)
        target_compile_definitions(${target} PRIVATE SNESRECOMP_TRACE=1)
    else()
        target_compile_definitions(${target} PRIVATE SNESRECOMP_TRACE=0)
    endif()
    # game/cpu.h removes its generated entry/call probes with the same value,
    # so the enabled definition must propagate to game-module consumers. The
    # header defaults an absent definition to zero; leaving release consumers
    # undefined avoids exporting a contradictory `=0` into a diagnostic target
    # that deliberately compiles a trace-capable recorder beside the library.
    if(_SNESRECOMP_TRACE_RECORDER)
        target_compile_definitions(${target} PUBLIC
            SNESRECOMP_TRACE_RECORDER=1)
    endif()
    if(NOT SNESRECOMP_PPU_BIT_WORD_BITS STREQUAL "auto")
        target_compile_definitions(${target} PRIVATE
            SNESRECOMP_PPU_BIT_WORD_BITS=${SNESRECOMP_PPU_BIT_WORD_BITS})
    endif()
endfunction()
