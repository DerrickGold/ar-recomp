#ifndef AR_HOST_LOCALIZATION_H
#define AR_HOST_LOCALIZATION_H
/* HostLocalization: the host resources behind localized and regional
 * presentation: the text backend and the font files it renders with, the
 * language-pack file host (and its notice when an installed pack needs an
 * upgrade), the settings overlay's Unicode interface fonts, and the regional
 * media donor files from game-assets/regions/.
 * Phase: host (main thread). */

#include <stdbool.h>

/* Publishes the installed language packs (game-assets/languages/packs) as the
 * settings' localization choices. Call before settings load. */
void HostLocalization_PublishInstalledPacks(void);
/* Installs the text backend, the font resources and the language-pack and
 * text-presentation hosts. Headless runs never show the upgrade notice. Call
 * before SettingsOverlay_Init. */
void HostLocalization_Install(bool headless);
/* Gives the settings overlay its Unicode interface fonts, keeping the native
 * interface when they are unavailable. Call after SettingsOverlay_Init. */
void HostLocalization_InstallInterfaceFonts(void);
/* Loads each installed regional media donor; a bad file keeps US graphics. */
void HostLocalization_LoadRegionalMedia(void);
/* The player chose Exit at the language-pack upgrade notice. */
bool HostLocalization_ExitRequested(void);
/* Unloads the regional media and detaches the localization runtime's hosts. */
void HostLocalization_Shutdown(void);
/* Releases the font resources, after everything that renders text is gone. */
void HostLocalization_ReleaseFonts(void);

/* Apply live settings here, beside the subsystem they configure. */
struct SettingDesc;
void HostLocalization_ApplySetting(const struct SettingDesc *desc);

#endif  /* AR_HOST_LOCALIZATION_H */
