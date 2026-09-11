cmake_minimum_required(VERSION 3.25)
if(NOT CMAKE_HOST_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
    message(FATAL_ERROR "Linux SDK cross-packaging currently supports macOS and Linux hosts")
endif()
if(NOT BUILDER_DIST_BUILD OR NOT BUILDER_OUTPUT)
    message(FATAL_ERROR "Set BUILDER_DIST_BUILD and a new BUILDER_OUTPUT .AppImage path")
endif()
get_filename_component(BUILDER_DIST_BUILD "${BUILDER_DIST_BUILD}" ABSOLUTE)
get_filename_component(BUILDER_OUTPUT "${BUILDER_OUTPUT}" ABSOLUTE)
if(EXISTS "${BUILDER_OUTPUT}" OR IS_SYMLINK "${BUILDER_OUTPUT}")
    message(FATAL_ERROR "Choose a new output; existing apps are never replaced")
endif()
file(STRINGS "${BUILDER_DIST_BUILD}/CMakeCache.txt" _os REGEX "^SNESBUILD_GOOS:STRING=")
file(STRINGS "${BUILDER_DIST_BUILD}/CMakeCache.txt" _arch REGEX "^SNESBUILD_GOARCH:STRING=")
file(STRINGS "${BUILDER_DIST_BUILD}/CMakeCache.txt" _deck REGEX "^SNESBUILD_STEAM_DECK:BOOL=")
if(NOT _os STREQUAL "SNESBUILD_GOOS:STRING=linux" OR
   NOT _arch MATCHES "^SNESBUILD_GOARCH:STRING=(amd64|arm64)$")
    message(FATAL_ERROR "Expected a configured linux/amd64 or linux/arm64 payload")
endif()
set(_arch "${CMAKE_MATCH_1}")
set(_abi_options)
if(_deck STREQUAL "SNESBUILD_STEAM_DECK:BOOL=ON")
    set(_abi_options --glibc-max 2.36)
endif()
foreach(_tool go pkg-config xz zstd mksquashfs glib-compile-schemas update-mime-database)
    find_program(_program_${_tool} NAMES "${_tool}" REQUIRED)
endforeach()
set(_go "${_program_go}")
if(NOT BUILDER_DOWNLOAD_CACHE)
    set(BUILDER_DOWNLOAD_CACHE "${CMAKE_CURRENT_LIST_DIR}/../packaging/cache/desktop")
endif()
get_filename_component(BUILDER_DOWNLOAD_CACHE "${BUILDER_DOWNLOAD_CACHE}" ABSOLUTE)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED ENV{BUILDER_LINUX_SDK_VOLUME})
    execute_process(COMMAND /bin/sh "${CMAKE_CURRENT_LIST_DIR}/with-linux-sdk-volume.sh"
        "${BUILDER_DOWNLOAD_CACHE}" "${CMAKE_COMMAND}"
        "-DBUILDER_DIST_BUILD=${BUILDER_DIST_BUILD}" "-DBUILDER_OUTPUT=${BUILDER_OUTPUT}"
        "-DBUILDER_DOWNLOAD_CACHE=${BUILDER_DOWNLOAD_CACHE}" "-DBUILDER_SMOKE_TEST=${BUILDER_SMOKE_TEST}"
        -P "${CMAKE_CURRENT_LIST_FILE}" COMMAND_ERROR_IS_FATAL ANY)
    return()
endif()
set(_sdk_parent "${BUILDER_DOWNLOAD_CACHE}/linux-sdk")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(_sdk_parent "$ENV{BUILDER_LINUX_SDK_VOLUME}")
endif()
set(_lock "${CMAKE_CURRENT_LIST_DIR}/linux-sdk-${_arch}.json")
file(SHA256 "${_lock}" _lock_sha)
set(_sdk "${_sdk_parent}/${_arch}-${_lock_sha}")
execute_process(COMMAND "${_go}" -C "${CMAKE_CURRENT_LIST_DIR}" run ./cmd/linux-sdk --mode stage
    --lock "${_lock}" --cache "${BUILDER_DOWNLOAD_CACHE}/linux-debs" --output "${_sdk}"
    COMMAND_ERROR_IS_FATAL ANY)

# Reuse the runner's pinned HOST compiler, never the Linux compiler in payload.
get_filename_component(_snes "${CMAKE_CURRENT_LIST_DIR}/../../snesrecomp-go" ABSOLUTE)
set(_zig_cache "${CMAKE_CURRENT_LIST_DIR}/../packaging/build/host-toolchain")
execute_process(COMMAND "${_go}" -C "${_snes}" run ./cmd/snesbuild toolchain fetch
    --cache-dir "${_zig_cache}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${_go}" -C "${_snes}" run ./cmd/snesbuild toolchain pin
    OUTPUT_VARIABLE _pin OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(_pin UNIX_COMMAND "${_pin}")
list(GET _pin 2 _archive)
string(REGEX REPLACE "\\.(tar\\.xz|zip)$" "" _zig_dir "${_archive}")
set(_zig "${_zig_cache}/${_zig_dir}/zig")
if(NOT EXISTS "${_zig}")
    message(FATAL_ERROR "Pinned host Zig was not staged at ${_zig}")
endif()
string(RANDOM LENGTH 12 ALPHABET abcdef0123456789 _suffix)
set(_stage "${BUILDER_DIST_BUILD}/builder-shell-${_suffix}")
file(MAKE_DIRECTORY "${_stage}")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILDER_DIST_BUILD}" --parallel 2
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILDER_DIST_BUILD}" --prefix "${_stage}/payload"
    OUTPUT_VARIABLE _install_log COMMAND_ERROR_IS_FATAL ANY)
set(_smoke)
if(BUILDER_SMOKE_TEST)
    set(_smoke --smoke)
endif()
execute_process(COMMAND "${_go}" -C "${CMAKE_CURRENT_LIST_DIR}" run ./cmd/linux-sdk --mode build
    --sdk "${_sdk}" --zig "${_zig}" --output "${_stage}/shell" ${_smoke}
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${_go}" -C "${CMAKE_CURRENT_LIST_DIR}" run ./cmd/package
    --source "${_stage}/payload" --shell "${_stage}/shell" --linux-sdk "${_sdk}"
    --output "${BUILDER_OUTPUT}" ${_abi_options} COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "Cross-built Linux Builder artifact: ${BUILDER_OUTPUT}")
