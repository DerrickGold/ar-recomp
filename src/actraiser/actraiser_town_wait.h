#ifndef ACTRAISER_TOWN_WAIT_H
#define ACTRAISER_TOWN_WAIT_H

#include "snesrecomp/game/cpu.h"

/* US/PAL $03:872A..873B (JP $86DA..86EB). One state-3 service call,
 * not the SIM master-loop cadence. A running countdown always completes;
 * reload is used only on the native zero transition. */
bool ActRaiserTownWait_Entry(CpuState *cpu);
RecompReturn ActRaiserTownWait_Step(CpuState *cpu, uint16_t reload);

#endif
