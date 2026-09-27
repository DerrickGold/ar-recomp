#include "app/settings_session.h"
#include "app/input_replay.h"
#include "app/performance_metrics.h"
#include "app/settings.h"
#include "app/user_data_dir.h"
#include "platform/sdl/settings_persistence_sdl.h"

#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

struct SettingsPersistence { int unused; };
static SettingsPersistence s_writer;
static bool s_replay, s_create_ok, s_drain_ok, s_save_ok, s_enabled;
static unsigned s_created, s_saved, s_reports, s_order;

bool InputReplay_ShouldProtectSaveData(void) { return s_replay; }
void Settings_SetPersistenceEnabled(bool enabled) { s_enabled = enabled; }
SettingsPersistence *SettingsPersistence_Create(void) {
  ++s_created;
  return s_create_ok ? &s_writer : NULL;
}
SettingsSaveHost SettingsPersistence_Host(SettingsPersistence *writer) {
  return (SettingsSaveHost){.context = writer};
}
void Settings_SetSaveHost(const SettingsSaveHost *host) {
  if (!host) {
    assert(s_order == 1);  /* Join the worker before removing its host. */
    s_order = 2;
  }
}
bool SettingsPersistence_Destroy(SettingsPersistence *writer) {
  assert(writer == (s_created && s_create_ok ? &s_writer : NULL));
  s_order = 1;
  return s_drain_ok;
}
bool Settings_Save(const char *path) {
  assert(s_order == 2);  /* Recovery saves must use the synchronous fallback. */
  assert(!strcmp(path, "test/settings.ini"));
  ++s_saved;
  return s_save_ok;
}
char *UserDataFile(char *out, size_t capacity, const char *leaf) {
  snprintf(out, capacity, "test/%s", leaf);
  return out;
}
SettingsPersistenceReport SettingsPersistence_TakeReport(SettingsPersistence *writer) {
  assert(writer == &s_writer);
  return (SettingsPersistenceReport){.writes = 2, .elapsed_ns = 50, .maximum_ns = 30};
}
uint32_t PerformanceMetrics_Epoch(void) { return 7; }
void PerformanceMetrics_RecordBatch(uint32_t epoch, PerformanceStage stage,
                                    uint64_t elapsed, uint64_t maximum, uint64_t count) {
  assert(epoch == 7 && stage == kPerformance_SettingsWrite);
  assert(elapsed == 50 && maximum == 30 && count == 2);
  ++s_reports;
}

static void CheckSession(bool replay, bool create_ok, bool drain_ok, bool fatal,
                         bool save_ok, unsigned expected_saves, bool success) {
  s_replay = replay;
  s_create_ok = create_ok;
  s_drain_ok = drain_ok;
  s_save_ok = save_ok;
  s_created = s_saved = s_reports = s_order = 0;
  SettingsSession_Start();
  assert(s_enabled == !replay && s_created == (replay ? 0 : 1));
  if (!replay && create_ok) {
    SettingsSession_PollWrites();
    assert(s_reports == 1);
  }
  assert(SettingsSession_Finish(fatal) == success);
  assert(s_saved == expected_saves);
}

int main(void) {
  /* Ordinary exits must not persist transient overrides. */
  CheckSession(false, true, true, false, true, 0, true);
  CheckSession(false, false, true, false, true, 0, true);
  CheckSession(false, true, false, false, true, 1, true);
  CheckSession(false, true, false, false, false, 1, false);
  CheckSession(false, true, true, true, true, 1, true);
  CheckSession(false, true, true, true, false, 1, false);
  CheckSession(true, true, true, false, true, 0, true);
  CheckSession(true, true, false, true, false, 0, true);
  puts("settings session: replay isolation, drain ordering and recovery passed");
  return 0;
}
