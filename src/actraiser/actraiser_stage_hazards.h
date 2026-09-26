#ifndef AR_ACTRAISER_STAGE_HAZARDS_H
#define AR_ACTRAISER_STAGE_HAZARDS_H
/* ActRaiser stage-hazards hook: selects the stage's collision-hazard records
 * from the regional hazard rules, leaving the native cursor and continuation
 * in place.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_stage_hazards_test.c */
#include "snesrecomp/game/cpu.h"
bool ActRaiser_StageHazardsEntry(CpuState *cpu);
RecompReturn ActRaiser_StageHazards(CpuState *cpu);
#endif
