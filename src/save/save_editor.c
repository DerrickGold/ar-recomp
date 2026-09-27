#include "save/save_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "snesrecomp/support/utf8_fs.h"
#include "app/user_data_dir.h"

/* Menu zero means leave unchanged. Direct fields keep their displayed value;
 * zero-based fields reserve zero by storing value + 1. Native progress/item
 * values and score units are translated here before SaveSystem validation. */
bool SaveEditor_BuildRequest(const Settings *settings, SaveEditRequest *edits) {
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
  if (!settings || !edits) return false;

  SaveEditRequest_Clear(edits);
  bool staged = false;
  for (int region = 0; region < kActRaiserSaveRegionCount; region++) {
    const int selector = settings->save_region_progress[region];
    if (selector < 0 || selector >= kSaveProgressEdit_Count) continue;
    edits->region_state[region] = kRegionStates[selector];
    staged = staged || edits->region_state[region] >= 0;
  }

#define SAVE_STAGE_DIRECT(request_field, setting_field) do { \
  edits->request_field = settings->setting_field > 0 \
      ? settings->setting_field : -1; \
  staged = staged || edits->request_field >= 0; \
} while (0)
#define SAVE_STAGE_ZERO(request_field, setting_field) do { \
  edits->request_field = settings->setting_field > 0 \
      ? settings->setting_field - 1 : -1; \
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

  if (settings->save_player_name[0]) {
    edits->player_name_set = true;
    snprintf(edits->player_name, sizeof(edits->player_name), "%s",
             settings->save_player_name);
    staged = true;
  }
  if (settings->save_professional_mode > 0) {
    edits->professional_mode = settings->save_professional_mode - 1;
    staged = true;
  }
  if (settings->save_death_heim_state > 0) {
    static const int kDeathHeimStates[] = { -1, 0, 1, 4 };
    const int selector = settings->save_death_heim_state;
    if (selector < (int)(sizeof(kDeathHeimStates) /
                         sizeof(kDeathHeimStates[0]))) {
      edits->death_heim_state = kDeathHeimStates[selector];
      staged = true;
    }
  }
  if (settings->save_equipped_magic > 0) {
    edits->equipped_magic = settings->save_equipped_magic - 1;
    staged = true;
  }
  for (int slot = 0; slot < kActRaiserSaveMagicSlotCount; slot++) {
    if (settings->save_magic_slots[slot] <= 0) continue;
    edits->magic_slots[slot] = settings->save_magic_slots[slot] - 1;
    staged = true;
  }
  for (int slot = 0; slot < kActRaiserSaveItemSlotCount; slot++) {
    const int selector = settings->save_item_slots[slot];
    if (selector <= 0 ||
        selector >= (int)(sizeof(kItemValues) / sizeof(kItemValues[0]))) {
      continue;
    }
    edits->item_slots[slot] = kItemValues[selector];
    staged = true;
  }
  for (int region = 0; region < kActRaiserSaveRegionCount; region++) {
    for (int act = 0; act < kActRaiserSaveActCount; act++) {
      const int selector = settings->save_scores[region][act];
      if (selector <= 0) continue;
      edits->scores[region][act] = (selector - 1) * 10;
      staged = true;
    }
  }
  return staged;
}

const char *SaveEditor_ExportExtension(SettingAction action) {
  switch (action) {
  case kSettingAction_SaveExportCampaign: return "arsave";
  case kSettingAction_SaveExportSrm: return "srm";
  case kSettingAction_SaveExportIni: return "ini";
  default: return NULL;
  }
}

SaveEditorActionResult SaveEditor_HandleFileAction(SettingAction action, const char *path,
                                                  const Settings *settings, SaveError *error) {
  if (error) *error = (SaveError){{0}};
  if (!settings || !path || !*path) {
    if (error) snprintf(error->message, sizeof(error->message), "Choose a save file first.");
    return kSaveEditorAction_Failed;
  }
  if (action == kSettingAction_SaveImport)
    return SaveSystem_Import(path, settings->save_autobackup, error)
               ? kSaveEditorAction_RestartRequired : kSaveEditorAction_Failed;
  if (!SaveEditor_ExportExtension(action)) return kSaveEditorAction_Failed;
  /* Export is a copy, never an alternate way to overwrite a managed slot or
   * its companions. This also preserves existing exports chosen accidentally. */
  FILE *existing = sr_fopen(path, "rb");
  if (existing) fclose(existing);
  if (existing || errno != ENOENT) {
    if (error) snprintf(error->message, sizeof(error->message),
                         "Choose a new export filename; existing files are preserved.");
    return kSaveEditorAction_Failed;
  }
  bool success = action == kSettingAction_SaveExportCampaign
      ? SaveSystem_ExportCampaign(path, error)
      : SaveSystem_Export(action == kSettingAction_SaveExportIni ? kSaveFileFormat_Ini
                                                               : kSaveFileFormat_NativeSrm,
                          path, error);
  return success ? kSaveEditorAction_Completed : kSaveEditorAction_Failed;
}

SaveEditorActionResult SaveEditor_HandleAction(SettingAction action,
                                              const Settings *settings) {
  if (!settings) return kSaveEditorAction_Failed;
  switch (action) {
  case kSettingAction_SaveApplySession:
  case kSettingAction_SaveApplyPersist: {
    SaveEditRequest edits;
    SaveEditor_BuildRequest(settings, &edits);
    SaveError error = {{0}};
    const bool persist = action == kSettingAction_SaveApplyPersist;
    if (!SaveSystem_ApplyEdits(
            &edits, settings->save_edit_armed, persist,
            settings->save_autobackup, &error)) {
      fprintf(stderr, "[save-editor] %s failed: %s\n",
              persist ? "apply and save" : "session apply", error.message);
      return kSaveEditorAction_Failed;
    }
    fprintf(stderr, "[save-editor] staged save edits applied%s\n",
            persist ? " and saved" : " for this session");
    return kSaveEditorAction_Completed;
  }
  case kSettingAction_SaveImport: {
    const char *path = getenv("AR_SAVE_IMPORT");
    char selected[kHostPathCapacity];
    SaveError error = { { 0 } };
    if (!path || !*path) {
      if (!SaveSystem_DefaultImportPath(selected, sizeof(selected), &error)) {
        fprintf(stderr, "[saves] import failed: %s\n", error.message);
        return kSaveEditorAction_Failed;
      }
      path = selected;
    }
    if (!SaveSystem_Import(path, settings->save_autobackup, &error)) {
      fprintf(stderr, "[save-editor] import %s failed: %s\n", path,
              error.message);
      return kSaveEditorAction_Failed;
    }
    fprintf(stderr, "[save-editor] imported %s -> %s\n", path,
            SaveSystem_ActivePath());
    return kSaveEditorAction_RestartRequired;
  }
  case kSettingAction_SaveExportCampaign: {
    SaveError error = {{0}};
    if (!SaveSystem_ExportToLibrary(kSaveFileFormat_NativeSrm, true, &error)) {
      fprintf(stderr, "[saves] campaign export failed: %s\n", error.message);
      return kSaveEditorAction_Failed;
    }
    return kSaveEditorAction_Completed;
  }
  case kSettingAction_SaveExportSrm:
  case kSettingAction_SaveExportIni: {
    const bool ini = action == kSettingAction_SaveExportIni;
    SaveError error = {{0}};
    const SaveFileFormat format =
        ini ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm;
    if (!SaveSystem_ExportToLibrary(format, false, &error)) {
      fprintf(stderr, "[saves] raw export failed: %s\n", error.message);
      return kSaveEditorAction_Failed;
    }
    return kSaveEditorAction_Completed;
  }
  default:
    return kSaveEditorAction_Failed;
  }
}
