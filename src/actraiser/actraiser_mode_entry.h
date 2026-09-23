#ifndef ACTRAISER_MODE_ENTRY_H
#define ACTRAISER_MODE_ENTRY_H
#include "snesrecomp/game/cpu.h"
bool ActRaiser_ModeTitleGateEntry(CpuState *cpu);
RecompReturn ActRaiser_ModeTitleGate(CpuState *cpu);
bool ActRaiser_ModeLateMenuEntry(CpuState *cpu);
RecompReturn ActRaiser_ModeLateMenu(CpuState *cpu);
bool ActRaiser_ModeInitialLabelEntry(CpuState *cpu);
RecompReturn ActRaiser_ModeInitialLabel(CpuState *cpu);
bool ActRaiser_ModeInitialChoiceEntry(CpuState *cpu);
RecompReturn ActRaiser_ModeInitialChoice(CpuState *cpu);
bool ActRaiser_ModeNextChoiceEntry(CpuState *cpu);
RecompReturn ActRaiser_ModeNextChoice(CpuState *cpu);
bool ActRaiser_ModeGameOverEntry(CpuState *cpu);
RecompReturn ActRaiser_ModeGameOver(CpuState *cpu);
bool ActRaiser_ModeReturnTargetEntry(CpuState *cpu);
RecompReturn ActRaiser_ModeReturnTarget(CpuState *cpu);
bool ActRaiser_ModeReturnRootEntry(CpuState *cpu);
RecompReturn ActRaiser_ModeReturnRoot(CpuState *cpu);
#endif
