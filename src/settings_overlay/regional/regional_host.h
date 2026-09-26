#ifndef AR_REGIONAL_HOST_H
#define AR_REGIONAL_HOST_H

#include <stdbool.h>

/* Regional settings' host adapter. The menu receives copied rules and guarded
 * edits; game-side continuation/population requests open localized dialogs and
 * yield to the host until the player decides. */
void SettingsOverlayRegionalHost_InstallHooks(void);
/* After campaign initialization (which clears the game's prompt callbacks).
 * Headless runs refuse prompts without opening or waiting on an invisible UI. */
void SettingsOverlayRegionalHost_InstallPrompts(bool headless);

#endif
