#ifndef AR_ACTRAISER_SIM_MENU_H
#define AR_ACTRAISER_SIM_MENU_H
/* ActRaiserSimMenu: the game adapter for the modern SIM menu. Drives the menu
 * model from native input, copies the model and help pages for the
 * presenter, and runs description dialogue.
 * Phase: game side.
 * Tests: tests/actraiser_sim_menu_test.c */

#include "sim/menu/sim_menu_model.h"
#include "sim/menu/sim_menu_help.h"
#include "snesrecomp/game/cpu.h"

bool ActRaiserSimMenu_OwnsInput(void);
bool ActRaiserSimMenu_OwnsPresentation(void);
void ActRaiserSimMenu_Reset(void);
bool ActRaiserSimMenu_ArtworkAvailable(void);
void ActRaiserSimMenu_CopyModel(SimMenuModel *model);
void ActRaiserSimMenu_CopyHelp(SimMenuHelpPage *page);
void ActRaiserSimMenu_ObserveScene(uint16_t scene);
void ActRaiserSimMenu_BeginDialogue(const CpuState *cpu);
void ActRaiserSimMenu_ClearDialogue(void);
bool ActRaiserSimMenu_SkipDialogue(const CpuState *cpu);
bool ActRaiserSimMenu_DescriptionAborted(void);
bool ActRaiserSimMenu_Describing(void);
bool ActRaiserSimMenu_FastReveal(void);
bool ActRaiser_SimMenuObserveFrame(CpuState *cpu);
void ActRaiserSimMenu_DescriptionWait(void);

bool ActRaiser_SimMenuBrowseEntry(CpuState *cpu);
RecompReturn ActRaiser_SimMenuBrowse(CpuState *cpu);
bool ActRaiser_SimMenuActionEntry(CpuState *cpu);
RecompReturn ActRaiser_SimMenuAction(CpuState *cpu);
bool ActRaiser_SimMenuInventoryEntry(CpuState *cpu);
RecompReturn ActRaiser_SimMenuInventory(CpuState *cpu);
bool ActRaiser_SimMenuConfirmEntry(CpuState *cpu);
RecompReturn ActRaiser_SimMenuConfirm(CpuState *cpu);
bool ActRaiser_SimMenuConfirmInputEntry(CpuState *cpu);
RecompReturn ActRaiser_SimMenuConfirmInput(CpuState *cpu);

#endif
