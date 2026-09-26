#ifndef AR_REGIONAL_PANEL_H
#define AR_REGIONAL_PANEL_H

#include "settings_overlay/settings_overlay_internal.h"
#include "settings_overlay/regional/regional_menu.h"

/* Regional menu controller/presentation. The pure row model is regional_menu;
 * game/campaign integration is regional_host. All indices belong to the shell. */
void RegionalMenu_Open(void);
void RegionalMenu_Close(void);
void RegionalMenu_Refresh(void);
bool RegionalMenu_Available(void);
int RegionalMenu_Count(OverlayRegionPage page);
const char *RegionalMenu_Key(OverlayRegionPage page, int selected);
void RegionalMenu_Change(OverlayRegionPage page, int selected, int direction, bool reset,
                         bool activate);
void RegionalMenu_OpenDetails(OverlayRegionPage page, int selected);
bool RegionalMenu_HandleConfirmation(MenuNav nav, bool repeat);
bool RegionalMenu_DrawConfirmation(const MenuLayout *layout);
int RegionalMenu_DrawRows(const MenuLayout *layout, OverlayRegionPage page,
                          const MenuRowViewport *viewport);
void RegionalMenu_DrawDescription(const MenuLayout *layout, OverlayRegionPage page, int selected,
                                  int x, int y, int width);
void RegionalMenu_AddHints(MenuHints *hints, OverlayRegionPage page, const char *change,
                           const char *confirm, const char *tabs, const char *reset,
                           const char *details);

#endif
