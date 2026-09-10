#ifndef AR_SETTINGS_OVERLAY_LOCALIZATION_H
#define AR_SETTINGS_OVERLAY_LOCALIZATION_H

#include "settings.h"
#include "localization/ui_catalog.h"

/* Presentation-only: no parsing/serialization, filesystem, or renderer objects.
 * Reads the input owner's display names and the host's live display status.
 * Stable descriptor IDs select translations. User pack names/IDs and direct
 * edit buffers must never be passed through an English-string lookup. */
const char *SettingsOverlay_LocalizedLabel(ArUiLocale locale,
                                         const SettingDesc *desc);
const char *SettingsOverlay_LocalizedHelp(ArUiLocale locale,
                                        const SettingDesc *desc);
int SettingsOverlay_LocalizedValue(ArUiLocale locale, const SettingDesc *desc,
                                   char *buffer, int capacity);

#endif
