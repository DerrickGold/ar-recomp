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
#endif
