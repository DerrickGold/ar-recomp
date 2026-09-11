cmake_minimum_required(VERSION 3.25)
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "The macOS Builder prototype must be packaged on macOS")
endif()
if(NOT BUILDER_DIST_BUILD OR NOT BUILDER_OUTPUT)
    message(FATAL_ERROR "Set BUILDER_DIST_BUILD to a configured macOS installer build and BUILDER_OUTPUT to a new .app path")
endif()
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(BUILDER_DIST_BUILD "${BUILDER_DIST_BUILD}" ABSOLUTE)
get_filename_component(BUILDER_OUTPUT "${BUILDER_OUTPUT}" ABSOLUTE)
find_program(_go go REQUIRED)
file(STRINGS "${BUILDER_DIST_BUILD}/CMakeCache.txt" _target_os REGEX "^SNESBUILD_GOOS:STRING=")
file(STRINGS "${BUILDER_DIST_BUILD}/CMakeCache.txt" _target_arch REGEX "^SNESBUILD_GOARCH:STRING=")
if(NOT _target_os STREQUAL "SNESBUILD_GOOS:STRING=darwin" OR
   NOT _target_arch MATCHES "^SNESBUILD_GOARCH:STRING=(arm64|amd64)$")
    message(FATAL_ERROR "Builder requires a darwin/arm64 or darwin/amd64 installer payload")
endif()
set(_arch "${CMAKE_MATCH_1}")
set(_clang_arch "${_arch}")
if(_arch STREQUAL "amd64")
    set(_clang_arch x86_64)
endif()
if(EXISTS "${BUILDER_OUTPUT}")
    message(FATAL_ERROR "Choose a new BUILDER_OUTPUT path; existing apps are never replaced")
endif()
string(RANDOM LENGTH 12 ALPHABET abcdef0123456789 _suffix)
set(_stage "${BUILDER_DIST_BUILD}/builder-shell-${_suffix}")
file(MAKE_DIRECTORY "${_stage}")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILDER_DIST_BUILD}" --parallel 2
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILDER_DIST_BUILD}" --prefix "${_stage}/payload"
    OUTPUT_VARIABLE _install_log COMMAND_ERROR_IS_FATAL ANY)
set(_tags production)
if(BUILDER_SMOKE_TEST)
    string(APPEND _tags ",smoketest")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GOOS=darwin" "GOARCH=${_arch}" "CGO_ENABLED=1"
    "CC=clang -arch ${_clang_arch}" "CXX=clang++ -arch ${_clang_arch}" "MACOSX_DEPLOYMENT_TARGET=12.0"
    "${_go}" -C "${CMAKE_CURRENT_LIST_DIR}" build -tags "${_tags}" -trimpath
    -ldflags "-s -w" -o "${_stage}/shell" . COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${_go}" -C "${CMAKE_CURRENT_LIST_DIR}" run ./cmd/package
    --source "${_stage}/payload" --shell "${_stage}/shell" --output "${BUILDER_OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "Builder feasibility artifact: ${BUILDER_OUTPUT}")
