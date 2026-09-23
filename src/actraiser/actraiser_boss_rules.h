#ifndef ACTRAISER_BOSS_RULES_H
#define ACTRAISER_BOSS_RULES_H
#include "snesrecomp/game/cpu.h"
bool ActRaiser_WizardPauseEntry(CpuState *cpu);
RecompReturn ActRaiser_WizardPause(CpuState *cpu);
bool ActRaiser_MinotaurAxeOffsetEntry(CpuState *cpu);
RecompReturn ActRaiser_MinotaurAxeOffset(CpuState *cpu);
bool ActRaiser_TanzraClockEntry(CpuState *cpu);
RecompReturn ActRaiser_TanzraClock(CpuState *cpu);
#endif
