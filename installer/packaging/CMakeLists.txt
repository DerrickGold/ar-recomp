# Packaging project for the self-contained ActRaiser Recomp distribution.
#
# Two modes, selected by whether a target platform is given:
#
#  * Orchestrator (no -DSNESBUILD_GOOS): defines a `release` target that
#    builds every platform bundle by invoking the per-platform presets, so
#    one command from the repo root produces all of them into release/.
#      cmake --workflow --preset release          (from installer/packaging)
#
#  * Single platform (-DSNESBUILD_GOOS/-GOARCH, set by the package-* presets):
#    cross-builds actraiser-builder and snesbuild, stages the target's pinned build dependencies
#    (Zig, SDL3 and SDL3_ttf for every supported platform), and
#    assembles a STANDALONE bundle with the authored game project and a
#    target-specific prebuilt runner SDK so a user needs only the Builder and
#    their own ROM — no repo checkout or developer tools. Go and Zig
#    cross-compile every target from any host.
#
# Only generic tools, the project's own authored source, and redistributable
# third-party components ship — never a ROM or generated C. User-provided HD
# packs and music do not ship (the game-assets/ folders are empty templates).
# The optional project-authored HD title and the manual are embedded in
# actraiser-builder; the Assets tab materializes the title only when selected, while the
# manual is materialized when the builder/launcher starts.

cmake_minimum_required(VERSION 3.25)
project(actraiser-recomp-dist NONE)

set(SNESBUILD_GOOS "" CACHE STRING "Target GOOS (darwin, linux, windows); empty = orchestrator")
set(SNESBUILD_GOARCH "" CACHE STRING "Target GOARCH (arm64, amd64); empty = orchestrator")
set(SNESBUILD_SDL3_VERSION "3" CACHE STRING "SDL3: latest stable major 3, or exact 3.x.y")
set(SNESBUILD_SDL3_TTF_VERSION "3" CACHE STRING "SDL3_ttf: latest stable major 3, or exact 3.x.y")
set(SNESBUILD_SDL_LOCKFILE "" CACHE FILEPATH "Reuse a target-specific SDL SDK lock (no metadata lookup)")
option(SNESBUILD_STEAM_DECK
    "Build the standalone x86_64 Steam Deck bundle with Valve's SDL3 runtime"
    OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/release_policy.cmake")
set(SNESBUILD_ALL_PRESETS "${BUILDER_ALL_PLATFORMS}")

# ── Orchestrator mode ────────────────────────────────────────────────────────
if(NOT SNESBUILD_GOOS AND NOT SNESBUILD_GOARCH)
    option(SNESBUILD_DESKTOP "Desktop release matrix; OFF explicitly produces legacy archives" ON)
    option(SNESBUILD_KEEP_BUILD "Retain per-platform release build trees" OFF)
    if(SNESBUILD_SDL_LOCKFILE)
        message(FATAL_ERROR "SDL lock files are target-specific; use one with a single-platform preset")
    endif()
    set(_legacy ON)
    if(SNESBUILD_DESKTOP)
        set(_legacy OFF)
    endif()
    add_custom_target(release
        COMMAND "${CMAKE_COMMAND}" "-DBUILDER_PLATFORMS=${SNESBUILD_ALL_PRESETS}"
            "-DBUILDER_LEGACY_ARCHIVES=${_legacy}" "-DBUILDER_KEEP_BUILD=${SNESBUILD_KEEP_BUILD}"
            "-DSNESBUILD_SDL3_VERSION=${SNESBUILD_SDL3_VERSION}"
            "-DSNESBUILD_SDL3_TTF_VERSION=${SNESBUILD_SDL3_TTF_VERSION}"
            -DSNESBUILD_SDL_LOCKFILE=
            -P "${CMAKE_CURRENT_SOURCE_DIR}/release.cmake"
        COMMENT "Building desktop Builders, portable companions, and generic Linux archives into release/"
        VERBATIM)
    return()
elseif(NOT SNESBUILD_GOOS OR NOT SNESBUILD_GOARCH)
    message(FATAL_ERROR "Set both SNESBUILD_GOOS and SNESBUILD_GOARCH, or neither "
        "(neither = orchestrator; see the package-* presets).")
endif()
if(SNESBUILD_STEAM_DECK AND
   (NOT SNESBUILD_GOOS STREQUAL "linux" OR
    NOT SNESBUILD_GOARCH STREQUAL "amd64"))
    message(FATAL_ERROR
        "The Steam Deck package is always linux/amd64 (x86_64)")
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
set(_archive_platform_name "${_os_name}")
if(SNESBUILD_STEAM_DECK)
    set(_os_name "Steam Deck")
    set(_archive_platform_name "steam-deck")
endif()
set(_exe_suffix "")
if(SNESBUILD_GOOS STREQUAL "windows")
    set(_exe_suffix ".exe")
endif()

set(INSTALLER_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(GAME_ROOT ${INSTALLER_MODULE_DIR}/.. ABSOLUTE)
set(SNESBUILD_MODULE_DIR ${GAME_ROOT}/snesrecomp-go)
set(_cache_dir ${CMAKE_CURRENT_SOURCE_DIR}/cache)
file(MAKE_DIRECTORY ${_cache_dir})
set(_go_cache ${CMAKE_BINARY_DIR}/go-cache)
set(_snesbuild ${CMAKE_BINARY_DIR}/bin/snesbuild${_exe_suffix})
set(_actraiser_builder ${CMAKE_BINARY_DIR}/bin/actraiser-builder${_exe_suffix})

add_custom_target(snesbuild_binary ALL
    COMMAND ${CMAKE_COMMAND} -E env
        GOOS=${SNESBUILD_GOOS} GOARCH=${SNESBUILD_GOARCH} CGO_ENABLED=0
        GOCACHE=${_go_cache}
        ${GO_EXECUTABLE} build -C ${SNESBUILD_MODULE_DIR} -trimpath
        -ldflags "-s -w -X main.version=${SNESBUILD_VERSION}"
        -o ${_snesbuild} ./cmd/snesbuild
    COMMENT "go build snesbuild for ${SNESBUILD_GOOS}/${SNESBUILD_GOARCH} (${SNESBUILD_VERSION})"
    VERBATIM)

add_custom_target(actraiser_builder_binary ALL
    COMMAND ${CMAKE_COMMAND} -E env
        GOOS=${SNESBUILD_GOOS} GOARCH=${SNESBUILD_GOARCH} CGO_ENABLED=0
        GOCACHE=${_go_cache}
        ${GO_EXECUTABLE} build -C ${INSTALLER_MODULE_DIR} -trimpath
        -ldflags "-s -w -X main.version=${SNESBUILD_VERSION}"
        -o ${_actraiser_builder} ./cmd/actraiser-builder
    COMMENT "go build actraiser-builder for ${SNESBUILD_GOOS}/${SNESBUILD_GOARCH} (${SNESBUILD_VERSION})"
    VERBATIM)

# ── Target-specific runner SDK archive ──────────────────────────────────────
# The target Zig bundled below cannot necessarily run on the packaging host
# (for example a Windows release assembled on macOS). Build the archive with a
# runnable host Zig and an explicit cross target, then ship only that archive
# and the public headers. The runtime command locates a host Zig or fetches the
# pinned one into a shared packaging cache; it never relies on the target Zig.
if(SNESBUILD_GOARCH STREQUAL "amd64")
    set(_runtime_zig_arch x86_64)
elseif(SNESBUILD_GOARCH STREQUAL "arm64")
    set(_runtime_zig_arch aarch64)
else()
    message(FATAL_ERROR "No runner archive target for ${SNESBUILD_GOARCH}")
endif()
if(SNESBUILD_GOOS STREQUAL "darwin")
    set(_runtime_zig_os macos)
elseif(SNESBUILD_GOOS STREQUAL "linux")
    set(_runtime_zig_os linux-gnu)
elseif(SNESBUILD_GOOS STREQUAL "windows")
    set(_runtime_zig_os windows-gnu)
else()
    message(FATAL_ERROR "No runner archive target for ${SNESBUILD_GOOS}")
endif()
set(_runtime_zig_target "${_runtime_zig_arch}-${_runtime_zig_os}")
if(SNESBUILD_GOOS STREQUAL "windows")
    set(_runtime_archive_name snesrecomp_runtime.lib)
else()
    set(_runtime_archive_name libsnesrecomp_runtime.a)
endif()
set(_runtime_sdk_stage ${CMAKE_BINARY_DIR}/runtime-sdk)
set(_runtime_archive
    ${_runtime_sdk_stage}/lib/${_runtime_zig_target}/${_runtime_archive_name})
file(GLOB_RECURSE _runtime_archive_inputs CONFIGURE_DEPENDS
    ${SNESBUILD_MODULE_DIR}/runtime/include/*.h
    ${SNESBUILD_MODULE_DIR}/runtime/src/*.c
    ${SNESBUILD_MODULE_DIR}/runtime/src/*.cpp
    ${SNESBUILD_MODULE_DIR}/runtime/src/*.h
    ${SNESBUILD_MODULE_DIR}/runtime/src/*.inc
    ${SNESBUILD_MODULE_DIR}/cmd/snesbuild/*.go
    ${SNESBUILD_MODULE_DIR}/internal/project/*.go
    ${SNESBUILD_MODULE_DIR}/internal/toolchain/*.go)
list(APPEND _runtime_archive_inputs
    ${SNESBUILD_MODULE_DIR}/runtime/runner.cmake
    ${SNESBUILD_MODULE_DIR}/runtime/sources.cmake)
add_custom_command(
    OUTPUT ${_runtime_archive}
    COMMAND ${CMAKE_COMMAND} -E env GOCACHE=${_go_cache}
        ${GO_EXECUTABLE} -C ${SNESBUILD_MODULE_DIR} run ./cmd/snesbuild
        runtime archive
        --runtime-dir ${SNESBUILD_MODULE_DIR}/runtime
        --output ${_runtime_archive}
        --target ${_runtime_zig_target}
        --cache-dir ${CMAKE_CURRENT_SOURCE_DIR}/build/host-toolchain
        --optimize=-O2
    DEPENDS ${_runtime_archive_inputs}
    COMMENT "Cross-building runner SDK archive for ${_runtime_zig_target} with packaging-host Zig"
    VERBATIM)
add_custom_target(runtime_archive ALL DEPENDS ${_runtime_archive})

# ── Bundled Zig toolchain (pin comes from the Go source of truth) ────────────
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GOCACHE=${_go_cache}
        ${GO_EXECUTABLE} -C ${SNESBUILD_MODULE_DIR} run ./cmd/snesbuild
        toolchain pin --goos ${SNESBUILD_GOOS} --goarch ${SNESBUILD_GOARCH}
    OUTPUT_VARIABLE _zig_pin OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _pin_result)
if(NOT _pin_result EQUAL 0)
    message(FATAL_ERROR "snesbuild toolchain pin failed for ${SNESBUILD_GOOS}/${SNESBUILD_GOARCH}")
endif()
separate_arguments(_zig_pin_fields UNIX_COMMAND "${_zig_pin}")
list(GET _zig_pin_fields 0 _zig_url)
list(GET _zig_pin_fields 1 _zig_sha)
list(GET _zig_pin_fields 2 _zig_archive)
message(STATUS
    "Bundle target compiler for ${SNESBUILD_GOOS}/${SNESBUILD_GOARCH}: ${_zig_archive}")
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

# ── Bundled SDL SDKs ──────────────────────────────────────────────────────
include(${CMAKE_CURRENT_SOURCE_DIR}/sdl-resolve.cmake)

if(SNESBUILD_GOOS STREQUAL "darwin")
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "macOS bundles must be packaged on a macOS host")
    endif()
    set(_sdl_dmg ${_sdl_archive})
    file(DOWNLOAD ${_sdl_url} ${_cache_dir}/${_sdl_dmg}
        EXPECTED_HASH SHA256=${_sdl_src_sha})
    if(NOT EXISTS ${_sdl_stage}/lib/libSDL3.dylib OR
       NOT EXISTS ${_sdl_stage}/include/SDL3/SDL.h OR
       NOT EXISTS ${_sdl_stage}/licenses/SDL3/LICENSE.txt)
        file(REMOVE_RECURSE ${_sdl_stage} ${CMAKE_BINARY_DIR}/deps/dmg)
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/deps/dmg)
        # Keep the attach/copy/detach lifecycle in one process so its EXIT and
        # signal traps can detach after a copy failure or interrupted configure.
        # The helper also recovers a matching stale attachment left by either
        # macOS architecture, which share this universal SDL3 disk image.
        execute_process(COMMAND /bin/sh
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract-macos-dmg.sh
            ${_cache_dir}/${_sdl_dmg}
            ${CMAKE_BINARY_DIR}/deps/dmg
            ${CMAKE_BINARY_DIR}/deps
            COMMAND_ERROR_IS_FATAL ANY)
        # SDL3 ships the macOS binary inside an xcframework as a universal
        # (arm64 + x86_64) library, so the one dmg serves both arch presets.
        file(GLOB_RECURSE _framework_binary
            ${CMAKE_BINARY_DIR}/deps/dmg/SDL3.xcframework/*/SDL3.framework/Versions/A/SDL3)
        if(NOT _framework_binary)
            message(FATAL_ERROR "SDL3.framework not found inside ${_sdl_dmg}")
        endif()
        list(GET _framework_binary 0 _framework_binary)
        get_filename_component(_framework_dir ${_framework_binary} DIRECTORY)
        file(MAKE_DIRECTORY ${_sdl_stage}/lib)
        file(COPY ${_framework_dir}/Headers/ DESTINATION ${_sdl_stage}/include/SDL3)
        file(COPY_FILE ${_framework_binary} ${_sdl_stage}/lib/libSDL3.dylib)
        file(COPY ${_framework_dir}/Resources/LICENSE.txt DESTINATION ${_sdl_stage}/licenses/SDL3)
        execute_process(COMMAND install_name_tool -id "@rpath/libSDL3.dylib"
            ${_sdl_stage}/lib/libSDL3.dylib COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND codesign --force --sign - --timestamp=none
            ${_sdl_stage}/lib/libSDL3.dylib COMMAND_ERROR_IS_FATAL ANY)
    endif()
    set(SNESBUILD_SDL3_BUNDLED TRUE)
elseif(SNESBUILD_GOOS STREQUAL "linux")
    include(${CMAKE_CURRENT_SOURCE_DIR}/sdl-linux.cmake)
    # Generic x86_64 and Steam Deck share Valve's Sniper SDK.
    file(REMOVE_RECURSE "${_sdl_stage}")
    stage_linux_sdl(SDL3)
    stage_linux_sdl(SDL3_ttf)
    set(SNESBUILD_SDL3_BUNDLED TRUE)
elseif(SNESBUILD_GOOS STREQUAL "windows" AND SNESBUILD_GOARCH STREQUAL "arm64")
    if(NOT _sdl_kind STREQUAL "vc")
        message(FATAL_ERROR "Expected the VC SDL3 archive for Windows ARM64, got ${_sdl_kind}")
    endif()
    file(DOWNLOAD ${_sdl_url} ${_cache_dir}/${_sdl_archive}
        EXPECTED_HASH SHA256=${_sdl_src_sha})
    if(NOT EXISTS ${_sdl_stage}/lib/SDL3.dll OR
       NOT EXISTS ${_sdl_stage}/lib/SDL3.lib OR
       NOT EXISTS ${_sdl_stage}/include/SDL3/SDL.h OR
       NOT EXISTS ${_sdl_stage}/licenses/SDL3/LICENSE.txt)
        file(REMOVE_RECURSE ${_sdl_stage} ${CMAKE_BINARY_DIR}/deps/vc)
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/deps/vc)
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf ${_cache_dir}/${_sdl_archive}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/deps/vc COMMAND_ERROR_IS_FATAL ANY)
        set(_vc_root ${CMAKE_BINARY_DIR}/deps/vc/SDL3-${_sdl_version})
        file(MAKE_DIRECTORY ${_sdl_stage}/lib)
        file(COPY ${_vc_root}/include/SDL3/ DESTINATION ${_sdl_stage}/include/SDL3)
        file(COPY_FILE ${_vc_root}/lib/arm64/SDL3.lib ${_sdl_stage}/lib/SDL3.lib)
        file(COPY_FILE ${_vc_root}/lib/arm64/SDL3.dll ${_sdl_stage}/lib/SDL3.dll)
        file(COPY ${_vc_root}/LICENSE.txt DESTINATION ${_sdl_stage}/licenses/SDL3)
    endif()
    set(SNESBUILD_SDL3_BUNDLED TRUE)
elseif(SNESBUILD_GOOS STREQUAL "windows" AND SNESBUILD_GOARCH STREQUAL "amd64")
    if(NOT _sdl_kind STREQUAL "mingw")
        message(FATAL_ERROR "Expected the MinGW SDL3 archive for Windows x86_64, got ${_sdl_kind}")
    endif()
    file(DOWNLOAD ${_sdl_url} ${_cache_dir}/${_sdl_archive}
        EXPECTED_HASH SHA256=${_sdl_src_sha})
    if(NOT EXISTS ${_sdl_stage}/lib/SDL3.dll OR
       NOT EXISTS ${_sdl_stage}/lib/libSDL3.dll.a OR
       NOT EXISTS ${_sdl_stage}/include/SDL3/SDL.h OR
       NOT EXISTS ${_sdl_stage}/licenses/SDL3/LICENSE.txt)
        file(REMOVE_RECURSE ${_sdl_stage} ${CMAKE_BINARY_DIR}/deps/mingw)
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/deps/mingw)
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf ${_cache_dir}/${_sdl_archive}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/deps/mingw COMMAND_ERROR_IS_FATAL ANY)
        set(_mingw_root ${CMAKE_BINARY_DIR}/deps/mingw/SDL3-${_sdl_version}/x86_64-w64-mingw32)
        file(MAKE_DIRECTORY ${_sdl_stage}/lib)
        file(COPY ${_mingw_root}/include/SDL3/ DESTINATION ${_sdl_stage}/include/SDL3)
        # SDL3 dropped the separate SDLmain static lib; only the import lib
        # for the DLL is needed to link.
        file(COPY ${_mingw_root}/lib/libSDL3.dll.a DESTINATION ${_sdl_stage}/lib)
        file(COPY_FILE ${_mingw_root}/bin/SDL3.dll ${_sdl_stage}/lib/SDL3.dll)
        file(COPY ${CMAKE_BINARY_DIR}/deps/mingw/SDL3-${_sdl_version}/LICENSE.txt
            DESTINATION ${_sdl_stage}/licenses/SDL3)
    endif()
    set(SNESBUILD_SDL3_BUNDLED TRUE)
else()
    message(FATAL_ERROR "No bundled SDL SDK for ${SNESBUILD_GOOS}/${SNESBUILD_GOARCH}")
endif()

# SDL3_ttf shares the SDK root and the resolved lock.
include(${CMAKE_CURRENT_SOURCE_DIR}/sdl-ttf.cmake)
file(WRITE "${_sdl_stage}/sdk.pin" "${_sdl_expected}")
configure_file("${CMAKE_BINARY_DIR}/sdl-sdk.lock.json" "${_sdl_stage}/sdl-sdk.lock.json" COPYONLY)
install(FILES "${CMAKE_BINARY_DIR}/sdl-sdk.lock.json" DESTINATION utils/licenses)
include(${CMAKE_CURRENT_SOURCE_DIR}/appimage.cmake)

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
set(SNESBUILD_SDL_NOTE "- SDL3 ${_sdl_version} and SDL3_ttf ${_ttf_version} headers and libraries are included.")
if(SNESBUILD_STEAM_DECK)
    string(APPEND SNESBUILD_SDL_NOTE " The Steam Deck build uses SteamOS runtime dependencies.")
elseif(SNESBUILD_GOOS STREQUAL "linux")
    string(JSON _sdl_dependencies GET "${_sdl_lock}" sdl dependencies)
    string(JSON _ttf_dependencies GET "${_sdl_lock}" ttf dependencies)
    string(APPEND SNESBUILD_SDL_NOTE
" Do not install
  SDL development packages. This build needs the Linux desktop/audio/font
  runtime libraries below. The Builder reports missing libraries
  before compiling. On a minimal Debian 13 installation these can be added with:
    sudo apt install libasound2t64 libpulse0 libdecor-0-0 libfribidi0 libx11-6 libxext6 libxcursor1 libxi6 libxfixes3 libxrandr2 libxss1 libxtst6 libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 libfreetype6 libharfbuzz0b
  These are OS runtime components, not compilers or SDL SDKs. A graphical
  session and working graphics drivers are also needed to play normally.
  Selected SDL3 package requirements: ${_sdl_dependencies}
  Selected SDL3_ttf package requirements: ${_ttf_dependencies}")
endif()
string(APPEND SNESBUILD_SDL_NOTE
"\n  Exact dependency versions, URLs and checksums: utils/licenses/sdl-sdk.lock.json")
set(SNESBUILD_APPIMAGE_NOTE "")
if(SNESBUILD_GOOS STREQUAL "linux")
    set(SNESBUILD_APPIMAGE_NOTE
"- Normal AppImage launching needs working FUSE (for example, Debian's fuse3).
  APPIMAGE_EXTRACT_AND_RUN=1 provides a no-FUSE launch fallback.")
endif()
configure_file(README.txt.in ${CMAKE_BINARY_DIR}/README.txt @ONLY)

# ── Install layout: minimal root, everything else under utils/ ───────────────
# The root a user sees holds only the instructions and the one build script.
# The whole buildable project, the toolchain, and SDL live under utils/ so the
# folder is not intimidating. `snesbuild` finds its bundled zig/SDL beside its
# own executable (utils/tools/), and the build script points `--root` at utils/.
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/${_run_script} DESTINATION .)
install(FILES ${CMAKE_BINARY_DIR}/README.txt DESTINATION .)

# utils/ is the game's WORKING DIRECTORY at runtime, not just a source drop:
# the generated launcher does `cd "$ROOT/utils"` before exec'ing the binary from
# the bundle root (internal/project/install.go), and the binary is not detected
# as a bundle (PortablePaths_IsBundle looks for game-assets/ beside the
# EXECUTABLE, which ships under utils/), so nothing chdirs away again. Every
# cwd-relative runtime file therefore belongs here -- config.ini is passed as a
# bare `--config config.ini`, and diorama-layers.ini is resolved by UserDataFile,
# which returns the bare leaf.
install(FILES
    ${SNESBUILD_MODULE_DIR}/LICENSE
    ${SNESBUILD_MODULE_DIR}/LICENSE_SCOPE.md
    ${SNESBUILD_MODULE_DIR}/ATTRIBUTION.md
    ${SNESBUILD_MODULE_DIR}/THIRD_PARTY_NOTICES.md
    ${GAME_ROOT}/snesbuild.ini
    DESTINATION utils)
install(FILES ${INSTALLER_MODULE_DIR}/THIRD_PARTY_NOTICES.md
    DESTINATION utils RENAME ACTRAISER-THIRD-PARTY-NOTICES.md)

# USER-EDITABLE FILES SHIP AS DEFAULTS, NOT AS THE LIVE FILES.
#
# These three used to install straight to utils/, which made upgrading destructive:
# extracting a new bundle over an existing install overwrote whatever the archive
# carried, so a user lost their display settings (config.ini), every authored
# diorama room (diorama-layers.ini), and their asset mappings
# (game-assets/manifest.ini). Measured, not assumed -- a played install lost all
# three. settings.ini and saves/ survived only because nothing ships them.
#
# Shipping to utils/defaults/ instead means extraction can only ever overwrite the
# DEFAULTS. The game merges them forward at startup
# (IniUpgrade_ApplyShippedDefaults): a value the user changed is kept, a key that
# is new in this version is added. The live files are created from these on first
# run and are never in the archive, so the clobber is structurally impossible
# rather than merely warned about.
#
# The paths matter: utils/ is the game's working directory (the launcher cd's into
# it), so the LIVE files must resolve there -- config.ini is passed as a bare
# `--config config.ini` and diorama-layers.ini via UserDataFile's bare leaf.
# config.ini ships from the TEMPLATE for the same reason the manifest does: the
# file at ${GAME_ROOT}/config.ini is the developer's own live config, and
# whatever was switched on in it at release time went out to every install --
# which is how AR_MOONJUMP, AR_NO_KNOCKBACK and AR_RANGED_SWORD came to be
# enabled in a shipped default. `make check-constants` holds the template stock.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/templates/config.ini
    DESTINATION utils/defaults)
# diorama-layers.ini is authored CONTENT, not personal settings: its per-room
# overrides are the project's work and are meant to ship as they are.
install(FILES
    ${GAME_ROOT}/diorama-layers.ini
    DESTINATION utils/defaults)
# The manifest ships from the TEMPLATE, not from the working copy at
# ${GAME_ROOT}/game-assets/manifest.ini. That file is live and user-edited --
# the builder writes to it, players hand-edit it, and a developer's own
# experiments accumulate in it -- so vending it shipped whatever happened to be
# checked out at release time. The template lives beside the builder's other
# embedded assets because `go:embed` cannot reach outside its own package, and
# the same bytes seed a fresh install from inside actraiser-builder.
install(FILES ${INSTALLER_MODULE_DIR}/internal/builder/assets/manifest.ini
    DESTINATION utils/defaults/game-assets)

# Buildable project source (authored input only; generated C is excluded and
# regenerated locally from the user's ROM).
install(DIRECTORY ${GAME_ROOT}/recomp/ DESTINATION utils/recomp FILES_MATCHING PATTERN "*.cfg")
install(DIRECTORY ${GAME_ROOT}/src/ DESTINATION utils/src
    PATTERN "gen" EXCLUDE
    PATTERN ".DS_Store" EXCLUDE)
install(DIRECTORY ${GAME_ROOT}/third_party/stb/ DESTINATION utils/third_party/stb
    PATTERN ".DS_Store" EXCLUDE)
install(DIRECTORY ${GAME_ROOT}/third_party/sheenbidi/ DESTINATION utils/third_party/sheenbidi
    PATTERN ".DS_Store" EXCLUDE)
install(FILES ${GAME_ROOT}/third_party/sheenbidi/LICENSE
    DESTINATION utils/licenses/SheenBidi)
install(FILES
    ${GAME_ROOT}/third_party/unicode/NOTICE
    ${GAME_ROOT}/third_party/unicode/utf8proc-LICENSE.md
    DESTINATION utils/third_party/unicode)

# Curated game documentation. This is an allowlist by design: public progress,
# player guidance, and durable reverse-engineering knowledge ship with the
# buildable source, while research ledgers, implementation plans, debugging
# journals, and retired specifications remain outside the bundle.
install(FILES ${GAME_ROOT}/LICENSE
    DESTINATION utils/docs RENAME GAME-LICENSE.txt)
install(FILES
    ${GAME_ROOT}/docs/README.md
    ${GAME_ROOT}/docs/manual.md
    ${GAME_ROOT}/docs/builder-workshop.md
    ${GAME_ROOT}/docs/desktop-packaging.md
    ${GAME_ROOT}/docs/language-pack-format.md
    ${GAME_ROOT}/docs/language-packs.md
    ${GAME_ROOT}/docs/language-authoring-reference.json
    ${GAME_ROOT}/docs/language-archive.schema.json
    ${GAME_ROOT}/docs/progress.md
    ${GAME_ROOT}/docs/SEAMS.md
    ${GAME_ROOT}/docs/ram-map.md
    ${GAME_ROOT}/docs/rom-map.md
    ${GAME_ROOT}/docs/research-symbol-map.md
    ${GAME_ROOT}/docs/rendering-engine.md
    ${GAME_ROOT}/docs/save-format.md
    ${GAME_ROOT}/docs/sim-object-catalog.md
    ${GAME_ROOT}/docs/sim-town-terrain.md
    ${GAME_ROOT}/docs/diorama-depth-shapes.md
    DESTINATION utils/docs)
# Independently authored syntax/demo artifacts; no retail prose.
install(DIRECTORY ${GAME_ROOT}/examples/language-pack/ DESTINATION utils/examples/language-pack
    PATTERN ".DS_Store" EXCLUDE)
install(FILES ${GAME_ROOT}/examples/example.fr-ca.arlang DESTINATION utils/examples)
# Vended runner SDK: generated/game sources compile against public headers and
# link this target-matched archive. Private headers, implementation sources,
# tests, examples, and source-build manifests intentionally do not ship.
install(DIRECTORY ${SNESBUILD_MODULE_DIR}/runtime/include/
    DESTINATION utils/snesrecomp-go/runtime/include
    PATTERN ".DS_Store" EXCLUDE)
install(FILES
    ${SNESBUILD_MODULE_DIR}/runtime/LICENSE
    ${SNESBUILD_MODULE_DIR}/runtime/NOTICE.md
    ${SNESBUILD_MODULE_DIR}/runtime/PROVENANCE.md
    DESTINATION utils/snesrecomp-go/runtime)
install(FILES ${SNESBUILD_MODULE_DIR}/runtime/licenses/Snaggletooth-LICENSE.txt
    DESTINATION utils/snesrecomp-go/runtime/licenses
)
install(FILES ${SNESBUILD_MODULE_DIR}/runtime/BINARY_SDK.md
    DESTINATION utils/snesrecomp-go/runtime RENAME README.md)
install(FILES
    ${SNESBUILD_MODULE_DIR}/runtime/docs/README.md
    ${SNESBUILD_MODULE_DIR}/runtime/docs/API_REFERENCE.md
    ${SNESBUILD_MODULE_DIR}/runtime/docs/GAME_ENHANCEMENT_INTEGRATION.md
    ${SNESBUILD_MODULE_DIR}/runtime/docs/RUNTIME.md
    DESTINATION utils/snesrecomp-go/runtime/docs)
install(FILES ${_runtime_archive}
    DESTINATION utils/snesrecomp-go/runtime/lib/${_runtime_zig_target})

# Licensed interface/game fonts are redistributed with their notices. Retail
# prose/source projects and ROM-owned graphics remain local.
install(FILES
    ${GAME_ROOT}/game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf
    ${GAME_ROOT}/game-assets/fonts/noto/OFL.txt
    ${GAME_ROOT}/game-assets/fonts/noto/NotoSansJP-Bold.otf
    ${GAME_ROOT}/game-assets/fonts/noto/NotoSansJP-OFL.txt
    ${GAME_ROOT}/game-assets/fonts/noto/NotoSansArabic-Bold.ttf
    ${GAME_ROOT}/game-assets/fonts/noto/NotoSansHebrew-Bold.ttf
    ${GAME_ROOT}/game-assets/fonts/noto/README.md
    DESTINATION utils/game-assets/fonts/noto)

# game-assets: initially empty HD/music folders. The bundled manual and optional
# HD title are already embedded in actraiser-builder; the manual is materialized when the
# builder starts, while the title is installed only from the Assets-tab toggle.
# manifest.ini itself is user-editable and ships as a template under
# utils/defaults/game-assets/ above, so an upgrade cannot overwrite the entries
# a user added; actraiser-builder materializes the live copy when there is none.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/templates/audio-README.txt
    DESTINATION utils/game-assets/audio RENAME README.txt)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/templates/hd-README.txt
    DESTINATION utils/game-assets/hd RENAME README.txt)

# Build machinery.
install(PROGRAMS ${_snesbuild} DESTINATION utils/tools)
install(PROGRAMS ${_actraiser_builder} DESTINATION utils/tools)
install(DIRECTORY ${_zig_stage}/${_zig_release} DESTINATION utils/tools/toolchain
    USE_SOURCE_PERMISSIONS)
if(SNESBUILD_SDL3_BUNDLED)
    install(DIRECTORY ${_sdl_stage}/ DESTINATION utils/tools/sdl3 USE_SOURCE_PERMISSIONS)
    if(EXISTS "${_sdl_stage}/licenses/SDL3")
        install(DIRECTORY ${_sdl_stage}/licenses/SDL3/ DESTINATION utils/licenses/SDL3)
    endif()
    # Keep notices outside the optional compiler SDK too: slimming removes
    # tools/sdl3, but the game's copied shared libraries remain in use.
    install(DIRECTORY ${_sdl_stage}/licenses/SDL3_ttf/
        DESTINATION utils/licenses/SDL3_ttf)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/licenses/sdl-ttf/
        DESTINATION utils/licenses/SDL3_ttf)
endif()

# ── Post-package leak gate ───────────────────────────────────────────────────
# Reviewed top-level entries only (root holds just the script + README + the
# utils/ tree); no first-party file may contain a build-machine path.
# Third-party payloads (bundled toolchain/SDL, upstream stb) are exempt from
# the string scan.
set(SNESBUILD_ALLOWED_TOPLEVEL "${_run_script_name}" README.txt utils)
set(SNESBUILD_SCAN_EXEMPT_DIRS utils/tools/toolchain utils/tools/sdl3 utils/third_party)
set(SNESBUILD_RUNTIME_ARCHIVE_RELATIVE
    "utils/snesrecomp-go/runtime/lib/${_runtime_zig_target}/${_runtime_archive_name}")
set(SNESBUILD_RUNTIME_SOURCE_PATHS
    utils/snesrecomp-go/runtime/src
    utils/snesrecomp-go/runtime/runner.cmake
    utils/snesrecomp-go/runtime/sources.cmake
    utils/snesrecomp-go/runtime/CMakeLists.txt
    utils/snesrecomp-go/runtime/tests
    utils/snesrecomp-go/runtime/examples)
set(SNESBUILD_REQUIRED_SDL_RUNTIME "")
if(SNESBUILD_SDL3_BUNDLED)
    if(SNESBUILD_GOOS STREQUAL "windows")
        set(SNESBUILD_REQUIRED_SDL_RUNTIME utils/tools/sdl3/lib/SDL3.dll)
    elseif(SNESBUILD_GOOS STREQUAL "darwin")
        set(SNESBUILD_REQUIRED_SDL_RUNTIME utils/tools/sdl3/lib/libSDL3.dylib)
    elseif(SNESBUILD_STEAM_DECK)
        set(SNESBUILD_REQUIRED_SDL_RUNTIME utils/tools/sdl3/lib/libSDL3.so)
    endif()
endif()
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
set(CPACK_PACKAGE_FILE_NAME
    "actraiser-recomp-${_archive_platform_name}-${_arch_name}")
set(CPACK_PACKAGE_DIRECTORY "${GAME_ROOT}/release")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
set(CPACK_THREADS 0)  # multithreaded compression (0 = all cores)
if(SNESBUILD_GOOS STREQUAL "windows")
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TXZ")
endif()
include(CPack)
