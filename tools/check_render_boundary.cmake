if(NOT DEFINED GAME_SOURCE_ROOT)
    message(FATAL_ERROR "GAME_SOURCE_ROOT is required")
endif()

# Action passes consume captured inputs and an explicit render device. Keep
# ownership of live game state and the host device outside this feature.
file(READ "${GAME_SOURCE_ROOT}/action/present_action_effects.c" _action_contents)
if(_action_contents MATCHES
   "(^|[^A-Za-z0-9_])(g_settings|g_ram|g_ppu|g_render_device|ActRaiser_ReadWram16|HostClock_Nanoseconds)([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR "Action effects bypass their presentation inputs")
endif()

# The diorama presenter owns GPU state, but receives completed game inputs.
file(READ "${GAME_SOURCE_ROOT}/diorama/present_diorama.c" _diorama_contents)
if(_diorama_contents MATCHES
   "(^|[^A-Za-z0-9_])(g_settings|g_ram|g_ppu|g_render_device|ActRaiser_ReadWram16)([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR "Diorama presentation bypasses its captured inputs")
endif()

# HUD owns its textures and receives both the device and captured game inputs.
file(READ "${GAME_SOURCE_ROOT}/render/present_hud.c" _hud_contents)
if(_hud_contents MATCHES
   "(^|[^A-Za-z0-9_])(g_settings|g_ram|g_ppu|g_render_device|ActRaiser_ReadWram16|HostClock_Nanoseconds)([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR "HUD presentation bypasses its captured inputs")
endif()

# HD composition consumes the captured frame and explicit device/texture handles.
file(READ "${GAME_SOURCE_ROOT}/replacements/present_hd_replacements.c" _hd_contents)
if(_hd_contents MATCHES
   "(^|[^A-Za-z0-9_])(g_settings|g_ram|g_ppu|g_render_device|g_hd_replacements|g_m7_texture|ActRaiser_ReadWram16)([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR "HD presentation bypasses its captured inputs")
endif()

# Reactive cameras receive observations and clocks explicitly. Host adapters
# capture live state; the response models cannot silently recapture it.
foreach(_camera IN ITEMS diorama/diorama_camera.c sim/sim3d/sim3d_camera_motion.c)
    file(READ "${GAME_SOURCE_ROOT}/${_camera}" _camera_contents)
    if(_camera_contents MATCHES
       "(^|[^A-Za-z0-9_])(g_settings|g_ram|g_ppu|ActRaiser_ReadWram16|HostClock_Nanoseconds|HostClock_Milliseconds)([^A-Za-z0-9_]|$)")
        message(FATAL_ERROR "Reactive camera bypasses its captured inputs: ${_camera}")
    endif()
endforeach()

# Every pattern below must match at least one file. A pattern that matches
# nothing is almost always a file that was moved or renamed, and it would let
# this check pass while checking nothing. The explicit file lists need no such
# guard: reading a missing file is already an error.
set(_boundary_script "${CMAKE_CURRENT_LIST_FILE}")
function(ar_glob_required out_var mode)
    set(_matched_all "")
    foreach(_pattern IN LISTS ARGN)
        file(${mode} _matched "${_pattern}")
        if(NOT _matched)
            message(FATAL_ERROR
                "Boundary check pattern matches no files:\n  ${_pattern}\n"
                "A file it covered was probably moved or renamed. Update the "
                "pattern in ${_boundary_script} so the check keeps covering it.")
        endif()
        list(APPEND _matched_all ${_matched})
    endforeach()
    set(${out_var} "${_matched_all}" PARENT_SCOPE)
endfunction()

# The portable localization core is held to the same rule as src/render: pack
# loading, contracts, sessions, grapheme handling and the rasterizer/backend
# contracts must not name an SDL type. Desktop enumeration lives in
# src/platform/sdl/pack_discovery_sdl.c, not here.
ar_glob_required(_portable_render_files GLOB_RECURSE
    "${GAME_SOURCE_ROOT}/app/performance*.c"
    "${GAME_SOURCE_ROOT}/app/performance*.h"
    "${GAME_SOURCE_ROOT}/sim/world_nav/present_world_nav*.c"
    "${GAME_SOURCE_ROOT}/sim/world_nav/present_world_nav*.h"
    "${GAME_SOURCE_ROOT}/sim/world_nav/present_sky_palace*.[ch]"
    "${GAME_SOURCE_ROOT}/present/presentation_view*.[ch]"
    "${GAME_SOURCE_ROOT}/present/render_preparation*.[ch]"
    "${GAME_SOURCE_ROOT}/render/render_capabilities.h"
    "${GAME_SOURCE_ROOT}/render/*.c"
    "${GAME_SOURCE_ROOT}/render/*.h"
    "${GAME_SOURCE_ROOT}/localization/*.c"
    "${GAME_SOURCE_ROOT}/localization/*.h"
    # Navigation owns game geometry/art, not backend resources. Include the
    # entire helper families so a newly added file cannot bypass this check.
    "${GAME_SOURCE_ROOT}/sim/world_nav/sim_world_navigation_*.c"
    "${GAME_SOURCE_ROOT}/sim/world_nav/sim_world_navigation_*.h"
    "${GAME_SOURCE_ROOT}/sim/town/sim_town_ground_art.c"
    "${GAME_SOURCE_ROOT}/sim/town/sim_town_ground_art.h"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxel_*.c"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxel_*.h")
list(APPEND _portable_render_files
    "${GAME_SOURCE_ROOT}/host/host_ppu_output.c"
    "${GAME_SOURCE_ROOT}/host/host_ppu_output.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_textures.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_textures.h"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxels.c"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxels.h"
    "${GAME_SOURCE_ROOT}/present/presentation_upload_mirror.c"
    "${GAME_SOURCE_ROOT}/present/presentation_upload_mirror.h"
    "${GAME_SOURCE_ROOT}/replacements/hd_replacement_host.c"
    "${GAME_SOURCE_ROOT}/replacements/hd_replacement_host.h"
    "${GAME_SOURCE_ROOT}/replacements/hd_replacements.h"
    "${GAME_SOURCE_ROOT}/replacements/present_hd_replacements.c"
    "${GAME_SOURCE_ROOT}/replacements/present_hd_replacements.h"
    "${GAME_SOURCE_ROOT}/render/crt_post.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_upload.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_upload.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_rom_skybox_resource.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_rom_skybox_resource.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_edge_aa.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_edge_aa.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_aperture.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_aperture.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_effect_backend.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_capture.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_capture.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_planes.h"
    "${GAME_SOURCE_ROOT}/diorama/present_diorama.c"
    "${GAME_SOURCE_ROOT}/diorama/present_diorama.h"
    "${GAME_SOURCE_ROOT}/present/presentation_surface.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_camera.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_camera.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_performance.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_performance.h"
    "${GAME_SOURCE_ROOT}/host/host_clock.h"
    "${GAME_SOURCE_ROOT}/host/parallel_work.h"
    "${GAME_SOURCE_ROOT}/dev/present_scene_inspector.c"
    "${GAME_SOURCE_ROOT}/dev/present_scene_inspector.h"
    "${GAME_SOURCE_ROOT}/dev/dev_tools.c"
    "${GAME_SOURCE_ROOT}/dev/dev_tools.h"
    "${GAME_SOURCE_ROOT}/dev/dev_tools_readback.h"
    "${GAME_SOURCE_ROOT}/action/action_effect_capture.c"
    "${GAME_SOURCE_ROOT}/action/action_effect_capture.h"
    "${GAME_SOURCE_ROOT}/present/frame_slot.c"
    "${GAME_SOURCE_ROOT}/present/present.h"
    "${GAME_SOURCE_ROOT}/present/present.c"
    "${GAME_SOURCE_ROOT}/present/present_frame.c"
    "${GAME_SOURCE_ROOT}/present/present_internal.h"
    "${GAME_SOURCE_ROOT}/sim/world_nav/present_world_nav.c"
    "${GAME_SOURCE_ROOT}/sim/world_nav/present_sim_globe.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_internal.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_environment.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_environment.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_clouds.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_clouds.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_underlay.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_underlay.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_effects.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_effects.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_project.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_project.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_shadows.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_shadows.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_terrain.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_terrain.h"
    "${GAME_SOURCE_ROOT}/settings_overlay/settings_overlay_render.h"
    # Action effect construction is game-side geometry generation. Keep its
    # public contract and pure batch builder portable even while the diorama
    # projection adapter still calls the native compositor implementation.
    "${GAME_SOURCE_ROOT}/action/action_effect_render.c"
    "${GAME_SOURCE_ROOT}/action/action_scene_effect_render.c"
    "${GAME_SOURCE_ROOT}/action/action_forest_effect_render.c"
    "${GAME_SOURCE_ROOT}/action/action_environment_geometry.c"
    "${GAME_SOURCE_ROOT}/action/action_cave_effect_render.c"
    "${GAME_SOURCE_ROOT}/action/action_bloodpool_effect_render.c"
    "${GAME_SOURCE_ROOT}/action/action_bloodpool_detail_render.c"
    "${GAME_SOURCE_ROOT}/action/action_castle_effect_render.c"
    "${GAME_SOURCE_ROOT}/action/action_scene_lightning_render.c"
    "${GAME_SOURCE_ROOT}/action/action_effect_render_internal.h"
    "${GAME_SOURCE_ROOT}/action/action_effect_render.h"
    "${GAME_SOURCE_ROOT}/action/present_action_effects.c"
    "${GAME_SOURCE_ROOT}/action/present_action_effects.h"
    "${GAME_SOURCE_ROOT}/action/action_effect_projection.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim_backdrop_render.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim_backdrop_render.h"
    # The enhanced SIM scene's depth contract is game-side; its current GPU
    # implementation belongs to the SDL platform adapter.
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_depth_pass.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_mesh_set.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_mesh_set.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_camera.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_camera.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_camera_motion.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_performance.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim3d_performance.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim_shadow_effect_backend.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/sim_cloud_effect_backend.h"
    "${GAME_SOURCE_ROOT}/sim/mountains/sim_background_mountain_render.c"
    "${GAME_SOURCE_ROOT}/sim/mountains/sim_background_mountain_render.h"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxel_renderer.c"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxel_renderer.h"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxel_project.c"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxel_project.h"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxel_terrain_depth.c"
    "${GAME_SOURCE_ROOT}/sim/voxels/sim_background_voxel_terrain_depth.h"
    # Diorama callers and the compositor share only opaque texture handles,
    # portable geometry, and render-device output/viewport operations. Frame
    # synthesis remains a separate optional platform adapter.
    "${GAME_SOURCE_ROOT}/diorama/diorama.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_frame_generation.h"
    "${GAME_SOURCE_ROOT}/diorama/diorama_projection.c")
list(REMOVE_DUPLICATES _portable_render_files)

# Font byte acquisition is host policy, not a renderer/frame responsibility.
# Keep both the portable contracts and the SDL rasterizer free of path opens;
# the backend receives immutable leased bytes, including for new raster sizes.
ar_glob_required(_font_resource_files GLOB
    "${GAME_SOURCE_ROOT}/localization/font_resource*.[ch]"
    "${GAME_SOURCE_ROOT}/localization/text_backend*.[ch]"
    "${GAME_SOURCE_ROOT}/localization/text_presentation*.[ch]"
    "${GAME_SOURCE_ROOT}/localization/localization_frame*.[ch]"
    "${GAME_SOURCE_ROOT}/render/localized_text*.[ch]"
    "${GAME_SOURCE_ROOT}/render/ui_text_renderer*.[ch]"
    "${GAME_SOURCE_ROOT}/platform/sdl/text_rasterizer*.[ch]")
foreach(_file IN LISTS _font_resource_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES "(fopen|SDL_IOFromFile|TTF_OpenFont)[ \t\r\n]*[(]" OR
       _contents MATCHES "ArHostFontResources_|ArLanguagePack_ResolveMemberPath" OR
       _contents MATCHES "primary_font_path|fallback_font_paths|FontPathCapacity")
        message(FATAL_ERROR
            "Text font resource boundary bypassed: ${_file}\n"
            "Resolve font members in the host and acquire immutable resource bytes.")
    endif()
endforeach()

# Dependency direction: the portable render and localization layers may not
# depend on the game, and may not name one of its screens. Cell geometry for a
# fixed menu arrives as an ArLocalizationTextGrid published by the game
# adapter, so nothing here needs to know which menu it is drawing.
ar_glob_required(_game_neutral_files GLOB_RECURSE
    "${GAME_SOURCE_ROOT}/render/*.c"
    "${GAME_SOURCE_ROOT}/render/*.h"
    "${GAME_SOURCE_ROOT}/localization/*.c"
    "${GAME_SOURCE_ROOT}/localization/*.h")
set(_game_knowledge_violations "")
foreach(_file IN LISTS _game_neutral_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES "#[ 	]*include[ 	]*[<\"]actraiser/" OR
       _contents MATCHES "ActRaiser[A-Za-z0-9_]*" OR
       _contents MATCHES "kArLocalizationTextLayout_(StatusCities|StatusScore|StatusMaster|MessageSpeed|FixedRows)")
        list(APPEND _game_knowledge_violations "${_file}")
    endif()
endforeach()
if(_game_knowledge_violations)
    list(JOIN _game_knowledge_violations "\n  " _formatted)
    message(FATAL_ERROR
        "Portable render/localization code depends on the game:\n  ${_formatted}\n"
        "Publish what the renderer needs as an ArLocalizationTextGrid from "
        "src/actraiser instead of naming a screen here.")
endif()

# Layer direction inside the portable code: the renderer consumes the
# localization contract, not the other way round. The one exception is
# render_types.h, the shared pixel/rectangle vocabulary the rasterizer contract
# deliberately speaks so upload code needs no native graphics header.
set(_layer_violations "")
ar_glob_required(_localization_files GLOB_RECURSE
    "${GAME_SOURCE_ROOT}/localization/*.c"
    "${GAME_SOURCE_ROOT}/localization/*.h")
foreach(_file IN LISTS _localization_files)
    file(READ "${_file}" _contents)
    string(REGEX MATCHALL "#[ 	]*include[ 	]*.render/[A-Za-z0-9_]+[.]h"
           _render_includes "${_contents}")
    foreach(_include IN LISTS _render_includes)
        if(NOT _include MATCHES "render/render_types[.]h")
            list(APPEND _layer_violations "${_file}: ${_include}")
        endif()
    endforeach()
endforeach()
if(_layer_violations)
    list(JOIN _layer_violations "\n  " _formatted)
    message(FATAL_ERROR
        "Localization code depends on the renderer:\n  ${_formatted}\n"
        "The renderer consumes the localization contract, not the reverse.")
endif()

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

# Native renderer/texture access is an implementation detail of SDL-owned
# adapters. Focused tests may include the internal header, but no game or host
# source outside platform/sdl may acquire a native graphics handle.
ar_glob_required(_game_source_files GLOB_RECURSE
    "${GAME_SOURCE_ROOT}/*.c"
    "${GAME_SOURCE_ROOT}/*.h")
set(_sdl_internal_violations "")
foreach(_file IN LISTS _game_source_files)
    if(_file MATCHES "/platform/sdl/")
        continue()
    endif()
    file(READ "${_file}" _contents)
    if(_contents MATCHES "sim3d_depth_reference[.]h|Sim3DDepthReference_|Sim3DDepthPass_(CreateModelMesh|CreateHardwareClippedModelMesh|UpdateModelMesh|AppendModelMesh)")
        message(FATAL_ERROR
            "Shipping source depends on depth test references: ${_file}\n"
            "Reference models belong only to focused tests/benchmarks and their test-enabled adapter.")
    endif()
    if(_contents MATCHES "platform/sdl/render_sdl_internal.h" OR
       _contents MATCHES "ArSdlRenderBackend_(Borrow|Unwrap)Texture" OR
       _contents MATCHES "ArSdlRenderBackend_Renderer")
        list(APPEND _sdl_internal_violations "${_file}")
    endif()
endforeach()
if(_sdl_internal_violations)
    list(JOIN _sdl_internal_violations "\n  " _formatted)
    message(FATAL_ERROR
        "Game/host source reached into SDL-native render interop:\n  "
        "${_formatted}\nKeep native handles inside src/platform/sdl/.")
endif()

# Persistent game-facing presentation resources must not regress from opaque
# handles while the surrounding presentation code is migrated incrementally.
# Private effect/render-target textures are intentionally out of scope until
# those subsystems move behind backend operations.
set(_resource_owner_files
    "${GAME_SOURCE_ROOT}/main.c"
    "${GAME_SOURCE_ROOT}/app/application.c"
    "${GAME_SOURCE_ROOT}/app/game_session.c"
    "${GAME_SOURCE_ROOT}/app/game_loop.c"
    "${GAME_SOURCE_ROOT}/present/present.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_internal.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_environment.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_effects.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_shadows.c")
set(_native_resource_violations "")
foreach(_file IN LISTS _resource_owner_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES
       "SDL_Texture[ \t]*\\*[ \t]*(g_(diorama_textures|sim_obj_atlas_texture|sim3d_layer_textures|sim3d_flat_texture)|s_sim_(underlay_texture|underlay_blur_texture|canvas_texture)|s_action_heat_target)")
        list(APPEND _native_resource_violations "${_file}")
    endif()
endforeach()

# The authored ROM skybox cache and compositor are device-owned/portable; keep
# the focused checks below as explicit diagnostics for common regressions.
file(READ "${GAME_SOURCE_ROOT}/diorama/diorama.c" _diorama_contents)
if(_diorama_contents MATCHES
   "(^|[^A-Za-z0-9_])(g_settings|g_ram)([^A-Za-z0-9_]|$)|DioramaLayerManifest_|DioramaRomSkyboxResource_|HostClock_|UserDataFile|getenv\\(")
    message(FATAL_ERROR
        "Diorama compositor has an implicit desktop scene input: "
        "${GAME_SOURCE_ROOT}/diorama/diorama.c. Pass captured options/resources instead.")
endif()
if(_diorama_contents MATCHES
   "SDL_GPU(Shader|RenderState|Device)|SDL_SetGPURenderState")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/diorama/diorama.c (native effect state)")
endif()
if(_diorama_contents MATCHES
   "SDL_Texture[ \t]*\\*[ \t]*(art_texture|target_texture|s_diorama_ss_texture)")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/diorama/diorama.c (ROM skybox cache)")
endif()
if(_diorama_contents MATCHES
   "SDL_(Texture|Vertex|FColor|FPoint)|SDL_RenderGeometry|SDL_(Get|Set)TextureBlendMode|SDL_SetRenderTextureAddressMode|ArSdlRenderBackend_(Borrow|Unwrap)Texture")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/diorama/diorama.c (native mesh/resource submission)")
endif()

file(READ "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_shadows.c"
     _sim_shadow_contents)
if(_sim_shadow_contents MATCHES
   "SDL_GPU(Shader|RenderState|Device)|SDL_SetGPURenderState")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_shadows.c (native effect state)")
endif()

# Fullscreen post-processing exposes only semantic parameters and opaque
# textures. Native shader/target ownership belongs to the selected adapter;
# player settings remain frame-orchestration policy.
if(EXISTS "${GAME_SOURCE_ROOT}/render/crt_post.c")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/render/crt_post.c (CRT implementation outside adapter)")
endif()
file(READ "${GAME_SOURCE_ROOT}/platform/sdl/crt_post_sdl.c"
     _crt_sdl_contents)
if(_crt_sdl_contents MATCHES "g_settings|#[ \t]*include[ \t]*[<\"](app/)?settings.h")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/platform/sdl/crt_post_sdl.c (player policy in adapter)")
endif()
if(_native_resource_violations)
    list(JOIN _native_resource_violations "\n  " _formatted)
    message(FATAL_ERROR
        "Persistent presentation resources regressed to SDL ownership:\n  "
        "${_formatted}\nUse ArRenderTexture and keep native access in the adapter bridge.")
endif()

# Shared presentation helpers are part of the game-side seam even while their
# implementation translation units still contain transitional SDL paths.
file(READ "${GAME_SOURCE_ROOT}/sim/sim3d/present_sim3d_internal.h"
     _sim3d_internal_contents)
if(_sim3d_internal_contents MATCHES
   "SDL_Texture[ 	]*\\*[ 	]*(EnsureSimUnderlayTexture|SimUnderlayBlurTexture)")
    message(FATAL_ERROR
        "SIM underlay helpers regressed to native texture handles; "
        "keep ArRenderTexture in the shared presentation contract.")
endif()
