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
    "host/host_ppu_output.c"
    "host/host_ppu_output.h"
    "sim/sim3d/sim3d_textures.c"
    "sim/sim3d/sim3d_textures.h"
    "diorama/present_diorama.c"
    "diorama/present_diorama.h"
    "diorama/diorama_planes.h"
    "present/presentation_surface.h"
    "dev/present_scene_inspector.c"
    "dev/present_scene_inspector.h"
    "render/present_hud.c"
    "render/present_hud.h"
    "diorama/diorama_capture.c"
    "diorama/diorama_capture.h"
    "action/action_effect_capture.c"
    "action/action_effect_capture.h"
    "sim/sim3d/sim3d_camera.h"
    "sim/sim3d/sim3d_camera_motion.c"
    "action/present_action_effects.c"
    "action/present_action_effects.h"
    "diorama/diorama_camera.c"
    "diorama/diorama_camera.h"
    "app/performance_metrics.c"
    "app/performance_overlay.c"
    "app/performance_boundary_probe.h"
    "host/parallel_work.h"
    "sim/sim3d/sim_cloud_effect_backend.h"
    "sim/sim3d/present_sim3d_underlay.c"
    "sim/world_nav/present_world_nav_geometry.c"
    "sim/world_nav/present_world_nav_sky.c"
    "sim/world_nav/present_world_nav_model_mesh.c"
    "sim/world_nav/present_world_nav_model_mesh.h"
    "sim/world_nav/present_sky_palace.c"
    "present/presentation_view.c"
    "present/render_preparation.c"
    "present/render_preparation_boundary_probe.h"
    "sim/world_nav/present_world_nav_boundary_probe.h"
    "sim/world_nav/sim_world_navigation_globe.c"
    # New headers must be covered without adding an explicit filename.
    "sim/world_nav/sim_world_navigation_boundary_probe.h"
    "sim/town/sim_town_ground_art.c"
    "sim/voxels/sim_background_voxels.c"
    "sim/voxels/sim_background_voxels.h"
    "sim/voxels/sim_background_voxel_model_cache.h")
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
foreach(_camera IN ITEMS diorama/diorama_camera.c sim/sim3d/sim3d_camera_motion.c)
    set(_camera_path "${_scratch}/${_camera}")
    file(READ "${_camera_path}" _original)
    foreach(_probe IN ITEMS "g_settings.diorama_camera_mode" "g_ram[0]" "HostClock_Nanoseconds()")
        file(WRITE "${_camera_path}" "${_original}\nvoid camera_probe(void) { (void)${_probe}; }\n")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" "-DGAME_SOURCE_ROOT=${_scratch}" -P "${BOUNDARY_CHECK}"
            RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
        if(_result STREQUAL "0" OR NOT _stderr MATCHES "Reactive camera bypasses its captured inputs")
            file(REMOVE_RECURSE "${_scratch}")
            message(FATAL_ERROR "Camera boundary accepted or misclassified ${_probe}: ${_stdout}${_stderr}")
        endif()
    endforeach()
    file(WRITE "${_camera_path}" "${_original}")
endforeach()

set(_action_path "${_scratch}/action/present_action_effects.c")
file(READ "${_action_path}" _original)
foreach(_probe IN ITEMS "g_settings.action_particles" "g_ram[0]" "g_render_device")
    file(WRITE "${_action_path}" "${_original}\nvoid action_probe(void) { (void)${_probe}; }\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DGAME_SOURCE_ROOT=${_scratch}" -P "${BOUNDARY_CHECK}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(_result STREQUAL "0" OR NOT _stderr MATCHES "Action effects bypass their presentation inputs")
        file(REMOVE_RECURSE "${_scratch}")
        message(FATAL_ERROR "Action boundary accepted or misclassified ${_probe}: ${_stdout}${_stderr}")
    endif()
endforeach()
file(WRITE "${_action_path}" "${_original}")

set(_hud_path "${_scratch}/render/present_hud.c")
file(READ "${_hud_path}" _original)
foreach(_probe IN ITEMS "g_settings.hud_scale_percent" "g_ram[0]" "HostClock_Nanoseconds()" "g_render_device")
    file(WRITE "${_hud_path}" "${_original}\nvoid hud_probe(void) { (void)${_probe}; }\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DGAME_SOURCE_ROOT=${_scratch}" -P "${BOUNDARY_CHECK}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(_result STREQUAL "0" OR NOT _stderr MATCHES "HUD presentation bypasses its captured inputs")
        file(REMOVE_RECURSE "${_scratch}")
        message(FATAL_ERROR "HUD boundary accepted or misclassified ${_probe}: ${_stdout}${_stderr}")
    endif()
endforeach()
file(WRITE "${_hud_path}" "${_original}")

set(_diorama_path "${_scratch}/diorama/present_diorama.c")
file(READ "${_diorama_path}" _original)
foreach(_probe IN ITEMS "g_settings.diorama_mode" "g_ram[0]" "g_render_device")
    file(WRITE "${_diorama_path}" "${_original}\nvoid diorama_probe(void) { (void)${_probe}; }\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DGAME_SOURCE_ROOT=${_scratch}" -P "${BOUNDARY_CHECK}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(_result STREQUAL "0" OR NOT _stderr MATCHES "Diorama presentation bypasses its captured inputs")
        file(REMOVE_RECURSE "${_scratch}")
        message(FATAL_ERROR "Diorama boundary accepted or misclassified ${_probe}: ${_stdout}${_stderr}")
    endif()
endforeach()
file(WRITE "${_diorama_path}" "${_original}")

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
# A moved or renamed file must fail the check instead of silently dropping out
# of a glob. present_sky_palace.[ch] are the only files one pattern covers.
set(_moved_files "sim/world_nav/present_sky_palace.c" "sim/world_nav/present_sky_palace.h")
foreach(_relative IN LISTS _moved_files)
    file(RENAME "${_scratch}/${_relative}" "${_scratch}/${_relative}.moved")
endforeach()
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DGAME_SOURCE_ROOT=${_scratch}" -P "${BOUNDARY_CHECK}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
if(_result STREQUAL "0" OR NOT _stderr MATCHES "pattern matches no files" OR
   NOT _stderr MATCHES "present_sky_palace")
    file(REMOVE_RECURSE "${_scratch}")
    message(FATAL_ERROR "Boundary checker accepted a pattern that matches nothing: ${_stdout}${_stderr}")
endif()
foreach(_relative IN LISTS _moved_files)
    file(RENAME "${_scratch}/${_relative}.moved" "${_scratch}/${_relative}")
endforeach()
set(_reference_path "${_scratch}/sim/sim3d/sim3d_depth_pass.h")
file(READ "${_reference_path}" _original)
foreach(_probe IN ITEMS "#include \"sim3d_depth_reference.h\"" "void *Sim3DDepthPass_CreateModelMesh(void)")
    file(WRITE "${_reference_path}" "${_original}\n${_probe}\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DGAME_SOURCE_ROOT=${_scratch}" -P "${BOUNDARY_CHECK}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(_result STREQUAL "0" OR NOT _stderr MATCHES "Shipping source depends on depth test references")
        file(REMOVE_RECURSE "${_scratch}")
        message(FATAL_ERROR "Reference boundary checker accepted or misclassified ${_probe}: ${_stdout}${_stderr}")
    endif()
endforeach()
file(REMOVE_RECURSE "${_scratch}")
message(STATUS "Navigation, font-resource, moved-file and depth-reference boundary negative cases: PASS")
