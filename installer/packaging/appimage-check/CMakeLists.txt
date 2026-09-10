cmake_minimum_required(VERSION 3.21)
project(ActRaiserAppImageCheck NONE)

# Maintainer-only, ROM-free acceptance. This is not another build path for
# players: it runs the production Go packager with the installer's own tools.
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "check-appimage must run on Linux (a Linux VM is fine); this check cannot be replaced by a cross-build")
endif()
find_program(GO_EXECUTABLE go REQUIRED)
find_program(CC_EXECUTABLE cc REQUIRED)
find_program(PKG_CONFIG_EXECUTABLE pkg-config REQUIRED)
find_program(DESKTOP_FILE_VALIDATE_EXECUTABLE desktop-file-validate REQUIRED)
execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}" --exists sdl3 sdl3-ttf
    RESULT_VARIABLE _sdl_result)
if(NOT _sdl_result EQUAL 0)
    message(FATAL_ERROR "Acceptance probe needs SDL3 and SDL3_ttf development libraries; see docs/desktop-packaging.md")
endif()
execute_process(COMMAND "${GO_EXECUTABLE}" env GOHOSTARCH
    OUTPUT_VARIABLE SNESBUILD_GOARCH OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
set(SNESBUILD_GOOS linux)
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(APPIMAGE_CHECK_CACHE "${_repo}/installer/packaging/cache" CACHE PATH "Release packaging tool download cache")
set(_cache_dir "${APPIMAGE_CHECK_CACHE}")
file(MAKE_DIRECTORY "${_cache_dir}")
# One set of URLs, checksums, install destinations and license notices for both
# release installers and acceptance. Downloads are never done by the player.
include("${CMAKE_CURRENT_LIST_DIR}/../appimage.cmake")

set(_stage "${CMAKE_BINARY_DIR}/stage")
add_custom_target(check-appimage
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${_stage}"
    COMMAND "${CMAKE_COMMAND}" -E env
        AR_APPIMAGE_ACCEPTANCE=1
        "AR_APPIMAGE_TOOL=${_stage}/utils/tools/appimagetool"
        "AR_APPIMAGE_RUNTIME=${_stage}/utils/tools/appimage-runtime"
        "${GO_EXECUTABLE}" -C "${_repo}/installer" test -count=1 -v
        -timeout=10m -run "^TestAppImageAcceptance$" ./internal/desktop
    USES_TERMINAL VERBATIM)
