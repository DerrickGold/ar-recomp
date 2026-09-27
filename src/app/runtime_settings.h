#ifndef AR_RUNTIME_SETTINGS_H
#define AR_RUNTIME_SETTINGS_H
/* RuntimeSettings: settings that act on the running game. Installs the
 * settings observers, dispatches feature actions and
 * reports the restart or shutdown a setting asked for.
 * Phase: host (main thread). */

#include <stdbool.h>

#include "app/settings.h"

typedef enum RuntimeLifecycleRequest {
  kRuntimeLifecycle_None,
  kRuntimeLifecycle_Restart,
  kRuntimeLifecycle_Exit,
} RuntimeLifecycleRequest;

/* Starts a session's lifecycle requests and installs its settings observers
 * after the renderer and overlay exist. */
void RuntimeSettings_BeginSession(void);
/* Detaches callbacks before their session-owned targets are torn down. */
void RuntimeSettings_EndSession(void);
bool RuntimeSettings_HandleAction(const SettingDesc *desc);

RuntimeLifecycleRequest RuntimeSettings_LifecycleRequest(void);
/* Caller has already durably staged and validated a slot restart. */
void RuntimeSettings_RequestPreparedRestart(void);

#endif /* AR_RUNTIME_SETTINGS_H */
