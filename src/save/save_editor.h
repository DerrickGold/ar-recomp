#ifndef AR_SAVE_EDITOR_H
#define AR_SAVE_EDITOR_H
/* Staged save controls, native save values and editor actions belong together.
 * The application owns the restart requested after a successful import. */
#include "app/settings.h"
#include "save/save_system.h"

/* Translate a settings snapshot into save edits. Returns whether any value
 * is staged; an empty draft still clears the output to leave-as-is sentinels. */
bool SaveEditor_BuildRequest(const Settings *settings, SaveEditRequest *edits);

typedef enum SaveEditorActionResult {
  kSaveEditorAction_Failed,
  kSaveEditorAction_Completed,
  kSaveEditorAction_RestartRequired,
} SaveEditorActionResult;

/* Apply/import/export through SaveSystem, preserving its validation and
 * durability checks. Unrecognized actions fail without touching a save. */
SaveEditorActionResult SaveEditor_HandleAction(SettingAction action,
                                              const Settings *settings);
/* The managed-slot menu has already confirmed the active destination. This
 * authorizes only this Apply action, without arming global boot overrides.
 * Return validation/storage errors to the menu instead of hiding them in logs. */
SaveEditorActionResult SaveEditor_ApplyConfirmedEdits(SettingAction action,
                                                     const Settings *settings,
                                                     SaveError *error);
/* Interactive file actions use the reviewed picker path, never diagnostic
 * environment variables or the import-folder fallback. Errors stay visible. */
const char *SaveEditor_ExportExtension(SettingAction action);
SaveEditorActionResult SaveEditor_HandleFileAction(SettingAction action, const char *path,
                                                  const Settings *settings, SaveError *error);
#endif
