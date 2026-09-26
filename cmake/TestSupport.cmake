# Shared implementations for ROM-free tests. Each target keeps its harness,
# focused stubs and per-test compile definitions. Archives pull only the object
# files it actually needs; shader/font variants remain on their original targets.
# These shared sources do not consume per-test AR_ definitions (including through
# their headers). See tests/README.md before adding a source with build variants.

function(ar_test_support name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${name} PUBLIC snesrecomp::sdk)
endfunction()

ar_test_support(actraiser_performance_test_support
    src/app/performance_metrics.c)

ar_test_support(actraiser_render_test_support
    src/render/render_device.c)

ar_test_support(actraiser_render_sdl_test_support
    src/platform/sdl/render_sdl.c)

ar_test_support(actraiser_scene_math_test_support
    src/render/scene3d_math.c)

ar_test_support(actraiser_text_test_support
    src/localization/unicode_grapheme.c
    src/localization/text_backend.c
    src/localization/text_rasterizer.c
    src/localization/language_pack.c)

ar_test_support(actraiser_rom_decode_test_support
    src/actraiser/quintet_lzss.c)

ar_test_support(actraiser_campaign_test_support
    src/randomizer/randomizer_config.c
    src/host/campaign_identity.c
    src/save/save_checkpoint.c)

ar_test_support(actraiser_town_model_test_support
    src/sim/town/sim_town_layout.c
    src/sim/voxels/sim_background_voxels.c
    src/sim/town/sim_structure_visuals.c
    src/sim/voxels/sim_background_voxel_models.c
    src/sim/voxels/sim_background_voxel_landmarks.c
    src/sim/mountains/sim_background_mountains.c
    src/sim/mountains/sim_background_mountain_silhouette.c
    src/sim/mountains/sim_background_mountain_relief.c
    src/sim/voxels/sim_background_voxel_region.c
    src/sim/town/sim_town_canvas.c
    src/sim/town/sim_town_terrain.c)

target_link_libraries(actraiser_render_test_support PUBLIC actraiser_math)
target_link_libraries(actraiser_render_sdl_test_support PUBLIC
    actraiser_render_test_support actraiser_performance_test_support SDL3::SDL3)
target_link_libraries(actraiser_scene_math_test_support PUBLIC actraiser_math)
target_link_libraries(actraiser_text_test_support PUBLIC ar_text_template)
target_link_libraries(actraiser_town_model_test_support PUBLIC actraiser_math)

# The production HLE fatal path escapes through the game coroutine. Unit
# targets link only the formatter/trampoline core; invalid direct calls
# retain abort semantics unless a focused test registers its own escape.
add_library(actraiser_hle_fatal_test_support STATIC
    src/actraiser/actraiser_hle_fatal.c)
target_include_directories(actraiser_hle_fatal_test_support PUBLIC
    ${CMAKE_SOURCE_DIR}/src)

# HLE tests share CPU entry/stack/flag helpers and their fatal-path dependency.
# Static archives let focused tests provide their own narrow stubs without
# pulling unrelated helpers into the link. Neither source has per-test flags.
add_library(actraiser_cpu_hle_test_support STATIC
    src/actraiser/actraiser_cpu_hle_internal.c)
target_include_directories(actraiser_cpu_hle_test_support PUBLIC
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(actraiser_cpu_hle_test_support PUBLIC
    snesrecomp::sdk actraiser_hle_fatal_test_support)

# Tests using the real save store share its complete dependency closure.
# Narrow adapters can still substitute SaveSystem functions by omitting this
# target, as save_editor_test does. Production uses the shipping manifest.
add_library(actraiser_save_test_support STATIC
    src/save/save_system.c src/save/save_paths.c
    src/host/atomic_replace.c)
target_include_directories(actraiser_save_test_support PUBLIC
    ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(actraiser_save_test_support PUBLIC actraiser_text_test_support)

# Portable frame snapshots, including the cell and rasterizer contracts.
ar_test_support(actraiser_localization_frame_test_support
    src/localization/localization_frame.c
    src/localization/enhanced_text_settings.c
    src/localization/text_cell_record.c)
target_link_libraries(actraiser_localization_frame_test_support PUBLIC actraiser_text_test_support)
