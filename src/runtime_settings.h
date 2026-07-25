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

typedef struct RuntimeSettingsCallbacks {
  void (*perform_warp)(void);
  void (*take_full_snapshot)(void);
  bool (*dump_scene_assets)(void);
} RuntimeSettingsCallbacks;

/* Installs the settings observers after the renderer and overlay exist.
 * Callback targets remain owned by main.c because they assemble live
 * developer-tool resources at the point of use. */
void RuntimeSettings_Install(const RuntimeSettingsCallbacks *callbacks);
bool RuntimeSettings_HandleAction(const SettingDesc *desc);
bool RuntimeSettings_BuildSaveEditRequest(SaveEditRequest *edits);

RuntimeLifecycleRequest RuntimeSettings_LifecycleRequest(void);

#endif /* RUNTIME_SETTINGS_H */
