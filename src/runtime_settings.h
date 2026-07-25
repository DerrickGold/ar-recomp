#ifndef RUNTIME_SETTINGS_H
#define RUNTIME_SETTINGS_H

#include <stdbool.h>

#include "settings.h"

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

#endif /* RUNTIME_SETTINGS_H */
