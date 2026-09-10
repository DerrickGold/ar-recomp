# Packaging tools run on the player's Linux machine after recompilation. They
# are carried unmodified; no FUSE mount or network fetch is needed at build time.
# Pins observed from upstream release asset digests on 2026-09-10.
# appimagetool source: 8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81
# type2-runtime source: 75849dce7cc37e4319b633df1f116ca895c71a12
if(NOT SNESBUILD_GOOS STREQUAL "linux")
    return()
endif()

if(SNESBUILD_GOARCH STREQUAL "amd64")
    set(_appimage_arch x86_64)
    set(_appimage_tool_sha a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0)
    set(_appimage_runtime_sha 1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf)
elseif(SNESBUILD_GOARCH STREQUAL "arm64")
    set(_appimage_arch aarch64)
    set(_appimage_tool_sha 1b00524ba8c6b678dc15ef88a5c25ec24def36cdfc7e3abb32ddcd068e8007fe)
    set(_appimage_runtime_sha 7d5d772b7c32f0c84caf0a452a3072a5709027d7eac5856feb89a7a7a8881372)
else()
    message(FATAL_ERROR "No AppImage toolchain for ${SNESBUILD_GOARCH}")
endif()

# The upstream continuous URLs are mutable; hashes make a replaced release
# fail closed. Preserve the cache or update URL + digest together on a pin bump.
set(ACTRAISER_APPIMAGE_TOOL "${_cache_dir}/appimagetool-${_appimage_arch}-${_appimage_tool_sha}.AppImage")
set(ACTRAISER_APPIMAGE_RUNTIME "${_cache_dir}/runtime-${_appimage_arch}-${_appimage_runtime_sha}")
file(DOWNLOAD
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${_appimage_arch}.AppImage"
    "${ACTRAISER_APPIMAGE_TOOL}" EXPECTED_HASH SHA256=${_appimage_tool_sha})
file(DOWNLOAD
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-${_appimage_arch}"
    "${ACTRAISER_APPIMAGE_RUNTIME}" EXPECTED_HASH SHA256=${_appimage_runtime_sha})

install(PROGRAMS "${ACTRAISER_APPIMAGE_TOOL}" DESTINATION utils/tools RENAME appimagetool)
install(PROGRAMS "${ACTRAISER_APPIMAGE_RUNTIME}" DESTINATION utils/tools RENAME appimage-runtime)
install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/licenses/appimage/" DESTINATION utils/licenses/AppImage)
