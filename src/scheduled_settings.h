#ifndef SCHEDULED_SETTINGS_H
#define SCHEDULED_SETTINGS_H

/* Parse the optional AR_SETTING_SET and AR_SETTING_AT_GF diagnostic controls. */
void ScheduledSettings_Init(void);

/* Apply the configured setting once its logical game-frame target is reached. */
void ScheduledSettings_ApplyIfDue(void);

#endif /* SCHEDULED_SETTINGS_H */
