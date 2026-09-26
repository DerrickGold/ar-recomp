#ifndef AR_LAYER_MENU_H
#define AR_LAYER_MENU_H

#include "settings_overlay/settings_overlay_internal.h"

/* Navigation belongs to the shell; editor state and row semantics stay here. */
void LayerMenu_ResetNavigation(void);
int LayerMenu_Count(int tab);
bool LayerMenu_RowExists(int tab, int selected);
bool LayerMenu_RowSelectable(int tab, int selected);
const char *LayerMenu_Key(int tab, int selected);
void LayerMenu_Change(int tab, int selected, int direction);
void LayerMenu_Activate(int tab, int selected);
void LayerMenu_Reset(int tab, int selected);
int LayerMenu_DrawRows(const MenuLayout *layout, int tab, const MenuRowViewport *viewport);
void LayerMenu_DrawDescription(const MenuLayout *layout, int tab, int selected, int x, int y,
                               int width);
void LayerMenu_AddHints(MenuHints *hints, int tab, int selected, bool multiple_tabs,
                        const char *change, const char *confirm, const char *tabs,
                        const char *reset);

#endif
