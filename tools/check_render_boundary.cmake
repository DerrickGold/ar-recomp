if(NOT DEFINED GAME_SOURCE_ROOT)
    message(FATAL_ERROR "GAME_SOURCE_ROOT is required")
endif()

file(GLOB_RECURSE _portable_render_files
    "${GAME_SOURCE_ROOT}/render/*.c"
    "${GAME_SOURCE_ROOT}/render/*.h")
list(APPEND _portable_render_files
    "${GAME_SOURCE_ROOT}/presentation_upload_mirror.c"
    "${GAME_SOURCE_ROOT}/presentation_upload_mirror.h"
    "${GAME_SOURCE_ROOT}/hd_replacement_host.c"
    "${GAME_SOURCE_ROOT}/hd_replacement_host.h"
    "${GAME_SOURCE_ROOT}/hd_replacements.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_upload.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_upload.h"
    # Action effect construction is game-side geometry generation. Keep its
    # public contract and pure batch builder portable even while the diorama
    # projection adapter still calls the native compositor implementation.
    "${GAME_SOURCE_ROOT}/action/action_effect_render.c"
    "${GAME_SOURCE_ROOT}/action/action_effect_render.h"
    "${GAME_SOURCE_ROOT}/action/action_effect_projection.h")

set(_violations "")
foreach(_file IN LISTS _portable_render_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES "#[ \t]*include[ \t]*[<\"]SDL" OR
       _contents MATCHES "SDL_[A-Za-z0-9_]+")
        list(APPEND _violations "${_file}")
    endif()
endforeach()

if(_violations)
    list(JOIN _violations "\n  " _formatted)
    message(FATAL_ERROR
        "Portable rendering code depends on SDL:\n  ${_formatted}\n"
        "Move native types and calls behind src/platform/sdl/.")
endif()

# Persistent game-facing presentation resources must not regress from opaque
# handles merely because their current compositor still has an SDL migration
# bridge. Private effect/render-target textures are intentionally out of scope
# until those subsystems move behind backend operations.
set(_resource_owner_files
    "${GAME_SOURCE_ROOT}/main.c"
    "${GAME_SOURCE_ROOT}/present.c"
    "${GAME_SOURCE_ROOT}/present_sim3d.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_effects.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_shadows.c")
set(_native_resource_violations "")
foreach(_file IN LISTS _resource_owner_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES
       "SDL_Texture[ \t]*\\*[ \t]*g_(diorama_textures|sim_obj_atlas_texture|sim3d_layer_textures|sim3d_flat_texture)")
        list(APPEND _native_resource_violations "${_file}")
    endif()
endforeach()
if(_native_resource_violations)
    list(JOIN _native_resource_violations "\n  " _formatted)
    message(FATAL_ERROR
        "Persistent presentation resources regressed to SDL ownership:\n  "
        "${_formatted}\nUse ArRenderTexture and keep native access in the adapter bridge.")
endif()
