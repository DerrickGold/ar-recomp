#ifndef ACTRAISER_BOSS_RULES_H
#define ACTRAISER_BOSS_RULES_H
#include "snesrecomp/game/cpu.h"
bool ActRaiser_WizardPauseEntry(CpuState *cpu);
RecompReturn ActRaiser_WizardPause(CpuState *cpu);
bool ActRaiser_MinotaurAxeOffsetEntry(CpuState *cpu);
RecompReturn ActRaiser_MinotaurAxeOffset(CpuState *cpu);
bool ActRaiser_TanzraClockEntry(CpuState *cpu);
RecompReturn ActRaiser_TanzraClock(CpuState *cpu);
bool ActRaiser_AntlionTriggerEntry(CpuState *cpu);
RecompReturn ActRaiser_AntlionTrigger(CpuState *cpu);
bool ActRaiser_AntlionVolleyEntry(CpuState *cpu);
RecompReturn ActRaiser_AntlionVolley(CpuState *cpu);
bool ActRaiser_AntlionDecisionEntry(CpuState *cpu);
RecompReturn ActRaiser_AntlionDecision(CpuState *cpu);
bool ActRaiser_DragonFlightBeginEntry(CpuState *cpu);
RecompReturn ActRaiser_DragonFlightBegin(CpuState *cpu);
bool ActRaiser_DragonFlightRepeatEntry(CpuState *cpu);
RecompReturn ActRaiser_DragonFlightRepeat(CpuState *cpu);
bool ActRaiser_ViperChoiceEntry(CpuState *cpu);
RecompReturn ActRaiser_ViperChoice(CpuState *cpu);
bool ActRaiser_PharaohHeadIdleEntry(CpuState *cpu);
RecompReturn ActRaiser_PharaohHeadIdle(CpuState *cpu);
bool ActRaiser_PharaohHeadRepeatEntry(CpuState *cpu);
RecompReturn ActRaiser_PharaohHeadRepeat(CpuState *cpu);
bool ActRaiser_PlantPhaseEntry(CpuState *cpu);
RecompReturn ActRaiser_PlantPhase(CpuState *cpu);
#endif
