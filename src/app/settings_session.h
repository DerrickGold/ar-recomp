#ifndef AR_SETTINGS_SESSION_H
#define AR_SETTINGS_SESSION_H
/* SettingsSession: settings.ini persistence for one running game. Owns the
 * background writer, replay protection, write reports and shutdown recovery.
 * The worker implementation lives in platform/sdl/settings_persistence_sdl.
 * Start after replay initialization; finish before any fatal-recovery dialog. */

#include <stdbool.h>

void SettingsSession_Start(void);
/* Collect metrics and retain the latest failure until the UI sees it or a
 * newer write succeeds. Taking the notification does not affect retry. */
void SettingsSession_PollWrites(void);
bool SettingsSession_TakeWriteFailure(void);
/* Drain the worker and retry the latest accepted snapshot if needed.
 * Without a worker, a fatal session retains the synchronous fallback.
 * Replays never write settings. Returns false if the final write fails. */
bool SettingsSession_Finish(bool fatal_session);

#endif /* AR_SETTINGS_SESSION_H */
