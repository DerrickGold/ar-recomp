#include "snesrecomp/support/utf8_fs.h"

#include "app/runtime_settings.h"
#include "audio/audio_session.h"
#include "host/host_localization.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "actraiser/actraiser_rtl.h"
#include "diorama/diorama.h"
#include "present/display_geometry.h"
#include "dev/debug_state.h"
#include "dev/host_dev_tools.h"
#include "randomizer/randomizer.h"
#include "host/host_display.h"
#include "host/host_input.h"
#include "manual/manual_reader.h"
#include "save/save_system.h"
#include "app/session_fatal.h"
#include "settings_overlay/settings_overlay.h"
#include "app/user_data_dir.h"
#include "sim/sim3d/sim3d_textures.h"

static RuntimeLifecycleRequest s_lifecycle_request;
void RuntimeSettings_RequestPreparedRestart(void) {
  s_lifecycle_request=kRuntimeLifecycle_Restart;
  SettingsOverlay_Close();
}

static int RuntimeSettings_QuickStateSlot(void) {
  const char *text = getenv("AR_QUICKSTATE_SLOT");
  if (!text || !text[0]) return 0;
  char *end = NULL;
  const long slot = strtol(text, &end, 0);
  if (!end || *end || slot < 0 || slot > 99) {
    fprintf(stderr,
            "[settings] invalid AR_QUICKSTATE_SLOT='%s' (want 0..99); "
            "using slot 0\n",
            text);
    return 0;
  }
  return (int)slot;
}

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
  if (!desc || desc->type != kSettingType_Action) return false;

  switch (desc->action) {
  case kSettingAction_SaveSlots: return SettingsOverlay_OpenSaveSlots(false);
  case kSettingAction_NewRandomizedGame: return SettingsOverlay_OpenSaveSlots(true);
  case kSettingAction_TogglePause: {
    HostInput_TogglePause();
    break;
  }
  case kSettingAction_ToggleTurbo: {
    HostInput_ToggleTurbo();
    break;
  }
  case kSettingAction_SaveState: {
    return DebugState_Apply(kSaveLoad_Save, RuntimeSettings_QuickStateSlot());
  }
  case kSettingAction_LoadState: {
    return DebugState_Apply(kSaveLoad_Load, RuntimeSettings_QuickStateSlot());
  }
  case kSettingAction_Warp: {
    extern void ActRaiser_Warp(unsigned region, unsigned map);
    const unsigned target = (unsigned)g_settings.warp_target;
    ActRaiser_Warp((target >> 8) & 0xff, target & 0xff);
    break;
  }
  case kSettingAction_Snapshot: {
    HostDevTools_TakeFullSnapshot();
    break;
  }
  case kSettingAction_Manual: {
    /* Returns false when there is no manual to show, which the overlay reports
     * as a failed action rather than opening onto an empty reader. */
    if (!ManualReader_Open()) return false;
    break;
  }
  case kSettingAction_Reroll: {
    if (!Randomizer_IsAvailable() || Randomizer_CampaignBound()) return false;
    Randomizer_Reroll();
    break;
  }
  case kSettingAction_DioramaReset: {
    Diorama_ResetCamera();
    break;
  }
  case kSettingAction_SimCameraReset: {
    HostInput_ResetSim3DCamera();
    break;
  }
  case kSettingAction_DumpSceneAssets: {
    if (!HostDevTools_DumpSceneAssets()) return false;
    break;
  }
  case kSettingAction_SaveApplySession:
  case kSettingAction_SaveApplyPersist: {
    SaveEditRequest edits;
    RuntimeSettings_BuildSaveEditRequest(&edits);
    SaveError error = {{0}};
    const bool persist = desc->action == kSettingAction_SaveApplyPersist;
    if (!SaveSystem_ApplyEdits(
            &edits, g_settings.save_edit_armed, persist,
            g_settings.save_autobackup, &error)) {
      fprintf(stderr, "[save-editor] %s failed: %s\n",
              persist ? "apply and save" : "session apply", error.message);
      return false;
    }
    fprintf(stderr, "[save-editor] staged save edits applied%s\n",
            persist ? " and saved" : " for this session");
    break;
  }
  case kSettingAction_SaveImport: {
    const char *path = getenv("AR_SAVE_IMPORT");
    char selected[kHostPathCapacity];
    SaveError error = { { 0 } };
    if(!path || !*path) {
      if(!SaveSystem_DefaultImportPath(selected,sizeof(selected),&error)) {
        fprintf(stderr, "[saves] import failed: %s\n", error.message);
        return false;
      }
      path=selected;
    }
    if (!SaveSystem_Import(path, g_settings.save_autobackup, &error)) {
      fprintf(stderr, "[save-editor] import %s failed: %s\n", path,
              error.message);
      return false;
    }
    fprintf(stderr, "[save-editor] imported %s -> %s\n", path,
            SaveSystem_ActivePath());
    RuntimeSettings_RequestPreparedRestart();
    break;
  }
  case kSettingAction_SaveExportCampaign: {
    SaveError error={{0}};
    if(!SaveSystem_ExportToLibrary(kSaveFileFormat_NativeSrm,true,&error)) {
      fprintf(stderr, "[saves] campaign export failed: %s\n", error.message);
      return false;
    }
    break;
  }
  case kSettingAction_SaveExportSrm:
  case kSettingAction_SaveExportIni: {
    const bool ini = desc->action == kSettingAction_SaveExportIni;
    SaveError error = {{0}};
    if (!SaveSystem_ExportToLibrary(ini ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm, false,
                                    &error)) {
      fprintf(stderr, "[saves] raw export failed: %s\n", error.message);
      return false;
    }
    break;
  }
  case kSettingAction_Restart:
  case kSettingAction_Exit: {
    char settings_path[kHostPathCapacity];
    UserDataFile(settings_path, sizeof(settings_path), "settings.ini");
    if (!Settings_Save(settings_path)) {
      fprintf(stderr, "[lifecycle] could not save settings.ini\n");
      return false;
    }
    s_lifecycle_request = desc->action == kSettingAction_Restart
        ? kRuntimeLifecycle_Restart
        : kRuntimeLifecycle_Exit;
    SettingsOverlay_Close();
    fprintf(stderr, "[lifecycle] %s requested\n",
            s_lifecycle_request == kRuntimeLifecycle_Restart
                ? "restart" : "exit");
    break;
  }
  default:
    return false;
  }
  return true;
}

static void OnRuntimeSettingChanged(const SettingDesc *desc,
                                    SettingChangeResult result) {
  (void)result;

  /* Owners interpret their own fields; this is only the application wiring.
   * A geometry edit is disjoint from the camera/texture edits, and display
   * invalidation still follows successful subsystem updates. */
  HostLocalization_ApplySetting(desc);
  AudioSession_ApplySetting(desc);
  HostInput_ApplySetting(desc);
  Diorama_ApplySetting(desc);
  if (!Sim3DTextures_ValidateSetting(desc))
    return;
  HostDisplay_ApplySetting(desc);
}

void RuntimeSettings_Install(void) {
  Settings_SetChangeObserver(OnRuntimeSettingChanged);
  Settings_SetActionObserver(RuntimeSettings_HandleAction);
}

RuntimeLifecycleRequest RuntimeSettings_LifecycleRequest(void) {
  return s_lifecycle_request;
}
