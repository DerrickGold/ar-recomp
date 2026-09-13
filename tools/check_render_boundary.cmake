if(NOT DEFINED GAME_SOURCE_ROOT)
    message(FATAL_ERROR "GAME_SOURCE_ROOT is required")
endif()

# The portable localization core is held to the same rule as src/render: pack
# loading, contracts, sessions, grapheme handling and the rasterizer/backend
# contracts must not name an SDL type. Desktop enumeration lives in
# src/platform/sdl/pack_discovery_sdl.c, not here.
file(GLOB_RECURSE _portable_render_files
    "${GAME_SOURCE_ROOT}/performance*.c"
    "${GAME_SOURCE_ROOT}/performance*.h"
    "${GAME_SOURCE_ROOT}/present_world_nav*.c"
    "${GAME_SOURCE_ROOT}/present_world_nav*.h"
    "${GAME_SOURCE_ROOT}/present_sky_palace*.[ch]"
    "${GAME_SOURCE_ROOT}/presentation_view*.[ch]"
    "${GAME_SOURCE_ROOT}/render_preparation*.[ch]"
    "${GAME_SOURCE_ROOT}/render_capabilities.h"
    "${GAME_SOURCE_ROOT}/render/*.c"
    "${GAME_SOURCE_ROOT}/render/*.h"
    "${GAME_SOURCE_ROOT}/localization/*.c"
    "${GAME_SOURCE_ROOT}/localization/*.h"
    # Navigation owns game geometry/art, not backend resources. Include the
    # entire helper families so a newly added file cannot bypass this check.
    "${GAME_SOURCE_ROOT}/sim/sim_world_navigation_*.c"
    "${GAME_SOURCE_ROOT}/sim/sim_world_navigation_*.h"
    "${GAME_SOURCE_ROOT}/sim/sim_town_ground_art.c"
    "${GAME_SOURCE_ROOT}/sim/sim_town_ground_art.h"
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxel_*.c"
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxel_*.h")
list(APPEND _portable_render_files
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxels.c"
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxels.h"
    "${GAME_SOURCE_ROOT}/presentation_upload_mirror.c"
    "${GAME_SOURCE_ROOT}/presentation_upload_mirror.h"
    "${GAME_SOURCE_ROOT}/hd_replacement_host.c"
    "${GAME_SOURCE_ROOT}/hd_replacement_host.h"
    "${GAME_SOURCE_ROOT}/hd_replacements.h"
    "${GAME_SOURCE_ROOT}/crt_post.h"
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
    "${GAME_SOURCE_ROOT}/diorama/diorama_performance.c"
    "${GAME_SOURCE_ROOT}/diorama/diorama_performance.h"
    "${GAME_SOURCE_ROOT}/host/host_clock.h"
    "${GAME_SOURCE_ROOT}/host/parallel_work.h"
    "${GAME_SOURCE_ROOT}/dev/dev_tools.c"
    "${GAME_SOURCE_ROOT}/dev/dev_tools.h"
    "${GAME_SOURCE_ROOT}/dev/dev_tools_readback.h"
    "${GAME_SOURCE_ROOT}/frame_slot.c"
    "${GAME_SOURCE_ROOT}/frame_slot.h"
    "${GAME_SOURCE_ROOT}/present.h"
    "${GAME_SOURCE_ROOT}/present.c"
    "${GAME_SOURCE_ROOT}/present_frame.c"
    "${GAME_SOURCE_ROOT}/present_internal.h"
    "${GAME_SOURCE_ROOT}/present_world_nav.c"
    "${GAME_SOURCE_ROOT}/present_sim3d.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_internal.h"
    "${GAME_SOURCE_ROOT}/present_sim3d_environment.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_clouds.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_clouds.h"
    "${GAME_SOURCE_ROOT}/present_sim3d_underlay.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_underlay.h"
    "${GAME_SOURCE_ROOT}/present_sim3d_effects.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_effects.h"
    "${GAME_SOURCE_ROOT}/present_sim3d_project.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_project.h"
    "${GAME_SOURCE_ROOT}/present_sim3d_shadows.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_shadows.h"
    "${GAME_SOURCE_ROOT}/present_sim3d_terrain.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_terrain.h"
    "${GAME_SOURCE_ROOT}/settings_overlay_render.h"
    # Action effect construction is game-side geometry generation. Keep its
    # public contract and pure batch builder portable even while the diorama
    # projection adapter still calls the native compositor implementation.
    "${GAME_SOURCE_ROOT}/action/action_effect_render.c"
    "${GAME_SOURCE_ROOT}/action/action_effect_render.h"
    "${GAME_SOURCE_ROOT}/action/action_effect_projection.h"
    "${GAME_SOURCE_ROOT}/sim/sim_backdrop_render.c"
    "${GAME_SOURCE_ROOT}/sim/sim_backdrop_render.h"
    # The enhanced SIM scene's depth contract is game-side; its current GPU
    # implementation belongs to the SDL platform adapter.
    "${GAME_SOURCE_ROOT}/sim/sim3d_depth_pass.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d_mesh_set.h"
    "${GAME_SOURCE_ROOT}/sim/sim3d_mesh_set.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d_camera.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d_performance.c"
    "${GAME_SOURCE_ROOT}/sim/sim3d_performance.h"
    "${GAME_SOURCE_ROOT}/sim/sim_shadow_effect_backend.h"
    "${GAME_SOURCE_ROOT}/sim/sim_cloud_effect_backend.h"
    "${GAME_SOURCE_ROOT}/sim/sim_background_mountain_render.c"
    "${GAME_SOURCE_ROOT}/sim/sim_background_mountain_render.h"
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxel_renderer.c"
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxel_renderer.h"
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxel_project.c"
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxel_project.h"
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxel_terrain_depth.c"
    "${GAME_SOURCE_ROOT}/sim/sim_background_voxel_terrain_depth.h"
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
file(GLOB _font_resource_files
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
file(GLOB_RECURSE _game_neutral_files
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
file(GLOB_RECURSE _localization_files
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
file(GLOB_RECURSE _game_source_files
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
    "${GAME_SOURCE_ROOT}/present.c"
    "${GAME_SOURCE_ROOT}/present_sim3d.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_internal.h"
    "${GAME_SOURCE_ROOT}/present_sim3d_effects.c"
    "${GAME_SOURCE_ROOT}/present_sim3d_shadows.c")
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
   "SDL_GPU(Shader|RenderState|Device)|SDL_SetGPURenderState")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/diorama/diorama.c (native effect state)")
endif()
if(_diorama_contents MATCHES
   "SDL_Texture[ \t]*\\*[ \t]*(art_texture|target_texture|g_diorama_ss_texture)")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/diorama/diorama.c (ROM skybox cache)")
endif()
if(_diorama_contents MATCHES
   "SDL_(Texture|Vertex|FColor|FPoint)|SDL_RenderGeometry|SDL_(Get|Set)TextureBlendMode|SDL_SetRenderTextureAddressMode|ArSdlRenderBackend_(Borrow|Unwrap)Texture")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/diorama/diorama.c (native mesh/resource submission)")
endif()

file(READ "${GAME_SOURCE_ROOT}/present_sim3d_shadows.c"
     _sim_shadow_contents)
if(_sim_shadow_contents MATCHES
   "SDL_GPU(Shader|RenderState|Device)|SDL_SetGPURenderState")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/present_sim3d_shadows.c (native effect state)")
endif()

# Fullscreen post-processing exposes only semantic parameters and opaque
# textures. Native shader/target ownership belongs to the selected adapter;
# player settings remain frame-orchestration policy.
if(EXISTS "${GAME_SOURCE_ROOT}/crt_post.c")
    list(APPEND _native_resource_violations
        "${GAME_SOURCE_ROOT}/crt_post.c (CRT implementation outside adapter)")
endif()
file(READ "${GAME_SOURCE_ROOT}/platform/sdl/crt_post_sdl.c"
     _crt_sdl_contents)
if(_crt_sdl_contents MATCHES "g_settings|#[ \t]*include[ \t]*[<\"]settings.h")
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
file(READ "${GAME_SOURCE_ROOT}/present_sim3d_internal.h"
     _sim3d_internal_contents)
if(_sim3d_internal_contents MATCHES
   "SDL_Texture[ 	]*\\*[ 	]*(EnsureSimUnderlayTexture|SimUnderlayBlurTexture)")
    message(FATAL_ERROR
        "SIM underlay helpers regressed to native texture handles; "
        "keep ArRenderTexture in the shared presentation contract.")
endif()
