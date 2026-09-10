# Resolve only while making installer archives. Players use the included SDK.
set(_sdl_args --goos ${SNESBUILD_GOOS} --goarch ${SNESBUILD_GOARCH}
    --sdl-version ${SNESBUILD_SDL3_VERSION} --ttf-version ${SNESBUILD_SDL3_TTF_VERSION})
if(SNESBUILD_SDL_LOCKFILE)
    list(APPEND _sdl_args --lock "${SNESBUILD_SDL_LOCKFILE}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E env GOCACHE=${_go_cache}
    ${GO_EXECUTABLE} -C ${SNESBUILD_MODULE_DIR} run ./cmd/snesbuild sdl resolve ${_sdl_args}
    OUTPUT_VARIABLE _sdl_lock OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
string(JSON _sdl_version GET "${_sdl_lock}" sdl version)
string(JSON _ttf_version GET "${_sdl_lock}" ttf version)
message(STATUS "Bundling SDL3 ${_sdl_version} / SDL3_ttf ${_ttf_version} for ${SNESBUILD_GOOS}/${SNESBUILD_GOARCH}")
file(WRITE "${CMAKE_BINARY_DIR}/sdl-sdk.lock.json" "${_sdl_lock}\n")
set(_sdl_stage "${CMAKE_BINARY_DIR}/deps/sdl3")
set(SNESBUILD_SDL3_BUNDLED FALSE)

# Invalidate the complete SDK whenever either resolved input changes. This
# also drops stale headers, old libraries, and previous staging-format files.
set(_sdl_expected "format=4\n${_sdl_lock}\n")
set(_sdl_installed "")
if(EXISTS "${_sdl_stage}/sdk.pin")
    file(READ "${_sdl_stage}/sdk.pin" _sdl_installed)
endif()
if(NOT _sdl_installed STREQUAL _sdl_expected)
    file(REMOVE_RECURSE "${_sdl_stage}")
endif()
if(NOT SNESBUILD_GOOS STREQUAL "linux")
    string(JSON _sdl_kind GET "${_sdl_lock}" sdl kind)
    string(JSON _sdl_url GET "${_sdl_lock}" sdl archives 0 url)
    string(JSON _sdl_src_sha GET "${_sdl_lock}" sdl archives 0 sha256)
    string(JSON _sdl_archive GET "${_sdl_lock}" sdl archives 0 archive)
endif()
