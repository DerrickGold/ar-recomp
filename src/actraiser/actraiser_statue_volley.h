#ifndef ACTRAISER_STATUE_VOLLEY_H
#define ACTRAISER_STATUE_VOLLEY_H
#include "snesrecomp/game/cpu.h"
#include <stdbool.h>
bool ActRaiser_StatueVolleyBeginEntry(CpuState *cpu);
RecompReturn ActRaiser_StatueVolleyBegin(CpuState *cpu);
bool ActRaiser_StatueVolleyRepeatEntry(CpuState *cpu);
RecompReturn ActRaiser_StatueVolleyRepeat(CpuState *cpu);
#endif
