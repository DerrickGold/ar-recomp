# Exercise the production file inventory as well as its token checks. The
# mutations belong only to a disposable copy; no game source is edited.
if(NOT DEFINED GAME_SOURCE_ROOT OR NOT DEFINED BOUNDARY_CHECK)
    message(FATAL_ERROR "GAME_SOURCE_ROOT and BOUNDARY_CHECK are required")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
set(_scratch "${CMAKE_CURRENT_BINARY_DIR}/render-boundary-negative-${_suffix}")
if(EXISTS "${_scratch}")
    message(FATAL_ERROR "Refusing to reuse boundary-test scratch directory")
endif()
file(MAKE_DIRECTORY "${_scratch}")
file(COPY "${GAME_SOURCE_ROOT}/" DESTINATION "${_scratch}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DGAME_SOURCE_ROOT=${_scratch}" -P "${BOUNDARY_CHECK}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
if(NOT _result STREQUAL "0")
    file(REMOVE_RECURSE "${_scratch}")
    message(FATAL_ERROR "Unmodified boundary control failed: ${_stdout}${_stderr}")
endif()

set(_cases
    "performance_metrics.c"
    "performance_overlay.c"
    "performance_boundary_probe.h"
    "host/parallel_work.h"
    "sim/sim_cloud_effect_backend.h"
    "present_sim3d_underlay.c"
    "present_world_nav_geometry.c"
    "present_world_nav_sky.c"
    "present_world_nav_boundary_probe.h"
    "sim/sim_world_navigation_globe.c"
    # New headers must be covered without adding an explicit filename.
    "sim/sim_world_navigation_boundary_probe.h"
    "sim/sim_town_ground_art.c"
    "sim/sim_background_voxels.c"
    "sim/sim_background_voxels.h"
    "sim/sim_background_voxel_model_cache.h")
foreach(_relative IN LISTS _cases)
    set(_path "${_scratch}/${_relative}")
    set(_existed false)
    set(_original "")
    if(EXISTS "${_path}")
        set(_existed true)
        file(READ "${_path}" _original)
    endif()
    file(WRITE "${_path}" "${_original}\nSDL_FPoint boundary_negative_probe;\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DGAME_SOURCE_ROOT=${_scratch}" -P "${BOUNDARY_CHECK}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(_result STREQUAL "0" OR NOT _stderr MATCHES "Portable rendering code depends on SDL")
        file(REMOVE_RECURSE "${_scratch}")
        message(FATAL_ERROR "Boundary checker accepted or misclassified ${_relative}: ${_stdout}${_stderr}")
    endif()
    string(FIND "${_stderr}" "${_relative}" _reported)
    if(_reported EQUAL -1)
        file(REMOVE_RECURSE "${_scratch}")
        message(FATAL_ERROR "Boundary checker did not identify ${_relative}: ${_stderr}")
    endif()
    if(_existed)
        file(WRITE "${_path}" "${_original}")
    else()
        file(REMOVE "${_path}")
    endif()
endforeach()
set(_font_cases
    "localization/text_backend.h"
    "localization/font_resource.c"
    "localization/localization_frame.h"
    "render/localized_text_presenter.c"
    "render/ui_text_renderer.c"
    "platform/sdl/text_rasterizer_sdl.c")
foreach(_relative IN LISTS _font_cases)
    set(_path "${_scratch}/${_relative}")
    file(READ "${_path}" _original)
    file(WRITE "${_path}" "${_original}\nvoid font_probe(void) { fopen(\"font.ttf\", \"rb\"); }\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DGAME_SOURCE_ROOT=${_scratch}" -P "${BOUNDARY_CHECK}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(_result STREQUAL "0" OR NOT _stderr MATCHES "Text font resource boundary bypassed")
        file(REMOVE_RECURSE "${_scratch}")
        message(FATAL_ERROR "Font boundary checker accepted or misclassified ${_relative}: ${_stdout}${_stderr}")
    endif()
    file(WRITE "${_path}" "${_original}")
endforeach()
file(REMOVE_RECURSE "${_scratch}")
message(STATUS "Navigation and font-resource boundary negative cases: PASS")
