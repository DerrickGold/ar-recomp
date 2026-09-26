#ifndef AR_RUNTIME_SETTINGS_H
#define AR_RUNTIME_SETTINGS_H
/* RuntimeSettings: settings that act on the running game. Installs the
 * settings observers, runs action settings, builds save-edit requests and
 * reports the restart or shutdown a setting asked for.
 * Phase: host (main thread). */

#include <stdbool.h>

#include "app/settings.h"

typedef struct SaveEditRequest SaveEditRequest;

typedef enum RuntimeLifecycleRequest {
  kRuntimeLifecycle_None,
  kRuntimeLifecycle_Restart,
  kRuntimeLifecycle_Exit,
} RuntimeLifecycleRequest;

/* Installs the settings observers after the renderer and overlay exist. */
void RuntimeSettings_Install(void);
bool RuntimeSettings_HandleAction(const SettingDesc *desc);
bool RuntimeSettings_BuildSaveEditRequest(SaveEditRequest *edits);

RuntimeLifecycleRequest RuntimeSettings_LifecycleRequest(void);
/* Caller has already durably staged and validated a slot restart. */
void RuntimeSettings_RequestPreparedRestart(void);

#endif /* AR_RUNTIME_SETTINGS_H */
