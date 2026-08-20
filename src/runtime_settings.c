#include "runtime_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "actraiser_rtl.h"
#include "actraiser/actraiser_action_bg.h"
#include "diorama/diorama.h"
#include "frame_slot.h"
#include "host/host_audio.h"
#include "dev/host_dev_tools.h"
#include "randomizer.h"
#include "host/host_display.h"
#include "host/host_input.h"
#include "manual/manual_reader.h"
#include "music_replacements.h"
#include "save_system.h"
#include "settings_overlay.h"
#include "user_data_dir.h"

static RuntimeLifecycleRequest s_lifecycle_request;

extern SDL_Window *g_window;
extern SDL_Renderer *g_renderer;
extern bool g_new_ppu;
extern bool g_ws_active;

bool RuntimeSettings_BuildSaveEditRequest(SaveEditRequest *edits) {
  static const int kRegionStates[kSaveProgressEdit_Count] = {
    -1,
    kSaveRegionState_Act1,
    kSaveRegionState_Act1Cleared,
    kSaveRegionState_Act2,
    kSaveRegionState_Act2Cleared,
  };
  static const int kItemValues[] = {
    -1, 0x00, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0d, 0x0e, 0x0f, 0x12, 0x13, 0x14,
  };
  if (!edits) return false;

  SaveEditRequest_Clear(edits);
  bool staged = false;
  for (int region = 0; region < kActRaiserSaveRegionCount; region++) {
    const int selector = g_settings.save_region_progress[region];
    if (selector < 0 || selector >= kSaveProgressEdit_Count) continue;
    edits->region_state[region] = kRegionStates[selector];
    staged = staged || edits->region_state[region] >= 0;
  }

#define SAVE_STAGE_DIRECT(request_field, setting_field) do { \
  edits->request_field = g_settings.setting_field > 0 \
      ? g_settings.setting_field : -1; \
  staged = staged || edits->request_field >= 0; \
} while (0)
#define SAVE_STAGE_ZERO(request_field, setting_field) do { \
  edits->request_field = g_settings.setting_field > 0 \
      ? g_settings.setting_field - 1 : -1; \
  staged = staged || edits->request_field >= 0; \
} while (0)
  SAVE_STAGE_DIRECT(master_level, save_master_level);
  SAVE_STAGE_DIRECT(master_hp, save_master_hp);
  SAVE_STAGE_ZERO(master_mp, save_master_mp);
  SAVE_STAGE_DIRECT(lives, save_lives);
  SAVE_STAGE_ZERO(angel_sp_current, save_angel_sp_current);
  SAVE_STAGE_ZERO(angel_sp_max, save_angel_sp_max);
  SAVE_STAGE_ZERO(angel_hp_current, save_angel_hp_current);
  SAVE_STAGE_DIRECT(angel_hp_max, save_angel_hp_max);
  SAVE_STAGE_ZERO(message_speed, save_message_speed);
#undef SAVE_STAGE_DIRECT
#undef SAVE_STAGE_ZERO

  if (g_settings.save_player_name[0]) {
    edits->player_name_set = true;
    snprintf(edits->player_name, sizeof(edits->player_name), "%s",
             g_settings.save_player_name);
    staged = true;
  }
  if (g_settings.save_professional_mode > 0) {
    edits->professional_mode = g_settings.save_professional_mode - 1;
    staged = true;
  }
  if (g_settings.save_death_heim_state > 0) {
    static const int kDeathHeimStates[] = { -1, 0, 1, 4 };
    const int selector = g_settings.save_death_heim_state;
    if (selector < (int)(sizeof(kDeathHeimStates) /
                         sizeof(kDeathHeimStates[0]))) {
      edits->death_heim_state = kDeathHeimStates[selector];
      staged = true;
    }
  }
  if (g_settings.save_equipped_magic > 0) {
    edits->equipped_magic = g_settings.save_equipped_magic - 1;
    staged = true;
  }
  for (int slot = 0; slot < kActRaiserSaveMagicSlotCount; slot++) {
    if (g_settings.save_magic_slots[slot] <= 0) continue;
    edits->magic_slots[slot] = g_settings.save_magic_slots[slot] - 1;
    staged = true;
  }
  for (int slot = 0; slot < kActRaiserSaveItemSlotCount; slot++) {
    const int selector = g_settings.save_item_slots[slot];
    if (selector <= 0 ||
        selector >= (int)(sizeof(kItemValues) / sizeof(kItemValues[0]))) {
      continue;
    }
    edits->item_slots[slot] = kItemValues[selector];
    staged = true;
  }
  for (int region = 0; region < kActRaiserSaveRegionCount; region++) {
    for (int act = 0; act < kActRaiserSaveActCount; act++) {
      const int selector = g_settings.save_scores[region][act];
      if (selector <= 0) continue;
      edits->scores[region][act] = (selector - 1) * 10;
      staged = true;
    }
  }
  return staged;
}

bool RuntimeSettings_HandleAction(const SettingDesc *desc) {
  if (!desc || !desc->key) return false;

  if (!strcmp(desc->key, "toggle_pause")) {
    HostInput_TogglePause();
  } else if (!strcmp(desc->key, "toggle_turbo")) {
    HostInput_ToggleTurbo();
  } else if (!strcmp(desc->key, "save_state")) {
    RtlSaveLoad(kSaveLoad_Save, 0);
    fprintf(stderr, "State saved.\n");
  } else if (!strcmp(desc->key, "load_state")) {
    RtlSaveLoad(kSaveLoad_Load, 0);
    FrameSlot_ResetActionEffects();
    ActRaiserActionBg_Reset();
    HostDisplay_InvalidatePresentHistory();
    fprintf(stderr, "State loaded.\n");
  } else if (!strcmp(desc->key, "warp_now")) {
    extern void ActRaiser_Warp(unsigned region, unsigned map);
    const unsigned target = (unsigned)g_settings.warp_target;
    ActRaiser_Warp((target >> 8) & 0xff, target & 0xff);
  } else if (!strcmp(desc->key, "take_snapshot")) {
    HostDevTools_TakeFullSnapshot();
  } else if (!strcmp(desc->key, "manual_open")) {
    /* Returns false when there is no manual to show, which the overlay reports
     * as a failed action rather than opening onto an empty reader. */
    if (!ManualReader_Open()) return false;
  } else if (!strcmp(desc->key, "rando_reroll")) {
    if (!Randomizer_IsAvailable()) return false;
    Randomizer_Reroll();
  } else if (!strcmp(desc->key, "diorama_reset")) {
    Diorama_ResetCamera();
  } else if (!strcmp(desc->key, "sim3d_reset_camera")) {
    HostInput_ResetSim3DCamera();
  } else if (!strcmp(desc->key, "dump_scene_assets")) {
    if (!HostDevTools_DumpSceneAssets()) return false;
  } else if (!strcmp(desc->key, "save_apply_session") ||
             !strcmp(desc->key, "save_apply_persist")) {
    SaveEditRequest edits;
    RuntimeSettings_BuildSaveEditRequest(&edits);
    SaveError error = {{0}};
    const bool persist = !strcmp(desc->key, "save_apply_persist");
    if (!SaveSystem_ApplyEdits(
            &edits, g_settings.save_edit_armed, persist,
            g_settings.save_autobackup, &error)) {
      fprintf(stderr, "[save-editor] %s failed: %s\n",
              persist ? "apply and save" : "session apply", error.message);
      return false;
    }
    fprintf(stderr, "[save-editor] staged save edits applied%s\n",
            persist ? " and saved" : " for this session");
  } else if (!strcmp(desc->key, "save_import")) {
    const char *path = getenv("AR_SAVE_IMPORT");
    if (!path || !path[0]) {
      FILE *probe = fopen("saves/import.srm", "rb");
      if (probe) {
        fclose(probe);
        path = "saves/import.srm";
      } else {
        path = "saves/import.ini";
      }
    }
    SaveError error = {{0}};
    if (!SaveSystem_Import(path, g_settings.save_autobackup, &error)) {
      fprintf(stderr, "[save-editor] import %s failed: %s\n", path,
              error.message);
      return false;
    }
    fprintf(stderr, "[save-editor] imported %s -> %s\n", path,
            SaveSystem_ActivePath());
  } else if (!strcmp(desc->key, "save_export_srm") ||
             !strcmp(desc->key, "save_export_ini")) {
    const bool ini = !strcmp(desc->key, "save_export_ini");
    const char *path = ini ? "saves/export.ini" : "saves/export.srm";
    SaveError error = {{0}};
    if (!SaveSystem_Export(ini ? kSaveFileFormat_Ini
                               : kSaveFileFormat_NativeSrm,
                           path, &error)) {
      fprintf(stderr, "[save-editor] export %s failed: %s\n", path,
              error.message);
      return false;
    }
    fprintf(stderr, "[save-editor] export -> %s\n", path);
  } else if (!strcmp(desc->key, "restart_game") ||
             !strcmp(desc->key, "exit_desktop")) {
    char settings_path[kHostPathCapacity];
    UserDataFile(settings_path, sizeof(settings_path), "settings.ini");
    if (!Settings_Save(settings_path)) {
      fprintf(stderr, "[lifecycle] could not save settings.ini\n");
      return false;
    }
    s_lifecycle_request = !strcmp(desc->key, "restart_game")
        ? kRuntimeLifecycle_Restart
        : kRuntimeLifecycle_Exit;
    SettingsOverlay_Close();
    fprintf(stderr, "[lifecycle] %s requested\n",
            s_lifecycle_request == kRuntimeLifecycle_Restart
                ? "restart" : "exit");
  } else {
    return false;
  }
  return true;
}

static void OnRuntimeSettingChanged(const SettingDesc *desc,
                                    SettingChangeResult result) {
  (void)result;

  if (desc->field == &g_settings.audio_master_volume)
    HostAudio_SetMasterVolumePercent(g_settings.audio_master_volume);
  if (desc->field == &g_settings.audio_enabled)
    HostAudio_SetEnabled(g_settings.audio_enabled);
  if (desc->field == &g_settings.music_replacements)
    MusicReplacements_ApplySetting();
  if (desc->field == &g_settings.scene_inspector &&
      !g_settings.scene_inspector)
    HostInput_CloseInspectorSelection();
  if (desc->field == &g_settings.window_mode && g_window) {
    HostDisplay_ApplyWindowMode();
    HostDisplay_UpdateProperties();
    HostDisplay_ApplyWindowScale();
  }
  if ((desc->field == &g_settings.refresh_mode ||
       desc->field == &g_settings.uncapped_framerate) && g_renderer)
    HostDisplay_ApplyRefreshVsync();
  if (desc->field == &g_settings.extended_aspect ||
      desc->field == &g_settings.pixel_aspect ||
      desc->field == &g_settings.ignore_aspect_ratio) {
    HostDisplay_ResolveVideoGeometry(true);
    HostInput_RequestPausedRedraw();
    return;
  }
  if (desc->field == &g_settings.diorama_tilt_x_mrad ||
      desc->field == &g_settings.diorama_tilt_y_mrad ||
      desc->field == &g_settings.diorama_distance_x100)
    Diorama_SeedCameraFromSettings();
  if (desc->field == &g_settings.new_renderer) {
    g_new_ppu = g_settings.new_renderer || g_ws_active;
    HostInput_RequestPausedRedraw();
  }
  if (desc->field == &g_settings.window_scale)
    HostDisplay_ApplyWindowScale();
  else if (desc->field == &g_settings.display_mode ||
           desc->category == kSettingCat_Widescreen)
    HostDisplay_ApplyWindowScale();
  if (desc->category == kSettingCat_Display ||
      desc->category == kSettingCat_Widescreen ||
      Settings_CategoryIsSim3D(desc->category))
    HostInput_RequestPausedRedraw();

  HostDisplay_InvalidatePresentHistory();
}

void RuntimeSettings_Install(void) {
  Settings_SetChangeObserver(OnRuntimeSettingChanged);
  Settings_SetActionObserver(RuntimeSettings_HandleAction);
}

RuntimeLifecycleRequest RuntimeSettings_LifecycleRequest(void) {
  return s_lifecycle_request;
}
