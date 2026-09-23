#ifndef ACTRAISER_TREE_ATTACK_H
#define ACTRAISER_TREE_ATTACK_H
#include "snesrecomp/game/cpu.h"
#include <stdbool.h>
bool ActRaiser_TreePrepareEntry(CpuState *cpu);
RecompReturn ActRaiser_TreePrepare(CpuState *cpu);
bool ActRaiser_TreeControllerEntry(CpuState *cpu);
RecompReturn ActRaiser_TreeController(CpuState *cpu);
#endif
