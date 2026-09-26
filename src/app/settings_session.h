#ifndef AR_SETTINGS_SESSION_H
#define AR_SETTINGS_SESSION_H
/* SettingsSession: settings.ini persistence for one running game. Owns the
 * background writer, replay protection, write reports and shutdown recovery.
 * The worker implementation lives in platform/sdl/settings_persistence_sdl.
 * Start after replay initialization; finish before any fatal-recovery dialog. */

#include <stdbool.h>

void SettingsSession_Start(void);
void SettingsSession_PollWrites(void);
/* Drain the worker and retry a failed/fatal-session write synchronously.
 * Normal exits preserve session-only overrides; replays never write settings.
 * Returns false only when the final recovery write also fails. */
bool SettingsSession_Finish(bool fatal_session);

#endif /* AR_SETTINGS_SESSION_H */
