# Packaging project for the self-contained ActRaiser Recomp distribution.
#
# Two modes, selected by whether a target platform is given:
#
#  * Orchestrator (no -DSNESBUILD_GOOS): defines a `release` target that
#    builds every platform bundle by invoking the per-platform presets, so
#    one command from the repo root produces all of them into release/.
#      cmake --workflow --preset release          (from the repo root)
#
#  * Single platform (-DSNESBUILD_GOOS/-GOARCH, set by the package-* presets):
#    cross-builds snesbuild, stages that platform's pinned build dependencies
#    (Zig, and prebuilt SDL3 where an official redistributable exists), and
#    assembles a STANDALONE bundle with the whole buildable game project so a
#    user needs only this archive and their own ROM — no repo checkout, no
#    developer tools. Go cross-compiles every target from any host.
#
# Only generic tools, the project's own authored source, and redistributable
# third-party components ship — never a ROM, generated C, or the media assets
# (the game-assets/ manifest + folders ship as an empty template for the user
# to fill).

cmake_minimum_required(VERSION 3.25)
project(actraiser-recomp-dist NONE)

set(SNESBUILD_GOOS "" CACHE STRING "Target GOOS (darwin, linux, windows); empty = orchestrator")
set(SNESBUILD_GOARCH "" CACHE STRING "Target GOARCH (arm64, amd64); empty = orchestrator")

set(SNESBUILD_ALL_PRESETS
    macos-arm64 macos-x86_64 linux-x86_64 linux-arm64 windows-x86_64 windows-arm64)

# ── Orchestrator mode ────────────────────────────────────────────────────────
if(NOT SNESBUILD_GOOS AND NOT SNESBUILD_GOARCH)
    get_filename_component(_release_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../release ABSOLUTE)
    add_custom_target(release
        COMMENT "Building every platform bundle into release/")
    foreach(_preset IN LISTS SNESBUILD_ALL_PRESETS)
        # Package the platform, then immediately drop its build tree (a fresh
        # ~180 MB Zig extraction lives there) so intermediate data does not
        # accumulate. The download cache (../cache) is untouched, so a re-run
        # re-extracts in seconds without re-downloading. The orchestrator's own
        # build dir (build/orchestrator) is left alone — it is in use here.
        add_custom_command(TARGET release POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "=== packaging ${_preset} ==="
            COMMAND ${CMAKE_COMMAND} --workflow --preset package-${_preset}
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_SOURCE_DIR}/build/${_preset}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            VERBATIM)
    endforeach()
    # CPack leaves its per-package staging tree in the output directory; drop
    # it once every bundle is built so release/ holds only the deliverables.
    add_custom_command(TARGET release POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_release_dir}/_CPack_Packages
        COMMENT "Cleaning CPack staging")
    return()
elseif(NOT SNESBUILD_GOOS OR NOT SNESBUILD_GOARCH)
    message(FATAL_ERROR "Set both SNESBUILD_GOOS and SNESBUILD_GOARCH, or neither "
        "(neither = orchestrator; see the package-* presets).")
endif()

# ── Single-platform bundle ───────────────────────────────────────────────────
find_program(GO_EXECUTABLE go REQUIRED
    DOC "Go toolchain used to cross-compile the distribution binaries")

# Version stamp: explicit -DSNESBUILD_VERSION wins; otherwise `git describe`.
set(SNESBUILD_VERSION "" CACHE STRING "Version stamped into the binary")
if(NOT SNESBUILD_VERSION)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE _git_describe OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(_git_describe)
        set(SNESBUILD_VERSION "${_git_describe}")
    else()
        set(SNESBUILD_VERSION "0.0.0-dev")
    endif()
endif()

set(_os_name "${SNESBUILD_GOOS}")
if(_os_name STREQUAL "darwin")
    set(_os_name "macos")
endif()
set(_arch_name "${SNESBUILD_GOARCH}")
if(_arch_name STREQUAL "amd64")
    set(_arch_name "x86_64")
endif()
set(_exe_suffix "")
if(SNESBUILD_GOOS STREQUAL "windows")
    set(_exe_suffix ".exe")
endif()

set(SNESBUILD_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)   # snesrecomp-go
get_filename_component(GAME_ROOT ${SNESBUILD_MODULE_DIR}/.. ABSOLUTE)  # repo root
set(_cache_dir ${CMAKE_CURRENT_SOURCE_DIR}/cache)
file(MAKE_DIRECTORY ${_cache_dir})
set(_snesbuild ${CMAKE_BINARY_DIR}/bin/snesbuild${_exe_suffix})

add_custom_target(snesbuild_binary ALL
    COMMAND ${CMAKE_COMMAND} -E env
        GOOS=${SNESBUILD_GOOS} GOARCH=${SNESBUILD_GOARCH} CGO_ENABLED=0
        ${GO_EXECUTABLE} build -C ${SNESBUILD_MODULE_DIR} -trimpath
        -ldflags "-s -w -X main.version=${SNESBUILD_VERSION}"
        -o ${_snesbuild} ./cmd/snesbuild
    COMMENT "go build snesbuild for ${SNESBUILD_GOOS}/${SNESBUILD_GOARCH} (${SNESBUILD_VERSION})"
    VERBATIM)

# ── Bundled Zig toolchain (pin comes from the Go source of truth) ────────────
execute_process(
    COMMAND ${GO_EXECUTABLE} -C ${SNESBUILD_MODULE_DIR} run ./cmd/snesbuild
        toolchain pin --goos ${SNESBUILD_GOOS} --goarch ${SNESBUILD_GOARCH}
    OUTPUT_VARIABLE _zig_pin OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _pin_result)
if(NOT _pin_result EQUAL 0)
    message(FATAL_ERROR "snesbuild toolchain pin failed for ${SNESBUILD_GOOS}/${SNESBUILD_GOARCH}")
endif()
separate_arguments(_zig_pin_fields UNIX_COMMAND "${_zig_pin}")
list(GET _zig_pin_fields 0 _zig_url)
list(GET _zig_pin_fields 1 _zig_sha)
list(GET _zig_pin_fields 2 _zig_archive)
file(DOWNLOAD ${_zig_url} ${_cache_dir}/${_zig_archive}
    EXPECTED_HASH SHA256=${_zig_sha} SHOW_PROGRESS)
string(REGEX REPLACE "\\.(tar\\.xz|zip)$" "" _zig_release "${_zig_archive}")
set(_zig_stage ${CMAKE_BINARY_DIR}/deps)
if(NOT IS_DIRECTORY ${_zig_stage}/${_zig_release})
    file(MAKE_DIRECTORY ${_zig_stage})
    message(STATUS "Extracting ${_zig_archive}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf ${_cache_dir}/${_zig_archive}
        WORKING_DIRECTORY ${_zig_stage} RESULT_VARIABLE _zig_extract)
    if(NOT _zig_extract EQUAL 0)
        file(REMOVE_RECURSE ${_zig_stage}/${_zig_release})
        message(FATAL_ERROR "extracting ${_zig_archive} failed")
    endif()
endif()

# ── Bundled SDL3 (official redistributables; zlib license) ───────────────────
set(SNESBUILD_SDL3_VERSION "3.4.12" CACHE STRING "Bundled SDL3 release")
set(_sdl_base "https://github.com/libsdl-org/SDL/releases/download/release-${SNESBUILD_SDL3_VERSION}")
set(_sdl_stage ${CMAKE_BINARY_DIR}/deps/sdl3)
set(SNESBUILD_SDL3_BUNDLED FALSE)

if(SNESBUILD_GOOS STREQUAL "darwin")
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "macOS bundles must be packaged on a macOS host")
    endif()
    find_program(SEVENZIP_EXECUTABLE NAMES 7zz 7z REQUIRED
        DOC "7-Zip, used to extract the SDL3 .dmg without mounting it")
    set(_sdl_dmg SDL3-${SNESBUILD_SDL3_VERSION}.dmg)
    file(DOWNLOAD ${_sdl_base}/${_sdl_dmg} ${_cache_dir}/${_sdl_dmg}
        EXPECTED_HASH SHA256=c77d36d9393bb5481e38d222b75a1a63ab16274457b3d18c63fef90aaf5fc93b)
    if(NOT EXISTS ${_sdl_stage}/lib/libSDL3.dylib)
        file(REMOVE_RECURSE ${_sdl_stage} ${CMAKE_BINARY_DIR}/deps/dmg)
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/deps/dmg)
        execute_process(COMMAND ${SEVENZIP_EXECUTABLE} x -y
            -o${CMAKE_BINARY_DIR}/deps/dmg ${_cache_dir}/${_sdl_dmg}
            OUTPUT_QUIET ERROR_QUIET)
        # SDL3 ships the macOS binary inside an xcframework as a universal
        # (arm64 + x86_64) library, so the one dmg serves both arch presets.
        file(GLOB_RECURSE _framework_binary
            ${CMAKE_BINARY_DIR}/deps/dmg/*/SDL3.xcframework/macos-arm64_x86_64/SDL3.framework/Versions/A/SDL3)
        if(NOT _framework_binary)
            message(FATAL_ERROR "SDL3.framework not found inside ${_sdl_dmg}")
        endif()
        list(GET _framework_binary 0 _framework_binary)
        get_filename_component(_framework_dir ${_framework_binary} DIRECTORY)
        file(MAKE_DIRECTORY ${_sdl_stage}/lib)
        file(COPY ${_framework_dir}/Headers/ DESTINATION ${_sdl_stage}/include/SDL3)
        file(COPY_FILE ${_framework_binary} ${_sdl_stage}/lib/libSDL3.dylib)
        execute_process(COMMAND install_name_tool -id "@rpath/libSDL3.dylib"
            ${_sdl_stage}/lib/libSDL3.dylib COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND codesign --force --sign - --timestamp=none
            ${_sdl_stage}/lib/libSDL3.dylib COMMAND_ERROR_IS_FATAL ANY)
    endif()
    set(SNESBUILD_SDL3_BUNDLED TRUE)
elseif(SNESBUILD_GOOS STREQUAL "windows" AND SNESBUILD_GOARCH STREQUAL "amd64")
    set(_sdl_mingw SDL3-devel-${SNESBUILD_SDL3_VERSION}-mingw.tar.gz)
    file(DOWNLOAD ${_sdl_base}/${_sdl_mingw} ${_cache_dir}/${_sdl_mingw}
        EXPECTED_HASH SHA256=ea8071241b934e1feec0337f7d78807a5004de9a500dba1942aaf615a988d7a2)
    if(NOT EXISTS ${_sdl_stage}/lib/SDL3.dll)
        file(REMOVE_RECURSE ${_sdl_stage} ${CMAKE_BINARY_DIR}/deps/mingw)
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/deps/mingw)
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf ${_cache_dir}/${_sdl_mingw}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/deps/mingw COMMAND_ERROR_IS_FATAL ANY)
        set(_mingw_root ${CMAKE_BINARY_DIR}/deps/mingw/SDL3-${SNESBUILD_SDL3_VERSION}/x86_64-w64-mingw32)
        file(MAKE_DIRECTORY ${_sdl_stage}/lib)
        file(COPY ${_mingw_root}/include/SDL3/ DESTINATION ${_sdl_stage}/include/SDL3)
        # SDL3 dropped the separate SDLmain static lib; only the import lib
        # for the DLL is needed to link.
        file(COPY ${_mingw_root}/lib/libSDL3.dll.a DESTINATION ${_sdl_stage}/lib)
        file(COPY_FILE ${_mingw_root}/bin/SDL3.dll ${_sdl_stage}/lib/SDL3.dll)
    endif()
    set(SNESBUILD_SDL3_BUNDLED TRUE)
else()
    message(STATUS "No official SDL3 redistributable for ${SNESBUILD_GOOS}/${SNESBUILD_GOARCH}; "
        "the run script will fall back to a system SDL3")
endif()

# ── One-click run script per platform ────────────────────────────────────────
if(SNESBUILD_GOOS STREQUAL "darwin")
    set(_run_script scripts/run-build.command)
elseif(SNESBUILD_GOOS STREQUAL "windows")
    set(_run_script scripts/run-build.bat)
else()
    set(_run_script scripts/run-build.sh)
endif()
get_filename_component(_run_script_name ${_run_script} NAME)

set(SNESBUILD_OS_NAME "${_os_name}")
set(SNESBUILD_ARCH_NAME "${_arch_name}")
set(SNESBUILD_RUN_SCRIPT "${_run_script_name}")
configure_file(README.txt.in ${CMAKE_BINARY_DIR}/README.txt @ONLY)

# ── Install layout: minimal root, everything else under utils/ ───────────────
# The root a user sees holds only the instructions and the one build script.
# The whole buildable project, the toolchain, and SDL live under utils/ so the
# folder is not intimidating. `snesbuild` finds its bundled zig/SDL beside its
# own executable (utils/tools/), and the build script points `--root` at utils/.
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/${_run_script} DESTINATION .)
install(FILES ${CMAKE_BINARY_DIR}/README.txt DESTINATION .)

install(FILES
    ${SNESBUILD_MODULE_DIR}/LICENSE
    ${SNESBUILD_MODULE_DIR}/ATTRIBUTION.md
    ${GAME_ROOT}/snesbuild.ini
    ${GAME_ROOT}/config.ini
    DESTINATION utils)

# Buildable project source (authored input only; generated C is excluded and
# regenerated locally from the user's ROM).
install(DIRECTORY ${GAME_ROOT}/recomp/ DESTINATION utils/recomp FILES_MATCHING PATTERN "*.cfg")
install(DIRECTORY ${GAME_ROOT}/src/ DESTINATION utils/src
    PATTERN "gen" EXCLUDE)
install(DIRECTORY ${GAME_ROOT}/third_party/stb/ DESTINATION utils/third_party/stb)
install(DIRECTORY ${SNESBUILD_MODULE_DIR}/runtime/ DESTINATION utils/snesrecomp-go/runtime)

# game-assets: manifest template + empty media folders (no media ships).
install(FILES ${GAME_ROOT}/game-assets/manifest.ini DESTINATION utils/game-assets)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/templates/audio-README.txt
    DESTINATION utils/game-assets/audio RENAME README.txt)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/templates/hd-README.txt
    DESTINATION utils/game-assets/hd RENAME README.txt)

# Build machinery.
install(PROGRAMS ${_snesbuild} DESTINATION utils/tools)
install(DIRECTORY ${_zig_stage}/${_zig_release} DESTINATION utils/tools/toolchain
    USE_SOURCE_PERMISSIONS)
if(SNESBUILD_SDL3_BUNDLED)
    install(DIRECTORY ${_sdl_stage}/ DESTINATION utils/tools/sdl3 USE_SOURCE_PERMISSIONS)
endif()

# ── Post-package leak gate ───────────────────────────────────────────────────
# Reviewed top-level entries only (root holds just the script + README + the
# utils/ tree); no first-party file may contain a build-machine path.
# Third-party payloads (bundled toolchain/SDL, upstream stb) are exempt from
# the string scan.
set(SNESBUILD_ALLOWED_TOPLEVEL "${_run_script_name}" README.txt utils)
set(SNESBUILD_SCAN_EXEMPT_DIRS utils/tools/toolchain utils/tools/sdl3 utils/third_party)
set(_leak_paths "${CMAKE_CURRENT_SOURCE_DIR}" "${GAME_ROOT}")
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    list(APPEND _leak_paths "$ENV{HOME}")
endif()
if(DEFINED ENV{USERPROFILE} AND NOT "$ENV{USERPROFILE}" STREQUAL "")
    string(REPLACE "\\" "/" _profile "$ENV{USERPROFILE}")
    list(APPEND _leak_paths "${_profile}")
endif()
set(SNESBUILD_LEAK_PATTERNS "")
foreach(_leak_path IN LISTS _leak_paths)
    string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" _escaped "${_leak_path}")
    list(APPEND SNESBUILD_LEAK_PATTERNS "${_escaped}")
endforeach()
configure_file(check_package.cmake.in ${CMAKE_BINARY_DIR}/check_package.cmake @ONLY)
set(CPACK_POST_BUILD_SCRIPTS ${CMAKE_BINARY_DIR}/check_package.cmake)

# ── Archive naming + output location ─────────────────────────────────────────
set(CPACK_PACKAGE_NAME "actraiser-recomp")
set(CPACK_PACKAGE_VERSION "${SNESBUILD_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "actraiser-recomp-${_os_name}-${_arch_name}")
set(CPACK_PACKAGE_DIRECTORY "${GAME_ROOT}/release")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
set(CPACK_THREADS 0)  # multithreaded compression (0 = all cores)
if(SNESBUILD_GOOS STREQUAL "windows")
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TXZ")
endif()
include(CPack)
