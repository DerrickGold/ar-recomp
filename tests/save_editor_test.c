#define _POSIX_C_SOURCE 200809L
#include "save/save_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures, s_apply_calls, s_import_calls, s_export_calls;
static bool s_success = true, s_default_available = true;
static bool s_armed, s_persist, s_backup, s_campaign;
static SaveFileFormat s_format;
static SaveEditRequest s_applied;
static char s_import_path[128];

#define CHECK(expr)                                                                                \
  do {                                                                                             \
    if (!(expr)) {                                                                                 \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr);                     \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (0)

/* SaveSystem's sentinel contract; the real codec and persistence transactions
 * have their own tests. These boundaries record requests without file I/O. */
void SaveEditRequest_Clear(SaveEditRequest *edits) {
  memset(edits, 0xff, sizeof(*edits));
  edits->player_name_set = false;
  edits->player_name[0] = 0;
}

bool SaveSystem_ApplyEdits(const SaveEditRequest *edits, bool armed, bool persist, bool auto_backup,
                           SaveError *error) {
  if (!s_success && error)
    snprintf(error->message, sizeof(error->message), "Save fixture rejected the edit.");
  s_apply_calls++;
  s_applied = *edits;
  s_armed = armed;
  s_persist = persist;
  s_backup = auto_backup;
  return s_success;
}

bool SaveSystem_DefaultImportPath(char *out, size_t capacity, SaveError *error) {
  (void)error;
  snprintf(out, capacity, "%s", "library/selected.srm");
  return s_default_available;
}

bool SaveSystem_Import(const char *path, bool auto_backup, SaveError *error) {
  (void)error;
  s_import_calls++;
  snprintf(s_import_path, sizeof(s_import_path), "%s", path);
  s_backup = auto_backup;
  return s_success;
}

const char *SaveSystem_ActivePath(void) { return "active.srm"; }

bool SaveSystem_ExportToLibrary(SaveFileFormat format, bool campaign, SaveError *error) {
  (void)error;
  s_export_calls++;
  s_format = format;
  s_campaign = campaign;
  return s_success;
}
bool SaveSystem_ExportCampaign(const char *path, SaveError *error) {
  snprintf(s_import_path, sizeof(s_import_path), "%s", path);
  return SaveSystem_ExportToLibrary(kSaveFileFormat_NativeSrm, true, error);
}
bool SaveSystem_Export(SaveFileFormat format, const char *path, SaveError *error) {
  snprintf(s_import_path, sizeof(s_import_path), "%s", path);
  return SaveSystem_ExportToLibrary(format, false, error);
}

static void TestStagedValues(void) {
  Settings draft = {0};
  SaveEditRequest edits;
  CHECK(!SaveEditor_BuildRequest(NULL, &edits));
  CHECK(!SaveEditor_BuildRequest(&draft, NULL));
  CHECK(!SaveEditor_BuildRequest(&draft, &edits));
  CHECK(!edits.player_name_set && !edits.player_name[0]);
  CHECK(edits.master_level == -1 && edits.master_hp == -1);
  CHECK(edits.master_mp == -1 && edits.lives == -1);
  CHECK(edits.angel_sp_current == -1 && edits.angel_sp_max == -1);
  CHECK(edits.angel_hp_current == -1 && edits.angel_hp_max == -1);
  CHECK(edits.message_speed == -1 && edits.professional_mode == -1);
  CHECK(edits.death_heim_state == -1 && edits.equipped_magic == -1);
  for (int region = 0; region < kActRaiserSaveRegionCount; region++) {
    CHECK(edits.region_state[region] == -1);
    for (int act = 0; act < kActRaiserSaveActCount; act++)
      CHECK(edits.scores[region][act] == -1);
  }
  for (int slot = 0; slot < kActRaiserSaveMagicSlotCount; slot++)
    CHECK(edits.magic_slots[slot] == -1);
  for (int slot = 0; slot < kActRaiserSaveItemSlotCount; slot++)
    CHECK(edits.item_slots[slot] == -1);

  draft.save_master_level = 7;
  draft.save_master_hp = 20;
  draft.save_master_mp = 1;
  draft.save_lives = 3;
  draft.save_angel_sp_current = 11;
  draft.save_angel_sp_max = 51;
  draft.save_angel_hp_current = 1;
  draft.save_angel_hp_max = 8;
  draft.save_message_speed = 2;
  draft.save_professional_mode = 1;
  draft.save_equipped_magic = 1;
  strcpy(draft.save_player_name, "ACTRAIS");
  draft.save_region_progress[0] = kSaveProgressEdit_Act1;
  draft.save_region_progress[1] = kSaveProgressEdit_Act1Cleared;
  draft.save_region_progress[2] = kSaveProgressEdit_Act2;
  draft.save_region_progress[3] = kSaveProgressEdit_Act2Cleared;
  draft.save_region_progress[4] = -1;
  draft.save_region_progress[5] = kSaveProgressEdit_Count;
  draft.save_magic_slots[0] = 1;
  draft.save_magic_slots[1] = 4;
  draft.save_item_slots[0] = 1;
  draft.save_item_slots[1] = 2;
  draft.save_item_slots[2] = 14;
  draft.save_item_slots[3] = 15;
  draft.save_item_slots[4] = -1;
  draft.save_scores[0][0] = 1;
  draft.save_scores[5][1] = 1235;
  CHECK(SaveEditor_BuildRequest(&draft, &edits));
  CHECK(edits.master_level == 7 && edits.master_hp == 20);
  CHECK(edits.master_mp == 0 && edits.lives == 3);
  CHECK(edits.angel_sp_current == 10 && edits.angel_sp_max == 50);
  CHECK(edits.angel_hp_current == 0 && edits.angel_hp_max == 8);
  CHECK(edits.message_speed == 1 && edits.professional_mode == 0);
  CHECK(edits.equipped_magic == 0);
  CHECK(edits.player_name_set && !strcmp(edits.player_name, "ACTRAIS"));
  CHECK(edits.region_state[0] == kSaveRegionState_Act1);
  CHECK(edits.region_state[1] == kSaveRegionState_Act1Cleared);
  CHECK(edits.region_state[2] == kSaveRegionState_Act2);
  CHECK(edits.region_state[3] == kSaveRegionState_Act2Cleared);
  CHECK(edits.region_state[4] == -1 && edits.region_state[5] == -1);
  CHECK(edits.magic_slots[0] == 0 && edits.magic_slots[1] == 3);
  CHECK(edits.item_slots[0] == 0 && edits.item_slots[1] == 5);
  CHECK(edits.item_slots[2] == 0x14 && edits.item_slots[3] == -1);
  CHECK(edits.item_slots[4] == -1);
  CHECK(edits.scores[0][0] == 0 && edits.scores[5][1] == 12340);
  const int death_states[] = {-1, 0, 1, 4, -1};
  for (int selector = 0; selector < 5; selector++) {
    draft.save_death_heim_state = selector;
    CHECK(SaveEditor_BuildRequest(&draft, &edits));
    CHECK(edits.death_heim_state == death_states[selector]);
  }
  /* An emptied draft must not reuse any prior staged fields. */
  draft = (Settings){0};
  CHECK(!SaveEditor_BuildRequest(&draft, &edits));
  CHECK(edits.scores[5][1] == -1 && !edits.player_name_set);
}

static void TestActions(void) {
  Settings draft = {.save_edit_armed = true, .save_autobackup = true, .save_master_level = 7};
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveApplySession, &draft) ==
        kSaveEditorAction_Completed);
  CHECK(s_apply_calls == 1 && s_armed && !s_persist && s_backup);
  CHECK(s_applied.master_level == 7 && s_applied.master_mp == -1);
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveApplyPersist, &draft) ==
        kSaveEditorAction_Completed);
  CHECK(s_apply_calls == 2 && s_persist);
  s_success = false;
  draft.save_edit_armed = false;
  draft.save_autobackup = false;
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveApplyPersist, &draft) ==
        kSaveEditorAction_Failed);
  CHECK(s_apply_calls == 3 && !s_armed && !s_backup);

  unsetenv("AR_SAVE_IMPORT");
  s_default_available = false;
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveImport, &draft) == kSaveEditorAction_Failed);
  CHECK(s_import_calls == 0);
  s_default_available = true;
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveImport, &draft) == kSaveEditorAction_Failed);
  CHECK(s_import_calls == 1 && !s_backup);
  s_success = true;
  draft.save_autobackup = true;
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveImport, &draft) ==
        kSaveEditorAction_RestartRequired);
  CHECK(!strcmp(s_import_path, "library/selected.srm") && s_backup);
  setenv("AR_SAVE_IMPORT", "explicit.srm", 1);
  s_default_available = false;
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveImport, &draft) ==
        kSaveEditorAction_RestartRequired);
  CHECK(!strcmp(s_import_path, "explicit.srm"));
  unsetenv("AR_SAVE_IMPORT");

  const SettingAction exports[] = {kSettingAction_SaveExportCampaign, kSettingAction_SaveExportSrm,
                                   kSettingAction_SaveExportIni};
  for (int i = 0; i < 3; i++) {
    s_success = true;
    CHECK(SaveEditor_HandleAction(exports[i], &draft) == kSaveEditorAction_Completed);
    CHECK(s_campaign == (i == 0));
    CHECK(s_format == (i == 2 ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm));
    s_success = false;
    CHECK(SaveEditor_HandleAction(exports[i], &draft) == kSaveEditorAction_Failed);
  }
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveImport, NULL) == kSaveEditorAction_Failed);
  CHECK(SaveEditor_HandleAction(kSettingAction_TogglePause, &draft) == kSaveEditorAction_Failed);
  CHECK(s_apply_calls == 3 && s_import_calls == 3 && s_export_calls == 6);
}

static void TestPickedFiles(void) {
  Settings settings = {.save_autobackup = true};
  SaveError error = {{0}};
  s_success = true;
  const int imports = s_import_calls, exports = s_export_calls;
  setenv("AR_SAVE_IMPORT", "must-not-import-this.srm", 1);
  CHECK(SaveEditor_HandleFileAction(kSettingAction_SaveImport, "chosen.srm", &settings, &error) ==
        kSaveEditorAction_RestartRequired);
  CHECK(s_import_calls == imports + 1 && !strcmp(s_import_path, "chosen.srm") && s_backup);
  unsetenv("AR_SAVE_IMPORT");
  const SettingAction actions[] = {kSettingAction_SaveExportCampaign, kSettingAction_SaveExportSrm,
                                   kSettingAction_SaveExportIni};
  const char *extensions[] = {"arsave", "srm", "ini"};
  for (unsigned i = 0; i < 3; ++i) {
    CHECK(!strcmp(SaveEditor_ExportExtension(actions[i]), extensions[i]));
    CHECK(SaveEditor_HandleFileAction(actions[i], "unused-picked-export", &settings, &error) ==
          kSaveEditorAction_Completed);
    CHECK(s_export_calls == exports + (int)i + 1 && s_campaign == (i == 0));
    CHECK(s_format == (i == 2 ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm));
    CHECK(!strcmp(s_import_path, "unused-picked-export"));
    /* A chosen existing file is never overwritten, including a slot's save. */
    CHECK(SaveEditor_HandleFileAction(actions[i], __FILE__, &settings, &error) ==
          kSaveEditorAction_Failed);
    CHECK(strstr(error.message, "existing files") && s_export_calls == exports + (int)i + 1);
  }
  CHECK(!SaveEditor_ExportExtension(kSettingAction_SaveImport));
  CHECK(SaveEditor_HandleFileAction(kSettingAction_SaveImport, "", &settings, &error) ==
        kSaveEditorAction_Failed);
  CHECK(SaveEditor_HandleFileAction(kSettingAction_SaveApplyPersist, "chosen", &settings, &error) ==
        kSaveEditorAction_Failed);
  CHECK(SaveEditor_HandleFileAction(kSettingAction_SaveImport, "chosen", NULL, &error) ==
        kSaveEditorAction_Failed);
  CHECK(s_import_calls == imports + 1 && s_export_calls == exports + 3);
}

static void TestConfirmedEdits(void) {
  Settings draft = {.save_master_level = 17, .save_autobackup = true};
  SaveError error = {{0}};
  int calls = s_apply_calls;
  s_success = true;
  const SettingAction actions[] = {kSettingAction_SaveApplySession,
                                   kSettingAction_SaveApplyPersist,
                                   kSettingAction_SaveApplyRestart};
  for (unsigned i = 0; i < 3; ++i) {
    const SaveEditorActionResult expected = i == 2 ? kSaveEditorAction_RestartRequired
                                                   : kSaveEditorAction_Completed;
    CHECK(SaveEditor_ApplyConfirmedEdits(actions[i], &draft, &error) == expected);
    CHECK(s_apply_calls == ++calls && s_armed && s_persist == (i != 0) && s_backup);
    CHECK(s_applied.master_level == 17 && !draft.save_edit_armed && !error.message[0]);
  }
  s_success = false;
  CHECK(SaveEditor_ApplyConfirmedEdits(kSettingAction_SaveApplyPersist, &draft, &error) ==
        kSaveEditorAction_Failed);
  CHECK(s_apply_calls == ++calls && !strcmp(error.message, "Save fixture rejected the edit."));
  CHECK(SaveEditor_ApplyConfirmedEdits(kSettingAction_SaveApplyRestart, &draft, &error) ==
        kSaveEditorAction_Failed);
  CHECK(s_apply_calls == ++calls && !strcmp(error.message, "Save fixture rejected the edit."));
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveApplyRestart, &draft) ==
        kSaveEditorAction_Failed);
  CHECK(s_apply_calls == ++calls && !s_armed && s_persist);
  s_success = true;
  draft.save_edit_armed = true;
  CHECK(SaveEditor_HandleAction(kSettingAction_SaveApplyRestart, &draft) ==
        kSaveEditorAction_RestartRequired);
  CHECK(s_apply_calls == ++calls && s_armed && s_persist);
  CHECK(SaveEditor_ApplyConfirmedEdits(kSettingAction_SaveImport, &draft, &error) ==
        kSaveEditorAction_Failed);
  CHECK(error.message[0] && s_apply_calls == calls);
  CHECK(SaveEditor_ApplyConfirmedEdits(kSettingAction_SaveApplyPersist, NULL, &error) ==
        kSaveEditorAction_Failed);
  draft = (Settings){0};
  CHECK(SaveEditor_ApplyConfirmedEdits(kSettingAction_SaveApplyPersist, &draft, &error) ==
        kSaveEditorAction_Failed);
  CHECK(strstr(error.message, "Choose at least one") && s_apply_calls == calls);
  CHECK(SaveEditor_ApplyConfirmedEdits(kSettingAction_SaveApplyRestart, &draft, &error) ==
        kSaveEditorAction_Failed);
  CHECK(s_apply_calls == calls);
}

int main(void) {
  TestStagedValues();
  TestActions();
  TestPickedFiles();
  TestConfirmedEdits();
  return s_failures ? 1 : 0;
}
