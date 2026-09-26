#ifndef SCHEDULED_SETTINGS_H
#define SCHEDULED_SETTINGS_H

/* Parse two diagnostic changes. The second pair has a `_2` suffix; action
 * values use `=run`. */
void ScheduledSettings_Init(void);

/* Apply the configured setting once its logical game-frame target is reached. */
void ScheduledSettings_ApplyIfDue(void);

#endif /* SCHEDULED_SETTINGS_H */
