#ifndef AR_SAVE_SLOT_MENU_H
#define AR_SAVE_SLOT_MENU_H
/* Save-slot UI owns reviewed slot copies, the new-game draft and confirmation
 * state. Navigation requests leave the containing overlay in charge of its
 * section/row/tab history. Tests: settings_overlay_test.c. */
#include <stdbool.h>
#include "settings_overlay/menu_input.h"

struct MenuLayout;
struct SettingDesc;
struct SettingsOverlaySaveSlotHooks;

typedef enum SaveSlotMenuNavResult {
  kSaveSlotMenuNav_Unhandled,
  kSaveSlotMenuNav_Handled,
  kSaveSlotMenuNav_OpenEditor,
  kSaveSlotMenuNav_ReturnToParent,
  kSaveSlotMenuNav_CloseOverlay,
} SaveSlotMenuNavResult;

void SaveSlotMenu_SetHooks(const struct SettingsOverlaySaveSlotHooks *hooks);
bool SaveSlotMenu_Available(void);
bool SaveSlotMenu_Open(bool randomized);
void SaveSlotMenu_Close(void);
void SaveSlotMenu_Refresh(void);
bool SaveSlotMenu_Active(void);
bool SaveSlotMenu_DecisionActive(void);
SaveSlotMenuNavResult SaveSlotMenu_HandleNav(MenuNav nav, bool repeat);
void SaveSlotMenu_Draw(const struct MenuLayout *layout);
const char *SaveSlotMenu_SelectedKey(void);
void SaveSlotMenu_TabState(int *active_tab, int *tab_count);

/* Advanced uses the overlay's settings rows, with the same reviewed active
 * slot. Returning rescans storage; confirming rejects stale slot fingerprints. */
bool SaveSlotMenu_ReturnFromEditor(void);
const char *SaveSlotMenu_EditorTitle(void);
/* Reuse the manager's slot column while the shell renders Advanced's rows. */
bool SaveSlotMenu_DrawEditorSidebar(const struct MenuLayout *layout, int width, int height);
bool SaveSlotMenu_EditorRowVisible(const struct SettingDesc *desc);
bool SaveSlotMenu_ConfirmEditorAction(const struct SettingDesc *desc);
#endif
