#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "input_map.h"
#include "render_capabilities.h"
#include "settings.h"
#include "sim/sim3d_camera_limits.h"
#include "sim/sim_town_terrain.h"
#include "user_data_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool g_ws_active;
int g_ws_extra;
int g_ws_display_extra;
uint8 g_ram[0x20000];
/* kSettingCat_Graphics's GpuShadersActive() availability gate reads this
 * (main.c's real runtime state); this harness has no renderer, so it's
 * never actually true here. */
bool g_gpu_shaders_active;
/* W4-2: present.c owns the real value (latched when a renderer rejects the rim
 * mask blend mode); stubbed true here so the row's availability is exercised. */
static bool s_sim_rim_mask_supported = true;
bool Present_SimRimMaskSupported(void) {
  return s_sim_rim_mask_supported;
}
/* Simulation and action effects use SDL's built-in additive blend. As with
 * the rim mask, present.c latches an actual backend rejection; this
 * renderer-free harness supplies the optimistic initial capability. */
static bool s_effect_renderer_supported = true;
bool Present_EffectRendererSupported(void) {
  return s_effect_renderer_supported;
}
/* Host-side diorama geometry rebind; no renderer in this harness. */
void Diorama_OnModeChanged(void) {}
static int s_failures;
static int s_observer_calls;
static const SettingDesc *s_observer_desc;
static SettingChangeResult s_observer_result;
static int s_action_calls;
static const SettingDesc *s_action_desc;

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static void ChangeObserved(const SettingDesc *desc,
                           SettingChangeResult result) {
  s_observer_calls++;
  s_observer_desc = desc;
  s_observer_result = result;
}

static bool ActionObserved(const SettingDesc *desc) {
  s_action_calls++;
  s_action_desc = desc;
  return true;
}

static void ClearSettingsEnv(void) {
  Settings_ClearConfigLayer();
  for (int i = 0; i < g_setting_desc_count; i++) {
    if (g_setting_descs[i].env)
      unsetenv(g_setting_descs[i].env);
  }
}

static void TestDefaultsAndMetadata(void) {
  ClearSettingsEnv();
  memset(g_ram, 0, sizeof(g_ram));
  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 43;
  Settings_Init();

  /* Count moves whenever a category gains or loses rows; the SIM 3D block
   * most recently gained the world-map underlay and cloud-shroud stage
   * toggles with their haze/density/falloff/inset dials, on top of the nine
   * named stage toggles and the shadow/height-pop tuning rows. On top of
   * that, Controls added eight device/tuning rows plus 42 binding rows: 18
   * actions x keyboard+gamepad (12 SNES buttons and 6 analog camera axes),
   * plus 6 gamepad-only host actions. Presentation then gained the diorama
   * edge-margin-fix A/B toggle (SPEC-backdrop-clip.md), and navigation gained
   * its separate off-by-default 3D scene switch. The in-game manual then added
   * two: the action that opens the reader, and its spreads/single-page choice.
   * The content randomizer then added eleven: master, seed and reroll on Seed;
   * health, damage, type shuffle and its range on Enemies; drops and placement
   * on Items; lair positions and monsters on Simulation. The magic-spell
   * cycle then added three: the Cheats-tab toggle that arms it, plus its
   * keyboard and gamepad binding rows. The sim synthetic-part work added one
   * measured, gameplay-affecting actor-range row. Background voxel polish then
   * added five independent performance boundaries, the audited landscape one
   * player-facing magnitude row, and the Aitos wind event one Extras row for
   * whether it stills every windmill or only the ones the ROM stamped. The
   * host FPS overlay adds one Video row, and frame generation adds one explicit
   * 30-to-60 Hz validation cadence. The runtime-only render comparison adds
   * one keyboard and one gamepad binding row, but deliberately no persisted
   * mode setting. */
  CHECK(g_setting_desc_count == 274);
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *a = &g_setting_descs[i];
    CHECK(a->key && a->key[0] && a->label && a->tooltip);
    CHECK((a->type == kSettingType_Action) == (a->field == NULL));
    CHECK(Settings_Find(a->key) == a);
    for (int j = i + 1; j < g_setting_desc_count; j++) {
      CHECK(strcmp(a->key, g_setting_descs[j].key) != 0);
      if (a->field && g_setting_descs[j].field)
        CHECK(a->field != g_setting_descs[j].field);
    }
    char formatted[512];
    Settings_FormatValue(a, formatted, sizeof(formatted));
    if (a->type == kSettingType_Action) {
      CHECK(!strcmp(formatted, "RUN"));
      CHECK(Settings_SetText(a, formatted) == kSettingChange_Rejected);
    } else {
      CHECK(Settings_SetText(a, formatted) == kSettingChange_Unchanged);
    }
  }
  CHECK(g_settings.display_mode == kDisplayMode_WideFull);
  CHECK(g_settings.hud_scale_percent == 0);
  CHECK(g_settings.menu_scale_percent == 0);
  CHECK(g_settings.extended_aspect == 0);
  CHECK(g_settings.pixel_aspect == kPixelAspect_Crt43);
  CHECK(g_settings.window_scale == 3);
  CHECK(g_settings.window_mode == kWindowMode_Windowed &&
        g_settings.new_renderer);
  CHECK(g_settings.refresh_mode == kRefreshMode_Vsync);
  CHECK(g_settings.gpu_interp_source_rate == kInterpolationSource_Native);
  {
    const SettingDesc *source_rate =
        Settings_Find("gpu_interp_source_rate");
    CHECK(source_rate && !Settings_IsAvailable(source_rate));
    g_settings.diorama_mode = true;
    CHECK(!Settings_IsAvailable(source_rate));
    g_settings.gpu_interp_enabled = true;
    CHECK(Settings_IsAvailable(source_rate));
    g_settings.gpu_interp_enabled = false;
    g_settings.diorama_mode = false;
  }
  CHECK(!g_settings.show_fps);
  CHECK(!g_settings.ignore_aspect_ratio);
  CHECK(g_settings.audio_enabled);
  CHECK(g_settings.audio_frequency == kAudioFrequency_Auto);
  CHECK(Settings_AudioFrequencyHz() == 0);   /* 0 = device-native at open */
  CHECK(g_settings.audio_samples == 2048);
  CHECK(g_settings.audio_master_volume == 100);
  CHECK(g_settings.audio_music_volume == 100);
  CHECK(g_settings.audio_sfx_volume == 100);
  CHECK(g_settings.audio_dialog_blip);
  CHECK(g_settings.turbo_multiplier == 8);
  CHECK(g_settings.warp_target == 0x0101);
  CHECK(!g_settings.scene_inspector);
  CHECK(!g_settings.sim3d_mode);
  CHECK(g_settings.sim_view_range == 0);
  {
    const SettingDesc *range = Settings_Find("sim_view_range");
    CHECK(range && range->type == kSettingType_Int &&
          range->minval == 0 && range->maxval == 256 && range->step == 16 &&
          range->player_visible);
  }
  CHECK(!g_settings.sim3d_world_navigation);
  CHECK(g_settings.sim3d_world_navigation_lighting);
  CHECK(!g_settings.sim3d_world_navigation_clouds);
  /* The stage toggles are what the player's master switch resolves, so a
   * landed stage missing from these defaults is dead in normal play. They must
   * agree with kSim3DShippedFeatures; bump both as each visual gate passes. */
  CHECK(Settings_Sim3DRequestedFeatures() == kSim3DShippedFeatures);
  CHECK(g_settings.sim3d_separated_composite);
  CHECK(g_settings.sim3d_ground_projection);
  CHECK(g_settings.sim3d_voxel_preset ==
        kSimBackgroundVoxelPreset_Balanced);
  CHECK(g_settings.sim3d_voxel_detail == kSimBackgroundVoxelDetail_High);
  CHECK(g_settings.sim3d_voxel_lod == kSimBackgroundVoxelLod_Adaptive);
  CHECK(g_settings.sim3d_voxel_shading ==
        kSimBackgroundVoxelShading_MaterialAware);
  CHECK(g_settings.sim3d_voxel_style == kSimBackgroundVoxelStyle_Varied);
  CHECK(g_settings.sim3d_voxel_facing ==
        kSimBackgroundVoxelFacing_PerModel);
  CHECK(g_settings.sim3d_voxel_render_scale ==
        kSimBackgroundVoxelRenderScale_PixelClean);
  CHECK(g_settings.sim3d_object_billboards);
  CHECK(g_settings.sim3d_virtual_height);
  CHECK(g_settings.sim3d_shadows);
  CHECK(g_settings.sim3d_backdrop);
  CHECK(g_settings.sim3d_soft_shadows);
  CHECK(g_settings.sim3d_rim_light);
  CHECK(g_settings.sim3d_effect_lighting);
  CHECK(g_settings.sim3d_particles);
  CHECK(g_settings.action_effect_lighting);
  CHECK(g_settings.action_effect_particles);
  CHECK(!g_settings.sim3d_picker_exit_ease);
  CHECK(g_settings.sim3d_diagnostic_layers == 0);
  /* Camera baseline captured from a tuned session (2026-07-22), not derived:
   * see the note on the sim3d defaults in settings.c. Distance is now a real
   * pinned zoom rather than 0, which meant "auto-fit to the scene". */
  CHECK(g_settings.sim3d_tilt_x_mrad == -575);
  CHECK(g_settings.sim3d_tilt_y_mrad == 0);
  CHECK(g_settings.sim3d_distance_x100 == 300);
  CHECK(g_settings.sim3d_landscape_height_pct ==
        kSimTownTerrainLandscapeHeightDefaultPct);
  const SettingDesc *landscape_height =
      Settings_Find("sim3d_landscape_height_pct");
  CHECK(landscape_height &&
        landscape_height->category == kSettingCat_Simulation);
  CHECK(landscape_height &&
        landscape_height->minval ==
            kSimTownTerrainLandscapeHeightMinimumPct &&
        landscape_height->maxval ==
            kSimTownTerrainLandscapeHeightMaximumPct &&
        landscape_height->step ==
            kSimTownTerrainLandscapeHeightStepPct &&
        landscape_height->defval ==
            kSimTownTerrainLandscapeHeightDefaultPct);
#if AR_SIM3D_TERRAIN_ELEVATION
  CHECK(landscape_height && landscape_height->player_visible);
#else
  CHECK(landscape_height && !landscape_height->player_visible);
#endif
  const SettingDesc *sim_pitch = Settings_Find("sim3d_tilt_x_mrad");
  const SettingDesc *dynamic_pitch =
      Settings_Find("sim3d_dyncam_baseline_tilt_x_mrad");
  CHECK(sim_pitch &&
        sim_pitch->minval == kSim3DCameraPitchMinimumMrad &&
        sim_pitch->maxval == kSim3DCameraPitchMaximumMrad);
  CHECK(dynamic_pitch &&
        dynamic_pitch->minval == kSim3DCameraPitchMinimumMrad &&
        dynamic_pitch->maxval == kSim3DCameraPitchMaximumMrad);
  /* D3c ships the catalogue heights unscaled; 0 is a valid "ground every
   * billboard" tuning value, so it must not double as the default. */
  CHECK(g_settings.sim3d_height_scale_x100 == 100);
  const SettingDesc *height_scale = Settings_Find("sim3d_height_scale_x100");
  CHECK(height_scale && height_scale->category == kSettingCat_Simulation);
  CHECK(height_scale && height_scale->minval == 0 &&
        height_scale->maxval == 400 && height_scale->defval == 100);
  CHECK(g_settings.save_backend == 0);
  CHECK(!g_settings.save_edit_armed && g_settings.save_autobackup);
  CHECK(g_settings.save_editor_page == kSaveEditorPage_Actions);
  for (int i = 0; i < 6; i++)
    CHECK(g_settings.save_region_progress[i] == kSaveProgressEdit_LeaveAsIs);
  CHECK(g_settings.save_master_level == 0 &&
        g_settings.save_master_hp == 0 &&
        g_settings.save_master_mp == 0 &&
        g_settings.save_lives == 0 &&
        g_settings.save_angel_sp_current == 0 &&
        g_settings.save_angel_sp_max == 0 &&
        g_settings.save_angel_hp_current == 0 &&
        g_settings.save_angel_hp_max == 0 &&
        g_settings.save_message_speed == 0);
  CHECK(!g_settings.save_player_name[0]);
  CHECK(g_settings.save_professional_mode == 0 &&
        g_settings.save_death_heim_state == 0 &&
        g_settings.save_equipped_magic == 0);
  for (int i = 0; i < 4; i++) CHECK(g_settings.save_magic_slots[i] == 0);
  for (int i = 0; i < 8; i++) CHECK(g_settings.save_item_slots[i] == 0);
  for (int region = 0; region < 6; region++)
    for (int act = 0; act < 2; act++)
      CHECK(g_settings.save_scores[region][act] == 0);
  CHECK(g_settings.ws_action && g_settings.ws_sim && g_settings.ws_sprites);
  CHECK(g_settings.cheat_inf_mp == 0);
  CHECK(!g_settings.cheat_moonjump);
  CHECK(g_settings.cheat_moonjump_speed == 6);
  CHECK(Settings_VisibleX0() == 0);
  CHECK(Settings_VisibleWidth() == 342);

  const SettingDesc *display = Settings_Find("display_mode");
  const SettingDesc *hp = Settings_Find("cheat_inf_hp");
  const SettingDesc *volume = Settings_Find("audio_master_volume");
  const SettingDesc *music_volume = Settings_Find("audio_music_volume");
  const SettingDesc *sfx_volume = Settings_Find("audio_sfx_volume");
  const SettingDesc *warp = Settings_Find("warp_target");
  const SettingDesc *warp_action = Settings_Find("warp_now");
  const SettingDesc *save_state_action = Settings_Find("save_state");
  const SettingDesc *load_state_action = Settings_Find("load_state");
  const SettingDesc *pause_action = Settings_Find("toggle_pause");
  const SettingDesc *restart_action = Settings_Find("restart_game");
  const SettingDesc *exit_action = Settings_Find("exit_desktop");
  const SettingDesc *music = Settings_Find("music_replacements");
  const SettingDesc *frequency = Settings_Find("audio_frequency");
  const SettingDesc *screen_ratio = Settings_Find("extended_aspect");
  const SettingDesc *stretch = Settings_Find("ignore_aspect_ratio");
  const SettingDesc *legacy_bg_refresh = Settings_Find("ws_bgrefresh");
  const SettingDesc *bridge_limit = Settings_Find("fix_bridge_limit");
  const SettingDesc *save_backend = Settings_Find("save_backend");
  const SettingDesc *save_fillmore = Settings_Find("save_prog_fillmore");
  const SettingDesc *save_page = Settings_Find("save_editor_page");
  const SettingDesc *save_apply = Settings_Find("save_apply_persist");
  const SettingDesc *inspector = Settings_Find("scene_inspector");
  const SettingDesc *dump_assets = Settings_Find("dump_scene_assets");
  const SettingDesc *sim_mode = Settings_Find("sim3d_mode");
  const SettingDesc *world_navigation =
      Settings_Find("sim3d_world_navigation");
  const SettingDesc *world_navigation_lighting =
      Settings_Find("sim3d_world_navigation_lighting");
  const SettingDesc *world_navigation_clouds =
      Settings_Find("sim3d_world_navigation_clouds");
  const SettingDesc *sim_reset = Settings_Find("sim3d_reset_camera");
  CHECK(display && display->type == kSettingType_Enum);
  CHECK(display && display->enum_count == kDisplayMode_PresetCount);
  CHECK(display && display->apply == kApply_Callback);
  CHECK(volume && volume->category == kSettingCat_Audio);
  CHECK(volume && volume->apply == kApply_Callback);
  CHECK(volume && volume->minval == 0 && volume->maxval == 100 &&
        volume->step == 5);
  CHECK(music_volume && music_volume->category == kSettingCat_Audio &&
        music_volume->apply == kApply_Callback &&
        music_volume->minval == 0 && music_volume->maxval == 100 &&
        music_volume->step == 5);
  CHECK(sfx_volume && sfx_volume->category == kSettingCat_Audio &&
        sfx_volume->apply == kApply_Callback &&
        sfx_volume->minval == 0 && sfx_volume->maxval == 100 &&
        sfx_volume->step == 5);
  CHECK(warp && warp->type == kSettingType_Custom);
  CHECK(!Settings_IsMenuVisible(warp));
  CHECK(!Settings_IsMenuVisible(warp_action));
  CHECK(!Settings_IsMenuVisible(save_state_action));
  CHECK(!Settings_IsMenuVisible(load_state_action));
  CHECK(pause_action && pause_action->type == kSettingType_Action &&
        pause_action->apply == kApply_Action);
  CHECK(restart_action && restart_action->type == kSettingType_Action);
  CHECK(exit_action && exit_action->type == kSettingType_Action);
  CHECK(music && music->apply == kApply_Callback);
  CHECK(frequency && frequency->type == kSettingType_Enum &&
        frequency->enum_count == kAudioFrequency_Count &&
        frequency->apply == kApply_Restart);
  CHECK(screen_ratio && screen_ratio->category == kSettingCat_Display);
  CHECK(stretch && stretch->category == kSettingCat_Display);
  CHECK(!Settings_IsMenuVisible(stretch));
  CHECK(legacy_bg_refresh &&
        legacy_bg_refresh->category == kSettingCat_Widescreen);
  CHECK(!Settings_IsMenuVisible(legacy_bg_refresh));
  CHECK(!Settings_IsMenuVisible(Settings_Find("uncapped_framerate")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_picker_exit_ease")));
  /* Screen ratio > Stretch derives the ignore-aspect field the runtime reads. */
  CHECK(Settings_SetLong(screen_ratio, kScreenAspect_Stretch) ==
        kSettingChange_Applied);
  CHECK(g_settings.ignore_aspect_ratio);
  CHECK(Settings_SetLong(screen_ratio, kScreenAspect_169) ==
        kSettingChange_Applied);
  CHECK(!g_settings.ignore_aspect_ratio);
  /* Transient host-display status is not part of the settings layer:
   * refresh_mode formats as its plain persisted enum label. */
  const SettingDesc *refresh = Settings_Find("refresh_mode");
  CHECK(refresh && refresh->type == kSettingType_Enum);
  CHECK(!strcmp(refresh->enum_labels[kRefreshMode_Uncapped], "Uncapped"));
  CHECK(!strcmp(refresh->enum_labels[kRefreshMode_Unlimited], "Unlimited"));
  char refresh_value[32];
  Settings_FormatValue(refresh, refresh_value, sizeof(refresh_value));
  CHECK(!strcmp(refresh_value, "Vsync"));
  CHECK(bridge_limit && bridge_limit->category == kSettingCat_Enhancements);
  CHECK(inspector && inspector->category == kSettingCat_Inspector);
  CHECK(dump_assets && dump_assets->category == kSettingCat_Inspector &&
        dump_assets->type == kSettingType_Action);
  CHECK(sim_mode && sim_mode->category == kSettingCat_Simulation);
  CHECK(world_navigation &&
        world_navigation->category == kSettingCat_Simulation &&
        world_navigation->type == kSettingType_Bool);
  CHECK(world_navigation_lighting && world_navigation_clouds);
  CHECK(!Settings_IsAvailable(world_navigation_lighting));
  CHECK(!Settings_IsAvailable(world_navigation_clouds));
  /* Camera rows (and the reset that restores them) are their own tab of the
   * Town 3D section, separate from the stage toggles. */
  CHECK(sim_reset && sim_reset->category == kSettingCat_SimCamera &&
        sim_reset->type == kSettingType_Action);
  CHECK(Settings_CategoryIsSim3D(kSettingCat_SimCamera) &&
        Settings_CategoryIsSim3D(kSettingCat_Simulation) &&
        !Settings_CategoryIsSim3D(kSettingCat_Display));

  /* Debug classification: the 3D numeric dials, internal A/B toggles, the SIM
   * diagnostic mask, and the inspector are developer-only; master toggles,
   * major on/off effects, camera mode, and unrelated settings are not. The
   * switch that reveals them is itself never hidden. */
  CHECK(Settings_IsDebugOnly(Settings_Find("sim3d_tilt_x_mrad")));
  CHECK(Settings_IsDebugOnly(Settings_Find("sim3d_shadow_opacity_pct")));
#if AR_SIM3D_TERRAIN_ELEVATION
  CHECK(!Settings_IsDebugOnly(
      Settings_Find("sim3d_landscape_height_pct")));
#else
  CHECK(Settings_IsDebugOnly(
      Settings_Find("sim3d_landscape_height_pct")));
#endif
  CHECK(Settings_IsDebugOnly(Settings_Find("sim3d_diagnostic_layers")));
  CHECK(Settings_IsDebugOnly(Settings_Find("sim3d_separated_composite")));
  CHECK(Settings_IsDebugOnly(Settings_Find("diorama_depth_shade")));
  CHECK(Settings_IsDebugOnly(Settings_Find("diorama_layer_bg2")));
  CHECK(Settings_IsDebugOnly(Settings_Find("scene_inspector")));
  CHECK(Settings_IsDebugOnly(Settings_Find("dump_scene_assets")));
  CHECK(!Settings_IsDebugOnly(Settings_Find("sim3d_mode")));
  CHECK(!Settings_IsDebugOnly(Settings_Find("sim_view_range")));
  CHECK(!Settings_IsDebugOnly(Settings_Find("sim3d_world_navigation")));
  CHECK(!Settings_IsDebugOnly(
      Settings_Find("sim3d_world_navigation_lighting")));
  CHECK(!Settings_IsDebugOnly(
      Settings_Find("sim3d_world_navigation_clouds")));
  CHECK(!Settings_IsDebugOnly(Settings_Find("sim3d_shadows")));
  CHECK(!Settings_IsDebugOnly(Settings_Find("sim3d_camera_mode")));
  CHECK(!Settings_IsDebugOnly(Settings_Find("diorama_skybox")));
  CHECK(!Settings_IsDebugOnly(Settings_Find("show_debug_settings")));
  CHECK(!Settings_IsDebugOnly(Settings_Find("audio_master_volume")));
  CHECK(!Settings_IsDebugOnly(Settings_Find("hud_scale_percent")));
  CHECK(save_backend && save_backend->category == kSettingCat_Save &&
        save_backend->apply == kApply_Restart);
  CHECK(save_fillmore && save_fillmore->apply == kApply_Save &&
        save_fillmore->enum_count == kSaveProgressEdit_Count);
  CHECK(save_page && save_page->enum_count == kSaveEditorPage_Count &&
        save_page->apply == kApply_Passive);
  /* Payload rows are paged; the backend/apply controls live only on Actions. */
  g_settings.save_editor_page = kSaveEditorPage_Progress;
  CHECK(Settings_IsMenuVisible(save_fillmore));
  CHECK(!Settings_IsMenuVisible(Settings_Find("save_master_level")));
  CHECK(!Settings_IsMenuVisible(save_backend));
  g_settings.save_editor_page = kSaveEditorPage_Actions;
  CHECK(Settings_IsMenuVisible(save_backend));
  CHECK(!Settings_IsMenuVisible(save_fillmore));
  CHECK(save_apply && save_apply->type == kSettingType_Action &&
        save_apply->category == kSettingCat_Save);
  CHECK(hp && Settings_IsAvailable(hp));
  for (int i = 0; i < g_setting_desc_count; i++)
    if (g_setting_descs[i].category == kSettingCat_Cheats)
      CHECK(Settings_IsAvailable(&g_setting_descs[i]));
  g_ram[0x18] = 1;
  CHECK(hp && Settings_IsAvailable(hp));
  CHECK(!strcmp(Settings_CategoryName(kSettingCat_Widescreen), "Widescreen"));
  CHECK(!strcmp(Settings_CategoryName(kSettingCat_Simulation), "Simulation"));
  CHECK(!strcmp(Settings_CategoryName(kSettingCat_Extras), "Tools"));
  CHECK(!strcmp(Settings_CategoryName(kSettingCat_Inspector), "Inspector"));
  CHECK(!strcmp(Settings_ApplyKindName(kApply_Restart), "Restart required"));
  CHECK(!strcmp(Settings_ApplyKindName(kApply_Save), "Staged save edit"));
  CHECK(!strcmp(Settings_ChangeResultName(kSettingChange_Applied), "applied"));
  Settings_SetActionObserver(ActionObserved);
  s_action_calls = 0;
  CHECK(Settings_InvokeAction(pause_action));
  CHECK(s_action_calls == 1 && s_action_desc == pause_action);
  CHECK(Settings_InvokeAction(restart_action));
  CHECK(s_action_calls == 2 && s_action_desc == restart_action);
  CHECK(Settings_InvokeAction(exit_action));
  CHECK(s_action_calls == 3 && s_action_desc == exit_action);
  Settings_SetActionObserver(NULL);
}

static void TestSim3DEnvironmentLabels(void) {
  ClearSettingsEnv();
  setenv("AR_SIM3D", "on", 1);
  setenv("AR_SIM3D_WORLD_NAV", "on", 1);
  setenv("AR_SIM3D_WORLD_NAV_LIGHTING", "off", 1);
  setenv("AR_SIM3D_WORLD_NAV_CLOUDS", "on", 1);
  setenv("AR_SIM3D_SHADOWS", "off", 1);
  setenv("AR_SIM3D_HEIGHT", "off", 1);
  setenv("AR_SIM3D_PITCH", "350", 1);
  Settings_Init();
  CHECK(g_settings.sim3d_mode);
  CHECK(g_settings.sim3d_world_navigation);
  CHECK(!g_settings.sim3d_world_navigation_lighting);
  CHECK(g_settings.sim3d_world_navigation_clouds);
  CHECK(Settings_IsAvailable(
      Settings_Find("sim3d_world_navigation_lighting")));
  CHECK(Settings_IsAvailable(
      Settings_Find("sim3d_world_navigation_clouds")));
  /* Shared atmosphere rows must remain usable when navigation is the only
   * 3D master. Town-only cull shape controls stay unavailable. */
  g_settings.sim3d_mode = false;
  CHECK(Settings_IsAvailable(Settings_Find("sim3d_backdrop")));
  CHECK(Settings_IsAvailable(
      Settings_Find("sim3d_backdrop_strength_pct")));
  CHECK(Settings_IsAvailable(Settings_Find("sim3d_cull_haze")));
  CHECK(Settings_IsAvailable(
      Settings_Find("sim3d_underlay_haze_pct")));
  CHECK(Settings_IsAvailable(
      Settings_Find("sim3d_cull_haze_lead_px")));
  CHECK(Settings_IsAvailable(
      Settings_Find("sim3d_underlay_defocus_pct")));
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_cull_dim_pct")));
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_cull_corner_px")));
  g_settings.sim3d_mode = true;
  /* Turning stages off by name is the only way to select a profile now, and
   * the fold must drop exactly those bits. */
  CHECK(!g_settings.sim3d_shadows && !g_settings.sim3d_virtual_height);
  CHECK(Settings_Sim3DRequestedFeatures() ==
        (kSimFeature_SeparatedComposite | kSimFeature_GroundProjection |
         kSimFeature_ObjectBillboards | kSimFeature_SoftShadows |
         kSimFeature_RimLight | kSimFeature_WorldUnderlay |
         kSimFeature_CloudShroud | kSimFeature_CullHaze |
         kSimFeature_Backdrop | kSimFeature_EffectLighting |
         kSimFeature_Particles));
  CHECK(g_settings.sim3d_tilt_x_mrad ==
        kSim3DCameraPitchMaximumMrad);
  ClearSettingsEnv();
}

static bool WriteTextFile(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (!file) return false;
  bool ok = fputs(text, file) >= 0;
  if (fclose(file) != 0) ok = false;
  return ok;
}

static bool FileContains(const char *path, const char *needle) {
  FILE *file = fopen(path, "r");
  if (!file) return false;
  char buffer[16384];
  size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
  buffer[size] = 0;
  fclose(file);
  return strstr(buffer, needle) != NULL;
}

static void TestConfigSettingsEnvironmentPrecedence(void) {
  static const char config_path[] = "actraiser-settings-config-test.ini";
  static const char settings_path[] = "actraiser-settings-layer-test.ini";
  static const char saved_path[] = "actraiser-settings-saved-test.ini";
  remove(config_path);
  remove(settings_path);
  remove(saved_path);
  remove("actraiser-settings-saved-test.ini.tmp");

  ClearSettingsEnv();
  CHECK(WriteTextFile(config_path,
      "[Graphics]\n"
      "WindowScale = 4\n"
      "Fullscreen = 1\n"
      "ExtendedAspectRatio = 16:9\n"
      "AspectPAR = 4:3\n"
      "[Sound]\n"
      "AudioFreq = 32040\n"
      "AR_AUDIO_VOLUME = 65\n"
      "[Cheats]\n"
      "AR_DISPLAY_MODE = 2\n"
      "AR_WS_SPRITES = 1\n"
      "[KeyMap]\n"
      "Fullscreen = Alt+Return\n"));
  ParseConfigFile(config_path);

  CHECK(WriteTextFile(settings_path,
      "# menu-owned layer\n"
      "window_scale = 5\n"
      "audio_master_volume = 70%\n"
      "audio_music_volume = 60%\n"
      "audio_sfx_volume = 75%\n"
      "extended_aspect = 16:10\n"
      "pixel_aspect = Square pixels\n"
      "ws_sprites = On\n"
      "cheat_moonjump_speed = 9\n"
      "cheat_moonjump_button = $4000\n"
      "unknown_future_key = retained-by-future-version\n"));

  /* Real environment values must remain distinguishable from config.ini's
   * staged AR_* compatibility values and win over both file layers. */
  setenv("AR_AUDIO_VOLUME", "85", 1);
  setenv("AR_MUSIC_VOLUME", "55", 1);
  setenv("AR_WS_SPRITES", "0", 1);
  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 52;
  Settings_InitWithFile(settings_path);
  Settings_FinalizeDisplayMode();

  CHECK(g_settings.window_scale == 5);       /* settings > config */
  /* Legacy [Graphics] Fullscreen=1 maps to Borderless; [KeyMap] Fullscreen did
   * not clobber it. */
  CHECK(g_settings.window_mode == kWindowMode_Borderless);
  CHECK(g_settings.audio_frequency == kAudioFrequency_32040);
  CHECK(Settings_AudioFrequencyHz() == 32040);
  CHECK(g_settings.audio_master_volume == 85); /* env > settings > config */
  CHECK(g_settings.audio_music_volume == 55);
  CHECK(g_settings.audio_sfx_volume == 75);
  CHECK(!g_settings.ws_sprites);
  CHECK(g_settings.display_mode == kDisplayMode_Custom);
  CHECK(Settings_ExtendedAspectX() == 16 && Settings_ExtendedAspectY() == 10);
  CHECK(g_settings.pixel_aspect == kPixelAspect_Square);
  CHECK(g_settings.cheat_moonjump);  /* migrated from the old speed-only row */
  CHECK(g_settings.cheat_moonjump_speed == 9);

  CHECK(Settings_SetLong(Settings_Find("window_scale"), 6) ==
        kSettingChange_Applied);
  CHECK(Settings_SetLong(Settings_Find("audio_master_volume"), 40) ==
        kSettingChange_Applied);
  CHECK(Settings_Save(saved_path));
  CHECK(FileContains(saved_path, "window_scale = 6"));
  CHECK(FileContains(saved_path, "audio_master_volume = 40%"));
  CHECK(FileContains(saved_path, "audio_music_volume = 55%"));
  CHECK(FileContains(saved_path, "audio_sfx_volume = 75%"));
  CHECK(FileContains(saved_path, "audio_frequency = 32.04 kHz"));
  CHECK(FileContains(saved_path, "turbo_multiplier = 8"));
  CHECK(FileContains(saved_path, "cheat_moonjump = On"));
  CHECK(FileContains(saved_path, "cheat_moonjump_speed = 9"));
  CHECK(!FileContains(saved_path, "cheat_moonjump_button"));
  CHECK(!FileContains(saved_path, "ignore_aspect_ratio ="));
  CHECK(!FileContains(saved_path, "uncapped_framerate ="));
  CHECK(!FileContains(saved_path, "sim3d_picker_exit_ease ="));
  CHECK(!FileContains(saved_path, "ws_bgrefresh ="));
  CHECK(FileContains(saved_path, "warp_target = 0101"));
  CHECK(FileContains(saved_path, "save_backend = native-srm"));
  CHECK(FileContains(saved_path, "save_prog_fillmore = Leave as-is"));
  CHECK(FileContains(saved_path, "save_master_level = Leave as-is"));
  CHECK(FileContains(saved_path, "save_player_name = Leave as-is"));
  CHECK(FileContains(saved_path, "save_score_northwall_2 = Leave as-is"));
  CHECK(!FileContains(saved_path, "toggle_pause ="));
  CHECK(!FileContains(saved_path, "display_mode ="));
  CHECK(!FileContains("actraiser-settings-saved-test.ini.tmp", "anything"));
  CHECK(Settings_SetLong(Settings_Find("audio_master_volume"), 45) ==
        kSettingChange_Applied);
  CHECK(Settings_Save(saved_path));  /* atomically replace an existing file */
  CHECK(FileContains(saved_path, "audio_master_volume = 45%"));

  ClearSettingsEnv();
  setenv("AR_WINDOW_MODE", "Windowed", 1);
  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 52;
  Settings_InitWithFile(saved_path);
  Settings_FinalizeDisplayMode();
  CHECK(g_settings.window_scale == 6);
  CHECK(g_settings.window_mode == kWindowMode_Windowed);
  CHECK(g_settings.audio_master_volume == 45);
  CHECK(g_settings.audio_frequency == kAudioFrequency_32040);
  CHECK(Settings_AudioFrequencyHz() == 32040);
  CHECK(!g_settings.ws_sprites);
  CHECK(g_settings.display_mode == kDisplayMode_Custom);
  CHECK(Settings_ExtendedAspectX() == 16 && Settings_ExtendedAspectY() == 10);

  remove(config_path);
  remove(settings_path);
  remove(saved_path);
}

static void TestLegacySeedEncodings(void) {
  ClearSettingsEnv();
  setenv("AR_INF_MP", "1", 1);
  setenv("AR_INF_HP", "0x20", 1); /* leading zero historically disables */
  setenv("AR_MOONJUMP", "9", 1);
  setenv("AR_NO_KNOCKBACK", "1", 1); /* now a plain on/off toggle */
  setenv("AR_PIN", "7E00210A,7F1234AA", 1);
  setenv("AR_WS_SPRITES", "0", 1);
  setenv("AR_AUDIO_VOLUME", "137", 1);
  setenv("AR_DIALOG_BLIP", "0", 1);
  setenv("AR_TURBO_MULT", "1", 1);
  setenv("AR_WARP", "0605", 1);
  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 43;
  Settings_Init();

  CHECK(g_settings.cheat_inf_mp == 10);
  CHECK(g_settings.cheat_inf_hp == 0);
  CHECK(g_settings.cheat_moonjump);
  CHECK(g_settings.cheat_moonjump_speed == 9);
  CHECK(g_settings.cheat_no_knockback);
  CHECK(g_settings.pin_count == 2);
  CHECK(g_settings.audio_master_volume == 100);
  CHECK(!g_settings.audio_dialog_blip);
  CHECK(g_settings.turbo_multiplier == 2);
  CHECK(g_settings.warp_target == 0x0605);
  CHECK(g_settings.pins[0].off == 0x0021 && g_settings.pins[0].val == 0x0a);
  CHECK(g_settings.pins[1].off == 0x11234 && g_settings.pins[1].val == 0xaa);
  CHECK(g_settings.display_mode == kDisplayMode_Custom);

  setenv("AR_DISPLAY_MODE", "1", 1);
  Settings_Init();
  CHECK(g_settings.display_mode == kDisplayMode_WideRaw);
  CHECK(g_settings.ws_action && g_settings.ws_sim);
  CHECK(!g_settings.ws_sprites);
}

static void TestMutationApi(void) {
  ClearSettingsEnv();
  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 43;
  Settings_Init();
  Settings_SetChangeObserver(ChangeObserved);
  s_observer_calls = 0;

  const SettingDesc *sprites = Settings_Find("ws_sprites");
  CHECK(Settings_SetLong(sprites, 0) == kSettingChange_Applied);
  CHECK(!g_settings.ws_sprites);
  CHECK(g_settings.display_mode == kDisplayMode_Custom);
  CHECK(s_observer_calls == 1 && s_observer_desc == sprites);
  CHECK(Settings_SetLong(sprites, 0) == kSettingChange_Unchanged);
  CHECK(s_observer_calls == 1);
  CHECK(Settings_SetLong(sprites, 1) == kSettingChange_Applied);
  CHECK(g_settings.display_mode == kDisplayMode_WideFull);

  const SettingDesc *hud_scale = Settings_Find("hud_scale_percent");
  CHECK(Settings_SetLong(hud_scale, 287) == kSettingChange_Applied);
  CHECK(g_settings.hud_scale_percent == 275);
  char hud_value[32];
  Settings_FormatValue(hud_scale, hud_value, sizeof(hud_value));
  CHECK(!strcmp(hud_value, "2.75x"));

  const SettingDesc *menu_scale = Settings_Find("menu_scale_percent");
  CHECK(menu_scale && menu_scale->maxval == 800);
  CHECK(Settings_SetLong(menu_scale, 162) == kSettingChange_Applied);
  CHECK(g_settings.menu_scale_percent == 150);
  Settings_FormatValue(menu_scale, hud_value, sizeof(hud_value));
  CHECK(!strcmp(hud_value, "1.50x"));
  CHECK(Settings_SetText(menu_scale, "Auto") == kSettingChange_Applied);
  CHECK(g_settings.menu_scale_percent == 0);

  const SettingDesc *volume = Settings_Find("audio_master_volume");
  const SettingDesc *music_volume = Settings_Find("audio_music_volume");
  const SettingDesc *sfx_volume = Settings_Find("audio_sfx_volume");
  CHECK(Settings_SetLong(volume, 87) == kSettingChange_Applied);
  CHECK(g_settings.audio_master_volume == 85);
  CHECK(s_observer_desc == volume);
  char volume_value[16];
  Settings_FormatValue(volume, volume_value, sizeof(volume_value));
  CHECK(!strcmp(volume_value, "85%"));
  CHECK(Settings_SetText(volume, "40%") == kSettingChange_Applied);
  CHECK(g_settings.audio_master_volume == 40);
  CHECK(Settings_SetLong(volume, -1) == kSettingChange_Applied);
  CHECK(g_settings.audio_master_volume == 0);
  CHECK(Settings_SetText(music_volume, "65%") == kSettingChange_Applied);
  CHECK(g_settings.audio_music_volume == 65);
  CHECK(Settings_SetLong(sfx_volume, 42) == kSettingChange_Applied);
  CHECK(g_settings.audio_sfx_volume == 40);

  const SettingDesc *dialog_blip = Settings_Find("audio_dialog_blip");
  CHECK(Settings_SetLong(dialog_blip, 0) == kSettingChange_Applied);
  CHECK(!g_settings.audio_dialog_blip);

  const SettingDesc *frequency = Settings_Find("audio_frequency");
  CHECK(Settings_SetText(frequency, "48 kHz") ==
        kSettingChange_RestartPending);
  CHECK(g_settings.audio_frequency == kAudioFrequency_48000);
  CHECK(Settings_AudioFrequencyHz() == 48000);
  CHECK(Settings_SetText(frequency, "32000") == kSettingChange_Rejected);
  CHECK(Settings_AudioFrequencyHz() == 48000);

  const SettingDesc *music = Settings_Find("music_replacements");
  CHECK(Settings_SetLong(music, 0) == kSettingChange_Applied);
  CHECK(!g_settings.music_replacements && s_observer_desc == music);

  char value[512];
  const SettingDesc *save_backend = Settings_Find("save_backend");
  CHECK(Settings_SetText(save_backend, "ini") ==
        kSettingChange_RestartPending);
  CHECK(g_settings.save_backend == 1);
  const SettingDesc *save_fillmore = Settings_Find("save_prog_fillmore");
  CHECK(Settings_SetText(save_fillmore, "act2-cleared") ==
        kSettingChange_Applied);
  CHECK(g_settings.save_region_progress[0] ==
        kSaveProgressEdit_Act2Cleared);
  Settings_FormatValue(save_fillmore, value, sizeof(value));
  CHECK(!strcmp(value, "Act 2 cleared"));
  const SettingDesc *save_level = Settings_Find("save_master_level");
  CHECK(Settings_SetText(save_level, "17") == kSettingChange_Applied);
  CHECK(g_settings.save_master_level == 17);
  Settings_FormatValue(save_level, value, sizeof(value));
  CHECK(!strcmp(value, "17"));
  const SettingDesc *save_mp = Settings_Find("save_master_mp");
  CHECK(Settings_SetText(save_mp, "0") == kSettingChange_Applied);
  CHECK(g_settings.save_master_mp == 1);
  Settings_FormatValue(save_mp, value, sizeof(value));
  CHECK(!strcmp(value, "0"));
  const SettingDesc *save_page = Settings_Find("save_editor_page");
  CHECK(Settings_SetText(save_page, "Status") == kSettingChange_Applied);
  CHECK(!Settings_IsMenuVisible(save_fillmore));
  CHECK(Settings_IsMenuVisible(save_level));
  CHECK(Settings_IsMenuVisible(Settings_Find("save_player_name")));
  const SettingDesc *save_name = Settings_Find("save_player_name");
  CHECK(Settings_SetText(save_name, "CODEX") == kSettingChange_Applied);
  CHECK(!strcmp(g_settings.save_player_name, "CODEX"));
  CHECK(Settings_SetText(save_name, "TOO-LONG-NAME") ==
        kSettingChange_Rejected);
  const SettingDesc *save_magic = Settings_Find("save_magic_slot_1");
  CHECK(Settings_SetLong(save_page, kSaveEditorPage_Magic) ==
        kSettingChange_Applied);
  CHECK(Settings_IsMenuVisible(save_magic));
  CHECK(!Settings_IsMenuVisible(save_level));
  CHECK(Settings_SetText(save_magic, "Magical Aura") ==
        kSettingChange_Applied);
  CHECK(g_settings.save_magic_slots[0] == 4);
  const SettingDesc *save_equipped = Settings_Find("save_equipped_magic");
  CHECK(Settings_SetText(save_equipped, "Magical Stardust") ==
        kSettingChange_Applied);
  CHECK(g_settings.save_equipped_magic == 3);
  const SettingDesc *save_item = Settings_Find("save_item_slot_8");
  CHECK(Settings_SetLong(save_page, kSaveEditorPage_Items) ==
        kSettingChange_Applied);
  CHECK(Settings_IsMenuVisible(save_item));
  CHECK(Settings_SetText(save_item, "Strength of Angel") ==
        kSettingChange_Applied);
  CHECK(g_settings.save_item_slots[7] == 14);
  const SettingDesc *save_score = Settings_Find("save_score_northwall_2");
  CHECK(Settings_SetLong(save_page, kSaveEditorPage_Scores) ==
        kSettingChange_Applied);
  CHECK(Settings_IsMenuVisible(save_score));
  CHECK(Settings_SetText(save_score, "12340") == kSettingChange_Applied);
  CHECK(g_settings.save_scores[5][1] == 1235);
  Settings_FormatValue(save_score, value, sizeof(value));
  CHECK(!strcmp(value, "12340"));
  CHECK(Settings_SetText(save_score, "12345") == kSettingChange_Rejected);

  const SettingDesc *turbo = Settings_Find("turbo_multiplier");
  CHECK(Settings_SetLong(turbo, 12) == kSettingChange_Applied);
  CHECK(g_settings.turbo_multiplier == 12);
  const SettingDesc *warp = Settings_Find("warp_target");
  CHECK(Settings_SetText(warp, "0303") == kSettingChange_Applied);
  CHECK(g_settings.warp_target == 0x0303);
  Settings_FormatValue(warp, value, sizeof(value));
  CHECK(!strcmp(value, "0303"));
  CHECK(Settings_SetText(warp, "garbage") == kSettingChange_Rejected);
  CHECK(g_settings.warp_target == 0x0303);

  const SettingDesc *renderer = Settings_Find("new_renderer");
  CHECK(Settings_SetLong(renderer, 0) == kSettingChange_Applied);
  CHECK(!g_settings.new_renderer);
  const SettingDesc *aspect = Settings_Find("extended_aspect");
  CHECK(Settings_SetText(aspect, "16:9") == kSettingChange_Applied);
  Settings_FormatValue(aspect, value, sizeof(value));
  CHECK(!strcmp(value, "16:9"));
  CHECK(Settings_SetText(aspect, "21:9") == kSettingChange_Rejected);
  const SettingDesc *pixel_aspect = Settings_Find("pixel_aspect");
  CHECK(Settings_SetText(pixel_aspect, "Square pixels") ==
        kSettingChange_Applied);
  const SettingDesc *window_scale = Settings_Find("window_scale");
  CHECK(Settings_SetLong(window_scale, 4) == kSettingChange_Applied);
  CHECK(g_settings.window_scale == 4);

  const SettingDesc *mp = Settings_Find("cheat_inf_mp");
  CHECK(Settings_SetLong(mp, 999) == kSettingChange_Applied);
  CHECK(g_settings.cheat_inf_mp == 255);
  Settings_FormatValue(mp, value, sizeof(value));
  CHECK(!strcmp(value, "255"));

  const SettingDesc *no_knockback = Settings_Find("cheat_no_knockback");
  /* Now a plain on/off toggle: any nonzero normalizes to 1 and formats "On". */
  CHECK(Settings_SetLong(no_knockback, 1) == kSettingChange_Applied);
  Settings_FormatValue(no_knockback, value, sizeof(value));
  CHECK(!strcmp(value, "On"));
  CHECK(Settings_SetText(no_knockback, value) == kSettingChange_Unchanged);

  const SettingDesc *freeze = Settings_Find("cheat_freeze_timer");
  Settings_FormatValue(freeze, value, sizeof(value));
  CHECK(!strcmp(value, "Off"));
  CHECK(Settings_SetText(freeze, value) == kSettingChange_Unchanged);

  const SettingDesc *magic = Settings_Find("cheat_all_magic");
  CHECK(Settings_SetLong(magic, 1) == kSettingChange_Applied);
  CHECK(Settings_SetLong(magic, 0) == kSettingChange_AppliedStickyDisable);
  CHECK(s_observer_result == kSettingChange_AppliedStickyDisable);

  const SettingDesc *pins = Settings_Find("pins");
  CHECK(Settings_SetText(pins, "7E00210A,7F1234AA") == kSettingChange_Applied);
  Settings_FormatValue(pins, value, sizeof(value));
  CHECK(!strcmp(value, "7E00210A,7F1234AA"));
  CHECK(Settings_Reset(pins) == kSettingChange_AppliedStickyDisable);
  CHECK(g_settings.pin_count == 0);

  const SettingDesc *display = Settings_Find("display_mode");
  CHECK(Settings_SetText(display, "Widescreen raw") == kSettingChange_Applied);
  CHECK(g_settings.display_mode == kDisplayMode_WideRaw);
  CHECK(g_settings.ws_action && !g_settings.ws_sprites);
  CHECK(Settings_Reset(display) == kSettingChange_Applied);
  CHECK(g_settings.display_mode == kDisplayMode_WideFull);
  CHECK(g_settings.ws_action && g_settings.ws_sprites);
}

static void TestCategoryReset(void) {
  ClearSettingsEnv();
  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 43;
  Settings_Init();
  Settings_SetChangeObserver(NULL);

  const SettingDesc *sim_mode = Settings_Find("sim3d_mode");
  const SettingDesc *sim_tilt = Settings_Find("sim3d_tilt_x_mrad");
  const SettingDesc *volume = Settings_Find("audio_master_volume");
  const SettingDesc *frequency = Settings_Find("audio_frequency");
  CHECK(sim_mode && sim_tilt && volume && frequency);
  CHECK(Settings_SetLong(sim_mode, 1) == kSettingChange_Applied);
  CHECK(Settings_SetLong(sim_tilt, sim_tilt->defval - sim_tilt->step) ==
        kSettingChange_Applied);
  CHECK(Settings_SetLong(volume, 55) == kSettingChange_Applied);
  CHECK(Settings_SetLong(frequency, kAudioFrequency_48000) ==
        kSettingChange_RestartPending);

  /* A registry category is narrow: resetting Town camera must not reset the
   * Town scene master toggle or an unrelated Audio setting. */
  CHECK(Settings_ResetCategory(kSettingCat_SimCamera) ==
        kSettingChange_Applied);
  CHECK(g_settings.sim3d_tilt_x_mrad == sim_tilt->defval);
  CHECK(g_settings.sim3d_mode);
  CHECK(g_settings.audio_master_volume == 55);
  CHECK(g_settings.audio_frequency == kAudioFrequency_48000);
  CHECK(Settings_ResetCategory(kSettingCat_SimCamera) ==
        kSettingChange_Unchanged);

  /* Batch results preserve the strongest consequence from any row. */
  CHECK(Settings_ResetCategory(kSettingCat_Audio) ==
        kSettingChange_RestartPending);
  CHECK(g_settings.audio_master_volume == volume->defval);
  CHECK(g_settings.audio_frequency == frequency->defval);
  CHECK(g_settings.sim3d_mode);
  CHECK(Settings_ResetCategory(kSettingCat_Count) ==
        kSettingChange_Rejected);
}

static void TestCheatsCanBeStagedOutsideTheirRuntimeMode(void) {
  ClearSettingsEnv();
  memset(g_ram, 0, sizeof(g_ram));
  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 43;
  Settings_Init();

  /* $18=00 is the simulation/title/UI family. Action-only effects must remain
   * editable here; the runtime action gate consumes them after transition. */
  CHECK(g_ram[0x18] == 0);
  const SettingDesc *hp = Settings_Find("cheat_inf_hp");
  const SettingDesc *freeze = Settings_Find("cheat_freeze_timer");
  const SettingDesc *moonjump = Settings_Find("cheat_moonjump");
  const SettingDesc *moonjump_speed =
      Settings_Find("cheat_moonjump_speed");
  const SettingDesc *no_knockback = Settings_Find("cheat_no_knockback");
  CHECK(Settings_IsAvailable(hp));
  CHECK(Settings_IsAvailable(freeze));
  CHECK(Settings_IsAvailable(moonjump));
  CHECK(Settings_IsAvailable(moonjump_speed));
  CHECK(Settings_Find("cheat_moonjump_button") == NULL);
  CHECK(Settings_IsAvailable(no_knockback));
  CHECK(Settings_SetLong(hp, 32) == kSettingChange_Applied);
  CHECK(Settings_SetLong(freeze, 1) == kSettingChange_Applied);
  CHECK(Settings_SetLong(moonjump, 1) == kSettingChange_Applied);
  CHECK(Settings_SetLong(moonjump_speed, 9) == kSettingChange_Applied);
  CHECK(Settings_SetLong(no_knockback, 1) == kSettingChange_Applied);

  g_ram[0x18] = 1;
  CHECK(g_settings.cheat_inf_hp == 32);
  CHECK(g_settings.cheat_freeze_timer);
  CHECK(g_settings.cheat_moonjump);
  CHECK(g_settings.cheat_moonjump_speed == 9);
  CHECK(g_settings.cheat_no_knockback == 1);
}

static void TestNoWideBudget(void) {
  ClearSettingsEnv();
  setenv("AR_DISPLAY_MODE", "2", 1);
  g_ws_active = false;
  g_ws_extra = g_ws_display_extra = 0;
  Settings_Init();
  CHECK(g_settings.display_mode == kDisplayMode_43);
  CHECK(Settings_SetLong(Settings_Find("display_mode"),
                         kDisplayMode_WideRaw) == kSettingChange_Unchanged);
  CHECK(Settings_VisibleX0() == 0);
  CHECK(Settings_VisibleWidth() == 256);
}


/* Input bindings: defaults reproduce the pre-rebinding hard-coded keyboard
 * layout, every row survives the ini text round trip, and claiming a control
 * that another row already owns steals it rather than double-binding. */
static void TestInputBindings(void) {
  ClearSettingsEnv();
  Settings_Init();

  const SettingDesc *key_b = Settings_Find("bind_key_b");
  const SettingDesc *key_a = Settings_Find("bind_key_a");
  const SettingDesc *pad_b = Settings_Find("bind_pad_b");
  const SettingDesc *pad_menu = Settings_Find("bind_pad_menu");
  CHECK(key_b && key_a && pad_b && pad_menu);
  CHECK(key_b->type == kSettingType_Binding);
  /* Host actions are gamepad-only; there is no keyboard twin. */
  CHECK(Settings_Find("bind_key_menu") == NULL);

  /* Magic cycle is the deliberate exception (input_map.h): a debug control,
   * not a route into or out of the menu, so it carries BOTH rows. It defaults
   * to keyboard M and to no pad button at all — the cheat must not ride a
   * button an ordinary session presses. Binding it is still inert until the
   * Cheats tab arms it, so the default row is safe to publish. */
  const SettingDesc *key_cycle = Settings_Find("bind_key_magic_cycle");
  const SettingDesc *pad_cycle = Settings_Find("bind_pad_magic_cycle");
  CHECK(key_cycle && pad_cycle);
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_MagicCycle] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_M, false));
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_MagicCycle] ==
        0);
  const SettingDesc *key_compare = Settings_Find("bind_key_render_compare");
  const SettingDesc *pad_compare = Settings_Find("bind_pad_render_compare");
  CHECK(key_compare && pad_compare);
  CHECK(g_settings.input_bind[kInputClass_Keyboard]
                             [kInputAction_RenderCompare] == 0);
  CHECK(g_settings.input_bind[kInputClass_Gamepad]
                             [kInputAction_RenderCompare] == 0);
  const SettingDesc *cycle_cheat = Settings_Find("cheat_magic_cycle");
  CHECK(cycle_cheat != NULL);
  CHECK(!g_settings.cheat_magic_cycle);

  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_B] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_Z, false));
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_B] ==
        INPUT_BIND_MAKE(kInputBind_PadButton, SDL_GAMEPAD_BUTTON_SOUTH,
                        false));
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_Menu] ==
        INPUT_BIND_MAKE(kInputBind_PadButton, SDL_GAMEPAD_BUTTON_LEFT_STICK,
                        false));

  char text[64];
  /* R7: keys persist as "Key <scancode> <name>" — the NUMBER is authoritative
   * because SDL scancode names are documented as platform-unstable ("Left GUI"
   * vs "Left Windows"; RETURN and RETURN2 both "Return"), which silently reset
   * bindings when a config moved between platforms. The name is a readability
   * tail the parser ignores. Not "#40": a space-preceded '#' would be eaten by
   * the ini inline-comment stripper. */
  Settings_FormatValue(key_b, text, sizeof(text));
  CHECK(!strcmp(text, "Key 29 Z"));
  /* The numeric field alone is enough, extra name text is ignored, and the
   * legacy name-only form still loads. All three land on the same binding. */
  const uint32 z_binding =
      INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_Z, false);
  uint32 parsed = 0;
  CHECK(InputMap_ParseBinding("Key 29", &parsed) && parsed == z_binding);
  CHECK(InputMap_ParseBinding("Key 29 Z", &parsed) && parsed == z_binding);
  CHECK(InputMap_ParseBinding("Key 29 whatever", &parsed) &&
        parsed == z_binding);
  CHECK(InputMap_ParseBinding("Key Z", &parsed) && parsed == z_binding);
  /* A scancode whose NAME is itself a digit must still resolve by name, not be
   * misread as a scancode number ("1" is SDL_SCANCODE_1 == 30, not scancode 1). */
  CHECK(InputMap_ParseBinding("Key 1", &parsed) &&
        parsed == INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_1, false));
  /* Out-of-range numbers are rejected rather than silently clamped. */
  CHECK(!InputMap_ParseBinding("Key 99999", &parsed));
  /* The menu shows the readable form, never the storage spelling. */
  InputMap_DescribeBinding(text, sizeof(text), z_binding);
  CHECK(!strcmp(text, "Key Z"));
  Settings_FormatValue(pad_b, text, sizeof(text));
  CHECK(!strcmp(text, "Pad A / south"));

  /* Rebinding B to the key A already holds must clear A, not duplicate it. */
  CHECK(InputMap_ApplyBinding(
            key_b, INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_X, false)) ==
        kSettingChange_Applied);
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_B] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_X, false));
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_A] == 0);
  Settings_FormatValue(key_a, text, sizeof(text));
  CHECK(!strcmp(text, "Unbound"));

  /* Reset restores that row's own default, not a shared zero. */
  CHECK(Settings_Reset(key_b) == kSettingChange_Applied);
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_B] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_Z, false));

  /* Axis bindings keep their direction across the text round trip. */
  const SettingDesc *pad_up = Settings_Find("bind_pad_up");
  CHECK(InputMap_ApplyBinding(
            pad_up, INPUT_BIND_MAKE(kInputBind_PadAxis,
                                    SDL_GAMEPAD_AXIS_LEFTY, true)) ==
        kSettingChange_Applied);
  Settings_FormatValue(pad_up, text, sizeof(text));
  CHECK(!strcmp(text, "Pad L-Stick Up"));
  CHECK(Settings_SetText(pad_up, text) == kSettingChange_Unchanged);

  /* A held trigger remains held inside the press/release hysteresis band.
   * This is the exact sequence hold-to-comparison uses: the initial edge
   * crosses 20000, natural trigger settling must not turn it into a click,
   * and only crossing back below 12000 releases it. */
  const uint32 positive_axis = INPUT_BIND_MAKE(
      kInputBind_PadAxis, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, false);
  CHECK(InputMap_AxisBindingHeld(positive_axis, 21000, false));
  CHECK(InputMap_AxisBindingHeld(positive_axis, 16000, true));
  CHECK(!InputMap_AxisBindingHeld(positive_axis, 11000, true));

  /* Camera actions default to the right stick (signed, sharing one axis) and
   * the triggers, and are analog rather than edge-dispatched. */
  CHECK(INPUT_ACTION_IS_ANALOG(kInputAction_CamYawLeft));
  CHECK(INPUT_ACTION_IS_ANALOG(kInputAction_CamZoomOut));
  CHECK(!INPUT_ACTION_IS_ANALOG(kInputAction_CamReset));
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_CamYawLeft] ==
        INPUT_BIND_MAKE(kInputBind_PadAxis, SDL_GAMEPAD_AXIS_RIGHTX, true));
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_CamYawRight] ==
        INPUT_BIND_MAKE(kInputBind_PadAxis, SDL_GAMEPAD_AXIS_RIGHTX, false));
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_CamPitchUp] ==
        INPUT_BIND_MAKE(kInputBind_PadAxis, SDL_GAMEPAD_AXIS_RIGHTY, true));
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_CamZoomIn] ==
        INPUT_BIND_MAKE(kInputBind_PadAxis, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                        false));
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_CamReset] ==
        INPUT_BIND_MAKE(kInputBind_PadButton, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
                        false));
  /* Camera keys are unbound by default: the desktop path is the mouse. */
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_CamYawLeft] ==
        0);
  /* Two rows legitimately share the right-stick X axis with opposite signs;
   * the duplicate-steal pass must not treat them as a collision. */
  const SettingDesc *yaw_left = Settings_Find("bind_pad_cam_yaw_left");
  CHECK(yaw_left);
  Settings_FormatValue(yaw_left, text, sizeof(text));
  CHECK(!strcmp(text, "Pad R-Stick Left"));
  CHECK(Settings_SetText(yaw_left, text) == kSettingChange_Unchanged);
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_CamYawRight] ==
        INPUT_BIND_MAKE(kInputBind_PadAxis, SDL_GAMEPAD_AXIS_RIGHTX, false));

  /* The whole table survives a save/load cycle. */
  const char *path = "actraiser-settings-bindings-test.ini";
  CHECK(Settings_Save(path));
  uint32 saved[kSettingsInputClasses][kSettingsInputActions];
  memcpy(saved, g_settings.input_bind, sizeof(saved));
  Settings_Init();
  CHECK(memcmp(saved, g_settings.input_bind, sizeof(saved)) != 0);
  CHECK(Settings_Load(path));
  CHECK(memcmp(saved, g_settings.input_bind, sizeof(saved)) == 0);
  remove(path);
}

/* R6: a pinned scale percentage is defined in source-pixels-per-OUTPUT-pixel,
 * and SDL_WINDOW_HIGH_PIXEL_DENSITY makes the output PHYSICAL pixels — so the
 * pinned value must be multiplied by the display's pixel density to keep its
 * apparent size. Auto (0) must pass through untouched: those paths derive from
 * the output size and are already density-correct. */
static void TestScalePercentToOutput(void) {
  Settings_SetHostPixelDensity(1.0f);
  CHECK(Settings_HostPixelDensity() == 1.0f);
  CHECK(Settings_ScalePercentToOutput(100) == 100);
  CHECK(Settings_ScalePercentToOutput(250) == 250);

  Settings_SetHostPixelDensity(2.0f);          /* Retina */
  CHECK(Settings_ScalePercentToOutput(100) == 200);
  CHECK(Settings_ScalePercentToOutput(250) == 500);
  CHECK(Settings_ScalePercentToOutput(0) == 0);        /* auto untouched */
  CHECK(Settings_ScalePercentToOutput(-1) == -1);      /* sentinel untouched */

  Settings_SetHostPixelDensity(1.5f);          /* Wayland fractional */
  CHECK(Settings_ScalePercentToOutput(100) == 150);
  CHECK(Settings_ScalePercentToOutput(75) == 113);     /* rounds, never 0 */

  /* A bogus density can never zero out or invert a pinned scale. */
  Settings_SetHostPixelDensity(0.0f);
  CHECK(Settings_HostPixelDensity() == 1.0f);
  CHECK(Settings_ScalePercentToOutput(100) == 100);
  Settings_SetHostPixelDensity(-3.0f);
  CHECK(Settings_HostPixelDensity() == 1.0f);
  CHECK(Settings_ScalePercentToOutput(1) == 1);
  Settings_SetHostPixelDensity(1.0f);          /* restore for later tests */
}

/* Portable data stays relative to the working root. A packaged game anchors
 * that root beside its executable; a developer run keeps its launch cwd. */
static void TestUserDataFile(void) {
  char buf[1024];
  UserDataFile(buf, sizeof buf, "settings.ini");
  CHECK(strcmp(buf, "settings.ini") == 0);

  char srm[1024];
  UserDataFile(srm, sizeof srm, "saves/save.srm");
  CHECK(strcmp(srm, "saves/save.srm") == 0);
}

/* W4-2: the "Rim light" row must disappear when the renderer cannot honour the
 * custom blend mode the effect needs, rather than offering a toggle that does
 * nothing. present.c owns the real flag and latches it on the first failed
 * SDL_SetTextureBlendMode; this drives the availability predicate directly. */
static void TestRimLightAvailabilityFollowsBlendSupport(void) {
  const SettingDesc *rim = Settings_Find("sim3d_rim_light");
  CHECK(rim != NULL);
  if (!rim) return;

  const bool restore_support = s_sim_rim_mask_supported;
  const int restore_mode = g_settings.sim3d_mode;
  const bool restore_separated = g_settings.sim3d_separated_composite;
  const bool restore_billboards = g_settings.sim3d_object_billboards;

  /* The row is gated on BOTH the sim-3D stage being enabled and the blend mode
   * being usable, so enable the stage first — otherwise this would pass for the
   * wrong reason (unavailable either way) and prove nothing. */
  g_settings.sim3d_mode = 1;
  g_settings.sim3d_separated_composite = true;
  g_settings.sim3d_object_billboards = true;

  s_sim_rim_mask_supported = true;
  CHECK(Settings_IsAvailable(rim));

  /* Unsupported blend mode alone must remove the row. */
  s_sim_rim_mask_supported = false;
  CHECK(!Settings_IsAvailable(rim));

  /* Restoring support brings it back — so the flag is demonstrably what decides
   * it, not some unrelated precondition. */
  s_sim_rim_mask_supported = true;
  CHECK(Settings_IsAvailable(rim));

  /* The stage gate still dominates: no blend support in the world makes an
   * unimplemented stage available. */
  g_settings.sim3d_mode = 0;
  CHECK(!Settings_IsAvailable(rim));

  s_sim_rim_mask_supported = restore_support;
  g_settings.sim3d_mode = restore_mode;
  g_settings.sim3d_separated_composite = restore_separated;
  g_settings.sim3d_object_billboards = restore_billboards;
}

static void TestEffectAvailabilityFollowsRendererSupport(void) {
  const SettingDesc *lighting = Settings_Find("sim3d_effect_lighting");
  const SettingDesc *particles = Settings_Find("sim3d_particles");
  const SettingDesc *action_lighting =
      Settings_Find("action_effect_lighting");
  const SettingDesc *action_particles =
      Settings_Find("action_effect_particles");
  CHECK(lighting != NULL && particles != NULL &&
        action_lighting != NULL && action_particles != NULL);
  if (!lighting || !particles || !action_lighting || !action_particles) return;
  const bool restore_support = s_effect_renderer_supported;
  const int restore_mode = g_settings.sim3d_mode;
  const bool restore_separated = g_settings.sim3d_separated_composite;
  const bool restore_ground = g_settings.sim3d_ground_projection;
  g_settings.sim3d_mode = 1;
  g_settings.sim3d_separated_composite = true;
  g_settings.sim3d_ground_projection = true;
  s_effect_renderer_supported = true;
  CHECK(Settings_IsAvailable(lighting));
  CHECK(Settings_IsAvailable(particles));
  CHECK(Settings_IsAvailable(action_lighting));
  CHECK(Settings_IsAvailable(action_particles));
  s_effect_renderer_supported = false;
  CHECK(!Settings_IsAvailable(lighting));
  CHECK(!Settings_IsAvailable(particles));
  CHECK(!Settings_IsAvailable(action_lighting));
  CHECK(!Settings_IsAvailable(action_particles));
  s_effect_renderer_supported = restore_support;
  g_settings.sim3d_mode = restore_mode;
  g_settings.sim3d_separated_composite = restore_separated;
  g_settings.sim3d_ground_projection = restore_ground;
}

static void TestVideoSettingAudit(void) {
  const char *legacy_path = "settings-video-legacy-test.ini";
  const char *saved_path = "settings-video-saved-test.ini";
  ClearSettingsEnv();
  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 43;
  Settings_Init();

  const SettingDesc *display = Settings_Find("display_mode");
  const SettingDesc *window_scale = Settings_Find("window_scale");
  const SettingDesc *hd = Settings_Find("hd_replacements");
  const SettingDesc *stretch = Settings_Find("ignore_aspect_ratio");
  const SettingDesc *uncapped = Settings_Find("uncapped_framerate");
  const SettingDesc *show_fps = Settings_Find("show_fps");
  CHECK(display && window_scale && hd && stretch && uncapped && show_fps);
  CHECK(Settings_IsMenuVisible(show_fps));
  CHECK(Settings_IsAvailable(show_fps));

  CHECK(Settings_IsAvailable(display));
  g_ws_active = false;
  CHECK(!Settings_IsAvailable(display));
  g_ws_active = true;

  g_settings.window_mode = kWindowMode_Windowed;
  CHECK(Settings_IsAvailable(window_scale));
  g_settings.window_mode = kWindowMode_Borderless;
  CHECK(!Settings_IsAvailable(window_scale));
  g_settings.window_mode = kWindowMode_Windowed;

  Settings_SetHdReplacementsAvailable(false);
  CHECK(!Settings_IsAvailable(hd));
  Settings_SetHdReplacementsAvailable(true);
  CHECK(Settings_IsAvailable(hd));
  Settings_SetHdReplacementsAvailable(false);

  /* Rebuilding wide geometry must not reset the correction profile. */
  Settings_SetDisplayMode(kDisplayMode_WideRaw);
  const bool raw_sprites = g_settings.ws_sprites;
  Settings_ReconcileDisplayModeAfterGeometryChange(kDisplayMode_WideRaw);
  CHECK(g_settings.display_mode == kDisplayMode_WideRaw);
  CHECK(g_settings.ws_sprites == raw_sprites);
  g_settings.ws_sprites = true;
  g_settings.display_mode = kDisplayMode_Custom;
  Settings_ReconcileDisplayModeAfterGeometryChange(kDisplayMode_Custom);
  CHECK(g_settings.display_mode == kDisplayMode_Custom);
  CHECK(g_settings.ws_sprites);
  g_ws_active = false;
  Settings_ReconcileDisplayModeAfterGeometryChange(kDisplayMode_Custom);
  CHECK(g_settings.display_mode == kDisplayMode_43);
  CHECK(!g_settings.ws_action && !g_settings.ws_sprites);
  g_ws_active = true;
  Settings_ReconcileDisplayModeAfterGeometryChange(kDisplayMode_43);
  CHECK(g_settings.display_mode == kDisplayMode_WideFull);
  CHECK(g_settings.ws_action && g_settings.ws_sprites);

  /* Camera pose rows only affect their own active camera. */
  g_settings.diorama_mode = true;
  g_settings.diorama_camera_mode = kDioramaCam_Free;
  CHECK(Settings_IsAvailable(Settings_Find("diorama_tilt_x_mrad")));
  CHECK(!Settings_IsAvailable(
      Settings_Find("diorama_dyncam_baseline_tilt_x_mrad")));
  CHECK(!Settings_IsAvailable(Settings_Find("diorama_reactive_strength")));
  g_settings.diorama_camera_mode = kDioramaCam_Dynamic;
  CHECK(!Settings_IsAvailable(Settings_Find("diorama_tilt_x_mrad")));
  CHECK(Settings_IsAvailable(
      Settings_Find("diorama_dyncam_baseline_tilt_x_mrad")));
  CHECK(Settings_IsAvailable(Settings_Find("diorama_reactive_strength")));

  /* Town 3D availability mirrors the resolver's parent dependencies. */
  g_settings.sim3d_mode = true;
  g_settings.sim3d_separated_composite = true;
  g_settings.sim3d_ground_projection = true;
  g_settings.sim3d_object_billboards = true;
  g_settings.sim3d_virtual_height = true;
  g_settings.sim3d_shadows = true;
  g_settings.sim3d_soft_shadows = true;
  g_settings.sim3d_world_underlay = true;
  g_settings.sim3d_cloud_shroud = true;
  g_settings.sim3d_cull_haze = true;
  g_settings.sim3d_camera_mode = kSimCam_Dynamic;
#if AR_SIM3D_TERRAIN_ELEVATION
  CHECK(Settings_IsAvailable(
      Settings_Find("sim3d_landscape_height_pct")));
#else
  CHECK(!Settings_IsAvailable(
      Settings_Find("sim3d_landscape_height_pct")));
#endif
  CHECK(Settings_IsAvailable(Settings_Find("sim3d_reactive_strength")));
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_tilt_x_mrad")));
  CHECK(Settings_IsAvailable(Settings_Find("sim3d_soft_shadows")));
  CHECK(Settings_IsAvailable(Settings_Find("sim3d_cloud_falloff_px")));

  g_settings.sim3d_camera_mode = kSimCam_Free;
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_reactive_strength")));
  CHECK(Settings_IsAvailable(Settings_Find("sim3d_tilt_x_mrad")));
  g_settings.sim3d_object_billboards = false;
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_virtual_height")));
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_shadows")));
  g_settings.sim3d_object_billboards = true;
  g_settings.sim3d_shadows = false;
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_soft_shadows")));
  g_settings.sim3d_shadows = true;
  g_settings.sim3d_world_underlay = false;
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_cloud_shroud")));
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_cull_haze_pct")));
  g_settings.sim3d_world_navigation = true;
  CHECK(Settings_IsAvailable(Settings_Find("sim3d_cull_haze")));
  CHECK(Settings_IsAvailable(Settings_Find("sim3d_underlay_haze_pct")));
  g_settings.sim3d_separated_composite = false;
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_ground_projection")));
  CHECK(!Settings_IsAvailable(Settings_Find("sim3d_camera_mode")));

  /* Old generated files carried both keys. False aliases must not override a
   * modern ratio/mode, while true aliases still migrate to the replacement. */
  CHECK(WriteTextFile(
      legacy_path,
      "extended_aspect = 16:10\n"
      "ignore_aspect_ratio = Off\n"
      "refresh_mode = Limit\n"
      "uncapped_framerate = Off\n"));
  Settings_InitWithFile(legacy_path);
  CHECK(g_settings.extended_aspect == kScreenAspect_1610);
  CHECK(!Settings_IgnoreAspectRatio());
  CHECK(g_settings.refresh_mode == kRefreshMode_Limit);

  CHECK(WriteTextFile(
      legacy_path,
      "ignore_aspect_ratio = On\n"
      "uncapped_framerate = On\n"));
  Settings_InitWithFile(legacy_path);
  CHECK(g_settings.extended_aspect == kScreenAspect_Stretch);
  CHECK(Settings_IgnoreAspectRatio());
  CHECK(g_settings.refresh_mode == kRefreshMode_Uncapped);
  g_settings.show_fps = true;
  CHECK(Settings_Save(saved_path));
  CHECK(!FileContains(saved_path, "ignore_aspect_ratio ="));
  CHECK(!FileContains(saved_path, "uncapped_framerate ="));
  CHECK(FileContains(saved_path, "show_fps = On"));
  Settings_InitWithFile(saved_path);
  CHECK(g_settings.show_fps);

  CHECK(WriteTextFile(
      legacy_path,
      "refresh_mode = Uncapped\n"
      "show_fps = On\n"));
  Settings_InitWithFile(legacy_path);
  CHECK(g_settings.refresh_mode == kRefreshMode_Uncapped);
  CHECK(g_settings.show_fps);

  CHECK(WriteTextFile(
      legacy_path,
      "refresh_mode = Unlimited\n"));
  Settings_InitWithFile(legacy_path);
  CHECK(g_settings.refresh_mode == kRefreshMode_Unlimited);

  remove(legacy_path);
  remove(saved_path);
}

/* A diagnostic run must not mutate the player's configuration. Settings were not
 * covered by the replay protection that already refuses to persist SRAM, and the
 * gap was real: with Dynamic Cam the diorama camera drifts its own baseline
 * during a long replay and the dirty-flush wrote that drift into settings.ini
 * (observed 2026-07-27, diorama_dyncam_baseline_distance_x100 294 -> 330). */
static void TestPersistenceCanBeDisabled(void) {
  const char *path = "settings_persistence_probe.ini";
  remove(path);

  /* Disabled: reports success (the caller's intent is satisfied) but writes
   * nothing at all — not even an empty or temporary file. */
  Settings_SetPersistenceEnabled(false);
  CHECK(Settings_Save(path));
  FILE *probe = fopen(path, "r");
  CHECK(probe == NULL);
  if (probe) fclose(probe);

  /* Re-enabled: saving works exactly as before, so the guard cannot strand a
   * normal run with unsaveable settings. */
  Settings_SetPersistenceEnabled(true);
  CHECK(Settings_Save(path));
  probe = fopen(path, "r");
  CHECK(probe != NULL);
  if (probe) fclose(probe);
  remove(path);
}

int main(void) {
  TestPersistenceCanBeDisabled();
  TestUserDataFile();
  TestRimLightAvailabilityFollowsBlendSupport();
  TestEffectAvailabilityFollowsRendererSupport();
  TestScalePercentToOutput();
  TestDefaultsAndMetadata();
  TestVideoSettingAudit();
  TestSim3DEnvironmentLabels();
  TestConfigSettingsEnvironmentPrecedence();
  TestLegacySeedEncodings();
  TestMutationApi();
  TestCategoryReset();
  TestCheatsCanBeStagedOutsideTheirRuntimeMode();
  TestNoWideBudget();
  TestInputBindings();
  ClearSettingsEnv();
  Settings_SetChangeObserver(NULL);
  Settings_SetActionObserver(NULL);
  if (s_failures) {
    fprintf(stderr, "settings tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "settings tests: pass\n");
  return 0;
}
