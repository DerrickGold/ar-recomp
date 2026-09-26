#ifndef AR_ACTRAISER_STATUE_VOLLEY_H
#define AR_ACTRAISER_STATUE_VOLLEY_H
/* ActRaiser statue-volley hooks: the statue enemy's volley wind-up and repeat
 * under the regional statue-volley rules.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_statue_volley_test.c */
#include "snesrecomp/game/cpu.h"
#include <stdbool.h>
bool ActRaiser_StatueVolleyBeginEntry(CpuState *cpu);
RecompReturn ActRaiser_StatueVolleyBegin(CpuState *cpu);
bool ActRaiser_StatueVolleyRepeatEntry(CpuState *cpu);
RecompReturn ActRaiser_StatueVolleyRepeat(CpuState *cpu);
#endif
