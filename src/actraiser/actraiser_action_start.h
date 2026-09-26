#ifndef AR_ACTRAISER_ACTION_START_H
#define AR_ACTRAISER_ACTION_START_H
/* ActRaiser action-start hook: the new-run initializer. Keeps the native
 * resets and applies the selected region's starting allowances.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_action_start_test.c */
#include "snesrecomp/game/cpu.h"
bool ActRaiser_ActionStartEntry(CpuState *cpu);
RecompReturn ActRaiser_ActionStart(CpuState *cpu);
#endif
