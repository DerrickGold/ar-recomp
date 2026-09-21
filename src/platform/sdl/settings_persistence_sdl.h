#ifndef AR_SETTINGS_PERSISTENCE_SDL_H
#define AR_SETTINGS_PERSISTENCE_SDL_H
#include "settings.h"

typedef struct SettingsPersistence SettingsPersistence;
/* One destination per writer, at most one active and one pending snapshot.
 * Construction/destruction belong to the host lifecycle, never a render pass. */
SettingsPersistence *SettingsPersistence_Create(void);
SettingsSaveHost SettingsPersistence_Host(SettingsPersistence *writer);
/* Takes completed timings and their latest result once, without waiting for
 * disk I/O. Elapsed time is worker work (including synchronous requests), not
 * a separate measurement of main-thread stall time. */
typedef struct SettingsPersistenceReport {
  uint64_t writes, elapsed_ns, maximum_ns;
  bool failed;
} SettingsPersistenceReport;
SettingsPersistenceReport SettingsPersistence_TakeReport(SettingsPersistence *writer);
/* Drains and joins before returning. False means the latest write failed. */
bool SettingsPersistence_Destroy(SettingsPersistence *writer);
#endif
