cmake_minimum_required(VERSION 3.25)
if(NOT BUILDER_DIST_BUILD OR NOT BUILDER_OUTPUT)
    message(FATAL_ERROR "Set BUILDER_DIST_BUILD to a configured Windows installer build and BUILDER_OUTPUT to a new .exe path")
endif()
get_filename_component(BUILDER_DIST_BUILD "${BUILDER_DIST_BUILD}" ABSOLUTE)
get_filename_component(BUILDER_OUTPUT "${BUILDER_OUTPUT}" ABSOLUTE)
if(EXISTS "${BUILDER_OUTPUT}" OR NOT BUILDER_OUTPUT MATCHES "\\.[eE][xX][eE]$")
    message(FATAL_ERROR "Choose a new .exe output path; existing executables are never replaced")
endif()
find_program(_go go REQUIRED)
file(STRINGS "${BUILDER_DIST_BUILD}/CMakeCache.txt" _target_os REGEX "^SNESBUILD_GOOS:STRING=")
file(STRINGS "${BUILDER_DIST_BUILD}/CMakeCache.txt" _target_arch REGEX "^SNESBUILD_GOARCH:STRING=")
if(NOT _target_os STREQUAL "SNESBUILD_GOOS:STRING=windows" OR
   NOT _target_arch MATCHES "^SNESBUILD_GOARCH:STRING=(amd64|arm64)$")
    message(FATAL_ERROR "Builder requires a configured windows/amd64 or windows/arm64 installer payload")
endif()
set(_arch "${CMAKE_MATCH_1}")
# The executable's version information uses the version its bundled tools carry.
file(STRINGS "${BUILDER_DIST_BUILD}/CMakeCache.txt" _version REGEX "^SNESBUILD_VERSION_STAMP:INTERNAL=.")
if(NOT _version)
    message(FATAL_ERROR "Reconfigure the Windows installer preset; its build records no release version")
endif()
string(REGEX REPLACE "^SNESBUILD_VERSION_STAMP:INTERNAL=" "" _version "${_version}")

# WebView2 is not bundled: the Builder uses the Evergreen runtime included with
# Windows 10/11 and explains how to install it when it is missing.
string(RANDOM LENGTH 12 ALPHABET abcdef0123456789 _suffix)
set(_stage "${BUILDER_DIST_BUILD}/builder-shell-${_suffix}")
file(MAKE_DIRECTORY "${_stage}")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILDER_DIST_BUILD}" --parallel 2
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILDER_DIST_BUILD}" --prefix "${_stage}/payload"
    OUTPUT_VARIABLE _install_log COMMAND_ERROR_IS_FATAL ANY)
set(_tags production,wv2runtime.error)
if(BUILDER_SMOKE_TEST)
    string(APPEND _tags ",smoketest")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GOOS=windows" "GOARCH=${_arch}" "CGO_ENABLED=0"
    "${_go}" -C "${CMAKE_CURRENT_LIST_DIR}" build -tags "${_tags}" -trimpath
    -ldflags "-s -w -H windowsgui" -o "${_stage}/shell.exe" . COMMAND_ERROR_IS_FATAL ANY)
# The packager runs on the maintainer host, not the Windows target. Do not
# inherit cross-compilation variables into go run.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=GOOS --unset=GOARCH --unset=CGO_ENABLED
    "${_go}" -C "${CMAKE_CURRENT_LIST_DIR}" run ./cmd/windows-package
    --payload "${_stage}/payload" --shell "${_stage}/shell.exe" --arch "${_arch}"
    --version "${_version}" --output "${BUILDER_OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "Windows Builder artifact: ${BUILDER_OUTPUT}")
message(STATUS "Unsigned prototype. Perform native Windows acceptance tests before distribution; sign only AFTER packaging.")
