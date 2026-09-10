# Included after SDL3 staging, which may replace the entire shared SDK root.
# All versions and checksums come from the resolved SDK lock. No retail assets
# or authoring tools are part of this dependency payload.
if(NOT SNESBUILD_SDL3_BUNDLED)
    message(FATAL_ERROR "Every installer must include SDL3 and SDL3_ttf")
endif()
if(SNESBUILD_GOOS STREQUAL "linux")
    # Staged together by sdl-linux.cmake, with matching dev/runtime packages.
    set(SNESBUILD_REQUIRED_TTF_RUNTIME "utils/tools/sdl3/lib/libSDL3_ttf.so.0")
    set(SNESBUILD_REQUIRED_TTF_LINK "utils/tools/sdl3/lib/libSDL3_ttf.so")
    return()
endif()

string(JSON _ttf_pin GET "${_sdl_lock}" ttf)
string(JSON _ttf_kind GET "${_ttf_pin}" kind)
string(JSON _ttf_url GET "${_ttf_pin}" archives 0 url)
string(JSON _ttf_sha GET "${_ttf_pin}" archives 0 sha256)
string(JSON _ttf_archive GET "${_ttf_pin}" archives 0 archive)

file(DOWNLOAD "${_ttf_url}" "${_cache_dir}/${_ttf_archive}"
    EXPECTED_HASH SHA256=${_ttf_sha})
get_filename_component(_ttf_parent "${_sdl_stage}" DIRECTORY)
set(_ttf_unpack "${_ttf_parent}/ttf-unpacked")
# A checksum stamp invalidates previous staging when the pin changes. Keeping
# it in the SDL stage also invalidates it when SDL3 itself is restaged.
set(_ttf_expected "format=1\n${_ttf_pin}\n")
set(_ttf_stamp "${_sdl_stage}/ttf.pin")
set(_ttf_installed "")
if(EXISTS "${_ttf_stamp}")
    file(READ "${_ttf_stamp}" _ttf_installed)
endif()
if(SNESBUILD_GOOS STREQUAL "darwin")
    set(_ttf_runtime libSDL3_ttf.dylib)
    set(_ttf_link libSDL3_ttf.dylib)
elseif(SNESBUILD_GOOS STREQUAL "windows")
    set(_ttf_runtime SDL3_ttf.dll)
    if(_ttf_kind STREQUAL "vc")
        set(_ttf_link SDL3_ttf.lib)
    else()
        set(_ttf_link libSDL3_ttf.dll.a)
    endif()
else()
    set(_ttf_runtime libSDL3_ttf.so.0)
    set(_ttf_link libSDL3_ttf.so)
endif()
set(SNESBUILD_REQUIRED_TTF_RUNTIME "utils/tools/sdl3/lib/${_ttf_runtime}")
set(SNESBUILD_REQUIRED_TTF_LINK "utils/tools/sdl3/lib/${_ttf_link}")

if(_ttf_installed STREQUAL _ttf_expected AND
   EXISTS "${_sdl_stage}/include/SDL3_ttf/SDL_ttf.h" AND
   EXISTS "${_sdl_stage}/lib/${_ttf_runtime}" AND
   EXISTS "${_sdl_stage}/lib/${_ttf_link}" AND
   EXISTS "${_sdl_stage}/licenses/SDL3_ttf/LICENSE.txt")
    return()
endif()

file(REMOVE_RECURSE "${_ttf_unpack}")
file(REMOVE_RECURSE "${_sdl_stage}/include/SDL3_ttf")
file(MAKE_DIRECTORY "${_ttf_unpack}" "${_sdl_stage}/lib"
    "${_sdl_stage}/licenses/SDL3_ttf")
if(_ttf_kind STREQUAL "dmg")
    execute_process(COMMAND /bin/sh
        ${CMAKE_CURRENT_LIST_DIR}/scripts/extract-macos-dmg.sh
        "${_cache_dir}/${_ttf_archive}" "${_ttf_unpack}" "${_ttf_parent}"
        COMMAND_ERROR_IS_FATAL ANY)
    file(GLOB_RECURSE _ttf_binaries
        "${_ttf_unpack}/SDL3_ttf.xcframework/*/SDL3_ttf.framework/Versions/A/SDL3_ttf")
    list(LENGTH _ttf_binaries _ttf_count)
    if(NOT _ttf_count EQUAL 1)
        message(FATAL_ERROR "Expected one universal macOS SDL3_ttf framework")
    endif()
    list(GET _ttf_binaries 0 _ttf_binary)
    get_filename_component(_ttf_root "${_ttf_binary}" DIRECTORY)
    file(COPY "${_ttf_root}/Headers/" DESTINATION "${_sdl_stage}/include/SDL3_ttf")
    file(COPY_FILE "${_ttf_binary}" "${_sdl_stage}/lib/${_ttf_runtime}")
    file(COPY "${_ttf_root}/Resources/LICENSE.txt"
        DESTINATION "${_sdl_stage}/licenses/SDL3_ttf")
    execute_process(COMMAND install_name_tool
        -id "@rpath/libSDL3_ttf.dylib"
        -change "@rpath/SDL3.framework/Versions/A/SDL3" "@rpath/libSDL3.dylib"
        "${_sdl_stage}/lib/${_ttf_runtime}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND codesign --force --sign - --timestamp=none
        "${_sdl_stage}/lib/${_ttf_runtime}" COMMAND_ERROR_IS_FATAL ANY)
else()
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${_cache_dir}/${_ttf_archive}"
        WORKING_DIRECTORY "${_ttf_unpack}" COMMAND_ERROR_IS_FATAL ANY)
    file(GLOB _ttf_roots "${_ttf_unpack}/SDL3_ttf-*")
    list(LENGTH _ttf_roots _ttf_count)
    if(NOT _ttf_count EQUAL 1)
        message(FATAL_ERROR "Expected one SDL3_ttf SDK root")
    endif()
    list(GET _ttf_roots 0 _ttf_root)
    file(COPY "${_ttf_root}/LICENSE.txt" DESTINATION "${_sdl_stage}/licenses/SDL3_ttf")
    if(_ttf_kind STREQUAL "vc" AND SNESBUILD_GOARCH STREQUAL "arm64")
        set(_ttf_libroot "${_ttf_root}/lib/arm64")
        file(COPY "${_ttf_root}/include/SDL3_ttf/" DESTINATION "${_sdl_stage}/include/SDL3_ttf")
        file(COPY "${_ttf_libroot}/SDL3_ttf.lib" "${_ttf_libroot}/SDL3_ttf.dll" DESTINATION "${_sdl_stage}/lib")
    elseif(_ttf_kind STREQUAL "mingw" AND SNESBUILD_GOARCH STREQUAL "amd64")
        set(_ttf_root "${_ttf_root}/x86_64-w64-mingw32")
        file(COPY "${_ttf_root}/include/SDL3_ttf/" DESTINATION "${_sdl_stage}/include/SDL3_ttf")
        file(COPY "${_ttf_root}/lib/libSDL3_ttf.dll.a" "${_ttf_root}/bin/SDL3_ttf.dll" DESTINATION "${_sdl_stage}/lib")
    else()
        message(FATAL_ERROR "Unsupported SDL3_ttf SDK ${_ttf_kind}/${SNESBUILD_GOARCH}")
    endif()
endif()
file(WRITE "${_ttf_stamp}" "${_ttf_expected}")
