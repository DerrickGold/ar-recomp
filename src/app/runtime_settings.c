#include "snesrecomp/support/utf8_fs.h"

#include "app/runtime_settings.h"
#include "audio/audio_session.h"
#include "host/host_localization.h"

#include <stdio.h>
#include <stdlib.h>

#include "actraiser/actraiser_rtl.h"
#include "snesrecomp/game/runtime.h"
#include "diorama/diorama.h"
#include "dev/debug_state.h"
#include "dev/host_dev_tools.h"
#include "randomizer/randomizer.h"
#include "host/host_display.h"
#include "host/host_input.h"
#include "manual/manual_reader.h"
#include "save/save_editor.h"
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
  case kSettingAction_SaveApplyPersist:
  case kSettingAction_SaveImport:
  case kSettingAction_SaveExportCampaign:
  case kSettingAction_SaveExportSrm:
  case kSettingAction_SaveExportIni: {
    const SaveEditorActionResult result =
        SaveEditor_HandleAction(desc->action, &g_settings);
    if (result == kSaveEditorAction_RestartRequired)
      RuntimeSettings_RequestPreparedRestart();
    return result != kSaveEditorAction_Failed;
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

void RuntimeSettings_BeginSession(void) {
  s_lifecycle_request = kRuntimeLifecycle_None;
  Settings_SetChangeObserver(OnRuntimeSettingChanged);
  Settings_SetActionObserver(RuntimeSettings_HandleAction);
}

void RuntimeSettings_EndSession(void) {
  Settings_SetChangeObserver(NULL);
  Settings_SetActionObserver(NULL);
}

RuntimeLifecycleRequest RuntimeSettings_LifecycleRequest(void) {
  return s_lifecycle_request;
}
