#include "save/save_slot_host.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser/regional/actraiser_regional_runtime.h"
#include "app/input_replay.h"
#include "app/runtime_settings.h"
#include "app/settings.h"
#include "app/performance_metrics.h"
#include "app/session_fatal.h"
#include "app/user_data_dir.h"
#include "constants.h"
#include "host/campaign_identity.h"
#include "host/host_video.h"
#include "randomizer/randomizer.h"
#include "save/save_editor.h"
#include "save/save_slot_manager.h"
#include "save/save_slots.h"
#include "save/save_system.h"
#include "settings_overlay/settings_overlay.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/types.h"
#include "snesrecomp/support/utf8_fs.h"

static SaveSlots s_save_slots;
static bool s_managed_slots;
static uint64_t s_town_visit_retry_ms;

static void FlushTownVisit(bool final) {
  const uint64_t now = SDL_GetTicks();
  if (!final && now < s_town_visit_retry_ms) return;
  SaveError error = {{0}};
  if (SaveSystem_FlushTownVisit(&error)) {
    s_town_visit_retry_ms = 0;
  } else {
    /* Optional presentation metadata must not make gameplay fatal. Bound
     * failed-I/O retries so a read-only disk cannot cause a per-frame hitch. */
    if (!s_town_visit_retry_ms || final)
      fprintf(stderr, "[saves] town bookmark not saved: %s\n", error.message);
    s_town_visit_retry_ms = now + 5000;
  }
}

static bool SlotChooseFile(SettingAction action, SaveError *error) {
  const char *extension = SaveEditor_ExportExtension(action);
  if (!extension && action != kSettingAction_SaveImport) return false;
  char location[kHostPathCapacity], leaf[96];
  if (extension)
    snprintf(leaf, sizeof(leaf), "saves/slot-%02u.%s", s_save_slots.active + 1, extension);
  else
    snprintf(leaf, sizeof(leaf), "saves");
  /* Launchers anchor the data working directory. Native pickers need an
   * absolute starting location, unlike the save APIs' relative paths. */
  char *directory = SDL_GetCurrentDirectory();
  if (!directory) {
    snprintf(error->message, sizeof(error->message), "%s", SDL_GetError());
    return false;
  }
  int length = snprintf(location, sizeof(location), "%s%s", directory, leaf);
  SDL_free(directory);
  if (length < 0 || (size_t)length >= sizeof(location)) {
    snprintf(error->message, sizeof(error->message), "The save file location is too long.");
    return false;
  }
  return SaveFileDialog_Begin(g_window, extension, location, error);
}
static bool SlotEditorReady(SaveError *error) {
  if (!s_managed_slots || !InputReplay_PolicyChangesAllowed() || s_save_slots.pending ||
      RuntimeSettings_LifecycleRequest() != kRuntimeLifecycle_None) {
    snprintf(error->message, sizeof(error->message), "Save routing is not ready for this action.");
    return false;
  }
  return true;
}
static bool SlotApplyEdits(SettingAction action, SaveError *error) {
  if (!SlotEditorReady(error)) return false;
  SaveEditorActionResult result = SaveEditor_ApplyConfirmedEdits(action, &g_settings, error);
  if (result == kSaveEditorAction_RestartRequired) RuntimeSettings_RequestPreparedRestart();
  return result != kSaveEditorAction_Failed;
}
static bool SlotFileAction(SettingAction action, const char *path, SaveError *error) {
  if (!SlotEditorReady(error)) return false;
  SaveEditorActionResult result = SaveEditor_HandleFileAction(action, path, &g_settings, error);
  if (result == kSaveEditorAction_RestartRequired) RuntimeSettings_RequestPreparedRestart();
  return result != kSaveEditorAction_Failed;
}

static bool SlotValidateActive(void *context,SaveError *error);
static bool SlotBeforeCommit(void *context,SaveError *error) {
  SaveSlots *slots=context;
  if(slots->records[slots->active].checkpoint_required &&
      slots->records[slots->active].ever_saved && !slots->first_write_in_progress &&
      !SlotValidateActive(context,error))return false;
  return SaveSlots_BeforeCommit(context,error);
}
static void SlotDidCommit(void *context,const uint8_t *image) {
  SaveSlots_DidCommit(context,image);
}
static bool SlotValidateActive(void *context,SaveError *error) {
  SaveSlots *slots = context;
  SaveSlotDetails details;
  if(!SaveSlots_ObserveCheckpoints(slots,error))return false;
  if(SaveSlotManager_Inspect(slots,slots->active,&details))return true;
  if (error) *error = details.error;
  return false;
}
static bool SlotScan(SaveSlotCollection *out) {
  if(!out)return false;
  *out=(SaveSlotCollection){.active=s_save_slots.active,
    .writable=s_managed_slots && InputReplay_PolicyChangesAllowed() && !s_save_slots.pending};
  if(!s_managed_slots) {
    snprintf(out->error.message, sizeof(out->error.message),
             "External save: slot switching is unavailable for diagnostic paths and recordings.");
    for(unsigned i=0;i<kSaveSlotCount;++i)out->slots[i].state=kSaveSlot_Unavailable;
    return true;
  }
  if (!out->writable)
    snprintf(out->error.message, sizeof(out->error.message),
             "Save slots are read-only during recording, replay or a pending restart.");
  for (unsigned i = 0; i < kSaveSlotCount; ++i)
    (void)SaveSlotManager_Inspect(&s_save_slots, i, &out->slots[i]);
  return true;
}
static bool SlotDraft(unsigned slot,ArRegionalSession *out,SaveError *error) {
  if(!s_managed_slots || slot>=kSaveSlotCount) {
    snprintf(error->message, sizeof(error->message), "This save slot is unavailable.");
    return false;
  }
  if(s_save_slots.records[slot].prepared)
    return SaveSlotManager_ReadDraft(&s_save_slots,slot,out,error);
  uint8_t id[16];
  if(!HostCampaignIdentity_Create(NULL,id)) {
    snprintf(error->message, sizeof(error->message), "Cannot create a new campaign identity.");
    return false;
  }
  ActRaiserRegionalRulesView current;
  ArRegionalSession baseline;
  const ArRegionalCostPolicy costs={{0}};
  if(!ArRegionalSession_NewGame(&baseline,slot,id,&costs))return false;
  const ArRegionalRules *rules =
      ActRaiserRegional_CopyRulesView(&current) ? &current.requested : &baseline.requested;
  RandomizerConfig recipe=Randomizer_CurrentConfig();
  if(!SaveSlotManager_Draft(out,slot,id,rules,&recipe)) {
    snprintf(error->message, sizeof(error->message), "Cannot prepare this new-game setup.");
    return false;
  }
  return true;
}
static bool SlotDraftView(const ArRegionalSession *draft,ActRaiserRegionalRulesView *out) {
  if(!SaveSlotManager_View(draft,true,out))return false;
  ActRaiserRegionalRulesView current;
  if(ActRaiserRegional_CopyRulesView(&current)) {
    out->artwork_available = current.artwork_available;
    out->sequences_available = current.sequences_available;
    out->actor_artwork_available=current.actor_artwork_available;
  }
  return true;
}
static bool SlotSaveRegionalSettings(void *context,const ArRegionalSession *before,
    const ArRegionalSession *after,SaveError *error) {
  SaveSlots *slots=context;
  if(!InputReplay_PolicyChangesAllowed() || slots->pending || before->slot!=slots->active ||
      RuntimeSettings_LifecycleRequest()!=kRuntimeLifecycle_None) {
    snprintf(error->message, sizeof(error->message),
             "Save routing is not ready for regional settings.");
    return false;
  }
  /* Finish any already-completed native save first. Never take a new gameplay
   * snapshot just because a menu setting changed. */
  if(!SaveSystem_FlushForSwitch(error) || !SaveSystem_ValidateActive(error))return false;
  uint8_t image[kActRaiserSramSize];
  if(SaveSystem_CopyDurableImage(image)) {
    SaveFileFormat format = SaveSystem_ActiveBackend() == kSaveBackend_Ini
        ? kSaveFileFormat_Ini
        : kSaveFileFormat_NativeSrm;
    return ArRegionalCampaign_SaveSettings(before, after, format, SaveSystem_ActivePath(), image,
                                           error);
  }
  ArRegionalSession draft;
  const RandomizerConfig recipe=Randomizer_CurrentConfig();
  uint8_t bytes[kSaveSlotDraftCapacity];
  size_t size;
  if(!SaveSlotManager_Draft(&draft,after->slot,after->campaign,&after->requested,&recipe) ||
      !ArRegionalSession_Encode(&draft,bytes,sizeof(bytes),&size)) {
    snprintf(error->message, sizeof(error->message), "Cannot prepare the new-game settings.");
    return false;
  }
  return SaveSlots_UpdateDraft(slots,bytes,size,error);
}
static bool SlotStart(unsigned slot, uint64_t fingerprint, const ArRegionalSession *draft,
                      SaveError *error) {
  ActRaiserRegionalRulesView current;
  if (!s_managed_slots || !InputReplay_PolicyChangesAllowed() || s_save_slots.pending ||
      RuntimeSettings_LifecycleRequest() != kRuntimeLifecycle_None ||
      (ActRaiserRegional_CopyRulesView(&current) &&
       (current.population_pending || current.miracle_in_progress))) {
    snprintf(error->message, sizeof(error->message),
             "Finish the current game operation before changing saves.");
    return false;
  }
  SaveSlotDetails target;
  if(!SaveSlotManager_Inspect(&s_save_slots,slot,&target)){*error=target.error;return false;}
  if (target.fingerprint != fingerprint) {
    snprintf(error->message, sizeof(error->message),
             "The slot changed. Close and reopen Saves to review it.");
    return false;
  }
  uint8_t bytes[kSaveSlotDraftCapacity];
  size_t size = 0;
  if(draft) {
    ArRegionalSession validated;
    if (draft->slot != slot ||
        !SaveSlotManager_Draft(&validated, slot, draft->campaign, &draft->requested,
                               &draft->randomizer) ||
        !ArRegionalSession_Encode(&validated, bytes, sizeof(bytes), &size)) {
      snprintf(error->message, sizeof(error->message), "The new-game configuration is invalid.");
      return false;
    }
  }
  if(!SaveSystem_FlushForSwitch(error) || !SaveSlots_Flush(&s_save_slots,error))return false;
  char settings_path[kHostPathCapacity];
  const char *path=getenv("AR_SETTINGS_PATH");
  if(!path || !*path)path=UserDataFile(settings_path,sizeof(settings_path),"settings.ini");
  /* Global editor overrides have no destination identity. Disarm them before
   * persisting the restart; manual editor actions remain available per slot. */
  g_settings.save_edit_armed=false;
  if (!Settings_Save(path)) {
    snprintf(error->message, sizeof(error->message),
             "Could not save preferences. The current slot remains active.");
    return false;
  }
  if (!SaveSlots_Request(&s_save_slots, slot, fingerprint, draft ? bytes : NULL, size,
                         (SaveBackend)g_settings.save_backend, error))
    return false;
  RuntimeSettings_RequestPreparedRestart();
  return true;
}

static void SlotValidateBoot(void) {
  for(;;) {
    SaveError error={{0}};SaveSlotDetails details;
    bool valid=SaveSlots_ValidateDestination(&s_save_slots,&error);
    if(valid && !SaveSlotManager_Inspect(&s_save_slots,s_save_slots.destination,&details)) {
      error = details.error;
      valid = false;
    }
    if(valid)return;
    const SDL_MessageBoxButtonData buttons[]={
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,0,"Exit"},
      {0,1,"Return to previous slot"},{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,2,"Retry"}};
    char message[512];
    snprintf(message, sizeof(message),
             "Slot %u could not be opened.\n%s\n\nYour saves have been preserved.",
             s_save_slots.destination + 1, error.message);
    SDL_MessageBoxData box = {
      SDL_MESSAGEBOX_ERROR, g_window, "Save recovery", message, 3, buttons, NULL
    };
    int choice = 0;
    if(!SDL_ShowMessageBox(&box,&choice) || choice<=0)Die(message);
    if(choice==1 && !SaveSlots_ReturnToPrevious(&s_save_slots,&error))Die(error.message);
  }
}

void SaveSlotHost_AttachBatterySave(bool headless) {
  s_town_visit_retry_ms = 0;
  /* Load persisted battery save (overrides the fresh-cart fill if present).
   * Portable builds use saves/ beside the executable after the bundle anchor;
   * developer runs use saves/ under their launch directory. */
  char saves_dir[kHostPathCapacity], save_srm[kHostPathCapacity],
      save_ini[kHostPathCapacity], legacy_srm[kHostPathCapacity];
  UserDataFile(saves_dir, sizeof saves_dir, "saves");
  sr_mkdir(saves_dir);
  {
    SaveError error = {{0}};
    const char *native_path = getenv("AR_SAVE_NATIVE_PATH");
    const char *ini_path = getenv("AR_SAVE_INI_PATH");
    s_managed_slots=!headless && !(native_path && *native_path) && !(ini_path && *ini_path) &&
        !getenv("AR_SAVE_BACKEND") && !getenv("AR_INPUT_REPLAY") && !getenv("AR_INPUT_RECORD");
    SaveBackend backend=(SaveBackend)g_settings.save_backend;
    if(s_managed_slots) {
      if(!SaveSlots_Open(&s_save_slots,saves_dir,backend,&error))Die(error.message);
      SlotValidateBoot();
      if (!SaveSlots_Paths(&s_save_slots, s_save_slots.destination, save_srm, save_ini,
                           sizeof(save_srm)))
        Die("Save slot path is too long.");
      native_path = save_srm;
      ini_path = save_ini;
      backend=SaveSlots_DestinationBackend(&s_save_slots);
    }
    if (!native_path || !native_path[0]) {
      UserDataFile(save_srm, sizeof save_srm, "saves/save.srm");
      native_path = save_srm;
    }
    if (!ini_path || !ini_path[0]) {
      UserDataFile(save_ini, sizeof save_ini, "saves/save.ini");
      ini_path = save_ini;
    }
    if (!SaveSystem_Attach(g_sram, (size_t)g_sram_size,
                           backend,
                           native_path, ini_path, &error))
      Die(error.message);
    if (!SaveSystem_SetStorageRoot(saves_dir, s_managed_slots ? (int)s_save_slots.destination : -1,
                                   &error))
      Die(error.message);
    snprintf(legacy_srm, sizeof(legacy_srm), "%s/%s.srm",
             saves_dir, RtlGameIdentifier());
    if(!s_managed_slots && !SaveSystem_MigrateLegacyNative(legacy_srm,&error))Die(error.message);
    if (!SaveSystem_LoadActive(&error)) {
      char message[512];
      snprintf(message, sizeof(message),
               "The active save could not be loaded: %s\n\nThe game will "
               "not start with fresh SRAM because doing so could overwrite "
               "recoverable progress. Repair, restore, or move %s and try "
               "again.",
               error.message, SaveSystem_ActivePath());
      Die(message);
    }

    SaveEditRequest edits;
    if(s_managed_slots)g_settings.save_edit_armed=false;
    bool staged = SaveEditor_BuildRequest(&g_settings, &edits);
    if (staged && g_settings.save_edit_armed) {
      if (!SaveSystem_ApplyEdits(
              &edits, true, false, g_settings.save_autobackup, &error))
        fprintf(stderr, "[save-editor] boot edits rejected: %s\n",
                error.message);
      else
        fprintf(stderr, "[save-editor] boot edits applied for this session\n");
    } else if (staged) {
      fprintf(stderr,
              "[save-editor] staged boot edits ignored; save editing is not armed\n");
    }
  }

}

void SaveSlotHost_InitializeRegionalCampaign(void) {
  if (!ActRaiserRegional_InitializeSlot(s_managed_slots ? s_save_slots.destination : 0,
                                        HostCampaignIdentity_Create, NULL))
    Die("Regional campaign storage could not be initialized; saves preserved.");
  if(s_managed_slots) {
    SaveSlotDetails details;
    if (!SaveSlotManager_Inspect(&s_save_slots, s_save_slots.destination, &details))
      Die(details.error.message);
    if(details.state==kSaveSlot_Empty && details.prepared) {
      ArRegionalSession draft;
      SaveError error = { { 0 } };
      if (!SaveSlotManager_ReadDraft(&s_save_slots, s_save_slots.destination, &draft, &error) ||
          !ActRaiserRegional_StageNewGame(&draft))
        Die("Cannot stage the prepared new game; saves preserved.");
    }
  }
}

void SaveSlotHost_InstallHooks(void) {
  if(s_managed_slots) {
    SaveError error={{0}};
    if(!SaveSlots_Acknowledge(&s_save_slots,&error))Die(error.message);
    const SaveStorageHooks storage = { &s_save_slots, SlotBeforeCommit, SlotDidCommit,
                                       SlotValidateActive };
    SaveSystem_SetStorageHooks(&storage);
    ActRaiserRegional_SetSettingsWriter(SlotSaveRegionalSettings,&s_save_slots);
  }
  const SettingsOverlaySaveSlotHooks slots = {
      .scan = SlotScan, .draft = SlotDraft, .edit = SaveSlotManager_Edit,
      .view = SlotDraftView, .start = SlotStart, .choose_file = SlotChooseFile,
      .poll_file = SaveFileDialog_Poll, .cancel_file = SaveFileDialog_Cancel,
      .file_action = SlotFileAction, .apply_edits = SlotApplyEdits};
  SettingsOverlay_SetSaveSlotHooks(&slots);
  if (s_managed_slots && SaveSlots_NeedsSetup(&s_save_slots)) {
    SettingsOverlay_Open();
    if (!SettingsOverlay_OpenSaveSlots(false))
      Die("The new-game save menu could not be opened.");
    fprintf(stderr, "[saves] no campaigns: opened save setup before the first game tick\n");
  }
}

bool SaveSlotHost_Close(void) {
  SaveFileDialog_Cancel();
  if (!s_managed_slots) return true;
  SaveError error = {{0}};
  const bool flushed = SaveSlots_Flush(&s_save_slots, &error);
  SaveSlots_Close(&s_save_slots);
  s_managed_slots = false;
  SaveSystem_SetStorageHooks(NULL);
  return flushed;
}

void SaveSlotHost_AfterTicks(void) {
  /* Auto-persist battery SRAM the moment the game writes a save, so progress
   * survives a freeze/force-quit (the clean-exit save-system write never runs
   * if the game hangs). Cheap: only writes when the 8KB SRAM actually changes.
   * SKIPPED during input replay: letting a diagnostic run overwrite save.srm
   * would change the initial state of the NEXT replay and invalidate canonical
   * initial-state/checkpoint digests as well as legacy frame alignment. */
  if (!InputReplay_ShouldProtectSaveData()) {
    static bool write_error_reported;
    static uint64_t first_write_failure_ms;
    SaveError error = {{0}};
    const PerformanceScope save_scope = PerformanceMetrics_Begin(kPerformance_SaveWrite);
    const bool saved = SaveSystem_AutoPersistIfChanged(&error);
    if (saved) FlushTownVisit(false);
    PerformanceMetrics_End(save_scope);
    if (!saved) {
      if (!write_error_reported)
        fprintf(stderr, "[saves] auto-persist failed: %s\n", error.message);
      write_error_reported = true;
      const uint64_t now_ms = SDL_GetTicks();
      if (!first_write_failure_ms) first_write_failure_ms = now_ms;
      if (now_ms - first_write_failure_ms >= 5000) {
        SessionFatal_RequestKind(kSessionFailure_BatterySave,
            "battery auto-persist failed for five seconds: %s; path: %s",
            error.message, SaveSystem_ActivePath());
      }
    } else {
      write_error_reported = false;
      first_write_failure_ms = 0;
    }
  }
}

bool SaveSlotHost_FlushBatterySave(void) {
  bool save_flush_failed = false;
  /* Rendering is synchronous, so nothing can be mid-render during the reverse-
   * dependency teardown below. Flush only game-originated battery changes on
   * exit. Deliberate session-only editor changes re-sync the save-system shadow;
   * Restart/Exit after one must not turn it into a persistent edit. Skip the
   * flush during replay so a replayed run never mutates the active save (see
   * the auto-persist note above — it would break the next replay's alignment). */
  if (!InputReplay_ShouldProtectSaveData()) {
    SaveError error = {{0}};
    if (!SaveSystem_AutoPersistIfChanged(&error)) {
      save_flush_failed = true;
      fprintf(stderr, "[saves] shutdown flush failed: %s\n", error.message);
    }
    if (!save_flush_failed) FlushTownVisit(true);
  }
  return !save_flush_failed;
}
