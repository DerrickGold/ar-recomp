#ifndef AR_SIM_MENU_MODEL_H
#define AR_SIM_MENU_MODEL_H

#include <stdbool.h>
#include <stdint.h>

enum { kSimMenuCategoryCount = 6, kSimMenuActionCount = 15,
       kSimMenuInventoryCapacity = 8 };

typedef enum SimMenuPhase {
  kSimMenu_Closed, kSimMenu_Browse, kSimMenu_Inventory,
  kSimMenu_Describe, kSimMenu_Confirm, kSimMenu_Native,
  /* Native execution can retain modern presentation, or hide the departing
   * town menu without drawing a replacement during a selector/scene handoff. */
  kSimMenu_Dialogue, kSimMenu_MessageSpeed, kSimMenu_Handoff,
  /* Native opening-release wait: presentation is already ours, but native
   * input ownership continues until the audited browse entry is reached. */
  kSimMenu_Opening,
} SimMenuPhase;

typedef enum SimMenuInput {
  kSimMenuInput_Right = 1, kSimMenuInput_Left = 2,
  kSimMenuInput_Down = 4, kSimMenuInput_Up = 8,
  kSimMenuInput_Describe = 16, kSimMenuInput_Back = 64,
  kSimMenuInput_Use = 128,
} SimMenuInput;

typedef enum SimMenuEvent {
  kSimMenuEvent_None, kSimMenuEvent_Changed, kSimMenuEvent_Close,
  kSimMenuEvent_Use, kSimMenuEvent_Describe, kSimMenuEvent_Advance,
} SimMenuEvent;

typedef struct SimMenuAction {
  const char *semantic_id;
  uint8_t category, row;
  uint16_t selection_pointer;
} SimMenuAction;

/* Owned by the game fiber. The presenter receives a value copy, never pointers
 * into WRAM or a second controller. Item identity includes its native slot. */
typedef struct SimMenuModel {
  uint64_t generation;
  SimMenuPhase phase, return_phase;
  uint8_t category, row[kSimMenuCategoryCount];
  uint8_t items[kSimMenuInventoryCapacity], item_count, item_slot;
  uint16_t miracle_sp[5]; /* published quote; Lightning/Rain/Sun/Wind/Quake */
  bool submenu, yes;
  /* Dialogue that introduces a pending Yes/No or number selector keeps the
   * modern modal presentation while the native interpreter reveals it. */
  bool dialogue_has_selector;
  /* Identifies one presentation session; native pages never change it. */
  uint32_t dialogue_generation;
  uint16_t dialogue_source;
  uint8_t held;
  uint8_t repeat_key, repeat_ticks;
} SimMenuModel;

extern const SimMenuAction kSimMenuActions[kSimMenuActionCount];
extern const uint16_t kSimMenuCategoryPointers[kSimMenuCategoryCount];
extern const uint8_t kSimMenuCategoryActionCounts[kSimMenuCategoryCount];

void SimMenuModel_Open(SimMenuModel *menu, uint64_t generation,
                       uint16_t native_selection);
void SimMenuModel_Inventory(SimMenuModel *menu, const uint8_t items[8]);
uint8_t SimMenuModel_Action(const SimMenuModel *menu);
uint16_t SimMenuModel_Selection(const SimMenuModel *menu);
/* Poll once per native game frame. Directions repeat after 18 frames, then
 * every 6 frames, only in Browse/Inventory. Actions never repeat.
 * A fresh release is required after every phase change. Simultaneous Back
 * wins over Use/Describe; opening or closing help can never dispatch Use. */
SimMenuEvent SimMenuModel_Poll(SimMenuModel *menu, uint8_t buttons);
void SimMenuModel_Confirm(SimMenuModel *menu);
void SimMenuModel_EndDescription(SimMenuModel *menu);
void SimMenuModel_ReleaseBarrier(SimMenuModel *menu);

#endif
