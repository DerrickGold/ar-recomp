#ifndef AR_SCHEDULED_SETTINGS_H
#define AR_SCHEDULED_SETTINGS_H

/* Parse two diagnostic changes. The second pair has a `_2` suffix; action
 * values use `=run`. */
#include <stdbool.h>
void ScheduledSettings_Init(void);
/* Read only, on the runner owner; apply only after producer acknowledgement. */
bool ScheduledSettings_IsDue(void);

/* Apply the configured setting once its logical game-frame target is reached. */
void ScheduledSettings_ApplyIfDue(void);

#endif /* AR_SCHEDULED_SETTINGS_H */
