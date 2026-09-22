#ifndef ACTRAISER_SAVE_TRANSACTION_H
#define ACTRAISER_SAVE_TRANSACTION_H

#include "snesrecomp/game/cpu.h"

bool ActRaiser_SaveStoryEntry(CpuState *cpu);
RecompReturn ActRaiser_SaveStory(CpuState *cpu);

#endif
