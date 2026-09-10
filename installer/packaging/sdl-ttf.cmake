# Included after SDL3 staging, which may replace the entire shared SDK root.
# All versions and checksums come from the Go pin registry. No retail assets
# or authoring tools are part of this dependency payload.
if(NOT SNESBUILD_SDL3_BUNDLED)
    return() # Generic Linux deliberately uses system development packages.
endif()

if(SNESBUILD_STEAM_DECK)
    execute_process(COMMAND ${CMAKE_COMMAND} -E env GOCACHE=${_go_cache}
        ${GO_EXECUTABLE} -C ${SNESBUILD_MODULE_DIR}
        run ./cmd/snesbuild toolchain pin --steam-deck-sdl-ttf
        OUTPUT_VARIABLE _ttf_pin OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    separate_arguments(_ttf_fields UNIX_COMMAND "${_ttf_pin}")
    list(GET _ttf_fields 0 _ttf_headers_url)
    list(GET _ttf_fields 1 _ttf_headers_sha)
    list(GET _ttf_fields 2 _ttf_headers_archive)
    list(GET _ttf_fields 3 _ttf_url)
    list(GET _ttf_fields 4 _ttf_sha)
    list(GET _ttf_fields 5 _ttf_archive)
    set(_ttf_kind deck)
else()
    execute_process(COMMAND ${CMAKE_COMMAND} -E env GOCACHE=${_go_cache}
        ${GO_EXECUTABLE} -C ${SNESBUILD_MODULE_DIR}
        run ./cmd/snesbuild toolchain pin --sdl-ttf
        --goos ${SNESBUILD_GOOS} --goarch ${SNESBUILD_GOARCH}
        OUTPUT_VARIABLE _ttf_pin OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    separate_arguments(_ttf_fields UNIX_COMMAND "${_ttf_pin}")
    list(GET _ttf_fields 0 _ttf_url)
    list(GET _ttf_fields 1 _ttf_sha)
    list(GET _ttf_fields 2 _ttf_archive)
    list(GET _ttf_fields 3 _ttf_kind)
endif()

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
elseif(_ttf_kind STREQUAL "deck")
    file(DOWNLOAD "${_ttf_headers_url}" "${_cache_dir}/${_ttf_headers_archive}"
        EXPECTED_HASH SHA256=${_ttf_headers_sha})
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${_cache_dir}/${_ttf_headers_archive}"
        WORKING_DIRECTORY "${_ttf_unpack}" COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX REPLACE "\\.tar\\.gz$" "" _ttf_headers_root "${_ttf_headers_archive}")
    file(COPY "${_ttf_unpack}/${_ttf_headers_root}/include/SDL3_ttf/"
        DESTINATION "${_sdl_stage}/include/SDL3_ttf")
    file(COPY "${_ttf_unpack}/${_ttf_headers_root}/LICENSE.txt"
        DESTINATION "${_sdl_stage}/licenses/SDL3_ttf")
    # Isolate each Debian container so different data.tar compression suffixes
    # cannot accidentally make us extract SDL3's old payload as SDL3_ttf.
    file(MAKE_DIRECTORY "${_ttf_unpack}/runtime")
    find_program(_ttf_ar ar REQUIRED)
    execute_process(COMMAND ${_ttf_ar} x "${_cache_dir}/${_ttf_archive}"
        WORKING_DIRECTORY "${_ttf_unpack}/runtime" COMMAND_ERROR_IS_FATAL ANY)
    file(GLOB _ttf_data "${_ttf_unpack}/runtime/data.tar.*")
    list(LENGTH _ttf_data _ttf_count)
    if(NOT _ttf_count EQUAL 1)
        message(FATAL_ERROR "Expected one SDL3_ttf Debian data archive")
    endif()
    list(GET _ttf_data 0 _ttf_data)
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${_ttf_data}"
        WORKING_DIRECTORY "${_ttf_unpack}/runtime" COMMAND_ERROR_IS_FATAL ANY)
    file(GLOB _ttf_binaries "${_ttf_unpack}/runtime/usr/lib/x86_64-linux-gnu/libSDL3_ttf.so.0.*")
    list(LENGTH _ttf_binaries _ttf_count)
    if(NOT _ttf_count EQUAL 1)
        message(FATAL_ERROR "Expected one versioned x86_64 SDL3_ttf library")
    endif()
    list(GET _ttf_binaries 0 _ttf_binary)
    file(COPY_FILE "${_ttf_binary}" "${_sdl_stage}/lib/${_ttf_runtime}")
    file(COPY_FILE "${_ttf_binary}" "${_sdl_stage}/lib/${_ttf_link}")
    file(COPY "${_ttf_unpack}/runtime/usr/share/doc/libsdl3-ttf0/copyright"
        DESTINATION "${_sdl_stage}/licenses/SDL3_ttf")
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
