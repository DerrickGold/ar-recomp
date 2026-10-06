# Packaging tools run on the player's Linux machine after recompilation. They
# are carried unmodified; no FUSE mount or network fetch is needed at build time.
# Named release asset digests verified against downloaded bytes on 2026-10-05.
# appimagetool source: 8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81
# type2-runtime source: dd6cebedcbddde9c82f89b011e8e1d40b6e43868
if(NOT SNESBUILD_GOOS STREQUAL "linux")
    return()
endif()

if(SNESBUILD_GOARCH STREQUAL "amd64")
    set(_appimage_arch x86_64)
    set(_appimage_tool_sha ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0)
    set(_appimage_runtime_sha 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d)
elseif(SNESBUILD_GOARCH STREQUAL "arm64")
    set(_appimage_arch aarch64)
    set(_appimage_tool_sha f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158)
    set(_appimage_runtime_sha 00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444)
else()
    message(FATAL_ERROR "No AppImage toolchain for ${SNESBUILD_GOARCH}")
endif()

# Never pin checksums to mutable continuous builds. Update the named release
# and digest together; existing caches and fresh machines use the same bytes.
include("${CMAKE_CURRENT_LIST_DIR}/appimage-download.cmake")
set(ACTRAISER_APPIMAGE_TOOL "${_cache_dir}/appimagetool-${_appimage_arch}-${_appimage_tool_sha}.AppImage")
set(ACTRAISER_APPIMAGE_RUNTIME "${_cache_dir}/runtime-${_appimage_arch}-${_appimage_runtime_sha}")
appimage_download(
    "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-${_appimage_arch}.AppImage"
    "${ACTRAISER_APPIMAGE_TOOL}" "${_appimage_tool_sha}")
appimage_download(
    "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-${_appimage_arch}"
    "${ACTRAISER_APPIMAGE_RUNTIME}" "${_appimage_runtime_sha}")

install(PROGRAMS "${ACTRAISER_APPIMAGE_TOOL}" DESTINATION utils/tools RENAME appimagetool)
install(PROGRAMS "${ACTRAISER_APPIMAGE_RUNTIME}" DESTINATION utils/tools RENAME appimage-runtime)
install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/licenses/appimage/" DESTINATION utils/licenses/AppImage)
