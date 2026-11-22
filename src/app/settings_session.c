#include "app/settings_session.h"
#include "app/runtime_log.h"

#include <stdio.h>

#include "app/input_replay.h"
#include "app/performance_metrics.h"
#include "app/settings.h"
#include "app/user_data_dir.h"
#include "constants.h"
#include "platform/sdl/settings_persistence_sdl.h"

static SettingsPersistence *s_settings_writer;
static bool s_write_failure_pending;

void SettingsSession_Start(void) {
  s_write_failure_pending = false;
  /* A replay must not mutate the player's configuration, for the same reason it
   * refuses to persist SRAM. Set from the same predicate so the two protections
   * cannot drift apart. */
  Settings_SetPersistenceEnabled(!InputReplay_ShouldProtectSaveData());
  if (!InputReplay_ShouldProtectSaveData()) {
    s_settings_writer = SettingsPersistence_Create();
    const SettingsSaveHost host = SettingsPersistence_Host(s_settings_writer);
    Settings_SetSaveHost(&host);
    if (!s_settings_writer)
      RuntimeLog_Printf("[settings] background writer unavailable; using durable synchronous saves\n");
  }
}

void SettingsSession_PollWrites(void) {
  const SettingsPersistenceReport written = SettingsPersistence_TakeReport(s_settings_writer);
  PerformanceMetrics_RecordBatch(PerformanceMetrics_Epoch(), kPerformance_SettingsWrite,
      written.elapsed_ns, written.maximum_ns, written.writes);
  if (written.failed)
    RuntimeLog_Printf("[settings] latest settings write failed; "
        "it will be retried by the next save or on exit\n");
  if (written.writes) s_write_failure_pending = written.failed;
}

bool SettingsSession_TakeWriteFailure(void) {
  const bool failed = s_write_failure_pending;
  s_write_failure_pending = false;
  return failed;
}

bool SettingsSession_Finish(bool fatal_session) {
  const bool had_writer = s_settings_writer != NULL;
  bool settings_save_ok = SettingsPersistence_Destroy(s_settings_writer);
  s_settings_writer = NULL;
  Settings_SetSaveHost(NULL);
  if (fatal_session && !had_writer && !InputReplay_ShouldProtectSaveData()) {
    char settings_path[kHostPathCapacity];
    UserDataFile(settings_path, sizeof(settings_path), "settings.ini");
    settings_save_ok = Settings_Save(settings_path);
  }
  if (!settings_save_ok)
    RuntimeLog_Printf("[settings] shutdown write failed; recent preferences may not have been saved\n");
  return settings_save_ok;
}
